//! Sequential, bounded-memory input preparation outside the solver timer.
use fusion_blossom::util::{SolverInitializer, SyndromePattern, VertexIndex};
use serde_json::Value;
use std::fs::{self, File};
use std::io::{self, BufReader, Read, Seek, SeekFrom};
use std::path::{Component, Path};

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn number(value: &Value) -> io::Result<u64> {
    value.as_u64().ok_or_else(|| invalid("Sparse input fields must be nonnegative integers"))
}

fn read_u64(input: &mut impl Read) -> io::Result<u64> {
    let mut bytes = [0; 8];
    input.read_exact(&mut bytes)?;
    Ok(u64::from_le_bytes(bytes))
}

struct Sidecar {
    offsets: BufReader<File>,
    events: BufReader<File>,
    shots: usize,
    total_events: u64,
    event_start: u64,
    next_shot: usize,
    previous_offset: u64,
    map: Vec<VertexIndex>,
    bytes: Vec<u8>,
}

pub struct Samples {
    inline: Vec<SyndromePattern>,
    sidecar: Option<Sidecar>,
}

impl Samples {
    pub fn new(j: &Value, input: &Path, initializer: &SolverInitializer, warmup: bool) -> Result<Self, Box<dyn std::error::Error>> {
        let (storage_key, inline_key, mapped_key) = if warmup {
            ("warmup_storage", "warmup", "blossom_warmup")
        } else {
            ("sample_storage", "samples", "blossom_samples")
        };
        if j.get(storage_key).is_none() {
            let samples = j[mapped_key].as_array().ok_or_else(|| invalid("Missing inline blossom samples"))?;
            if samples.is_empty() { return Err(invalid("Empty workload").into()); }
            let mut inline = Vec::with_capacity(samples.len());
            for sample in samples {
                let vertices: Vec<VertexIndex> = serde_json::from_value(sample.clone())?;
                let mut seen = std::collections::HashSet::new();
                for &v in &vertices {
                    if v >= initializer.vertex_num || initializer.virtual_vertices.contains(&v) || !seen.insert(v) {
                        return Err(invalid("Invalid or duplicate inline blossom detector").into());
                    }
                }
                inline.push(SyndromePattern::new_vertices(vertices));
            }
            return Ok(Self { inline, sidecar: None });
        }
        if j.get(inline_key).is_some() || j.get(mapped_key).is_some() {
            return Err(invalid("Ambiguous inline and sidecar samples").into());
        }
        let storage = &j[storage_key];
        if storage["format"] != "bb-sparse-u64-v1" {
            return Err(invalid("Unsupported sparse sample format").into());
        }
        let name = Path::new(storage["file"].as_str().ok_or_else(|| invalid("Missing sidecar filename"))?);
        let mut components = name.components();
        if !matches!(components.next(), Some(Component::Normal(_))) || components.next().is_some() {
            return Err(invalid("Sample sidecar must be a sibling filename").into());
        }
        let path = input.parent().unwrap_or(Path::new(".")).join(name);
        let metadata = fs::symlink_metadata(&path)?;
        if !metadata.file_type().is_file() {
            return Err(invalid("Sample sidecar must be a regular non-symlink file").into());
        }
        let mut offsets = BufReader::new(File::open(&path)?);
        if read_u64(&mut offsets)? != 0x4242535041525331 || read_u64(&mut offsets)? != 1 {
            return Err(invalid("Invalid sparse sample magic or version").into());
        }
        let shots = read_u64(&mut offsets)?;
        let total_events = read_u64(&mut offsets)?;
        if shots == 0 || shots != number(&storage["shots"])? || total_events != number(&storage["total_events"])? {
            return Err(invalid("Sparse sample metadata differs from header").into());
        }
        let event_start = shots.checked_mul(8).and_then(|n| n.checked_add(40))
            .ok_or_else(|| invalid("Sparse sample offsets overflow"))?;
        let file_size = total_events.checked_mul(8).and_then(|n| n.checked_add(event_start))
            .ok_or_else(|| invalid("Sparse sample size overflow"))?;
        if file_size != metadata.len() { return Err(invalid("Sparse sample file size differs from header").into()); }
        let shots = usize::try_from(shots)?;
        let detectors = number(&j["num_detectors"])?;
        let map: Vec<VertexIndex> = serde_json::from_value(j["blossom_vertex_map"].clone())?;
        if map.is_empty() || u64::try_from(map.len())? != detectors {
            return Err(invalid("Blossom vertex map differs from detector count").into());
        }
        let mut seen = vec![false; initializer.vertex_num as usize];
        for &v in &initializer.virtual_vertices { seen[v as usize] = true; }
        for &v in &map {
            if v >= initializer.vertex_num || seen[v as usize] {
                return Err(invalid("Blossom vertex map is not bounded, unique and nonvirtual").into());
            }
            seen[v as usize] = true;
        }
        let mut previous = read_u64(&mut offsets)?;
        if previous != 0 { return Err(invalid("First sparse sample offset must be zero").into()); }
        for _ in 0..shots {
            let current = read_u64(&mut offsets)?;
            if current < previous || current > total_events || current - previous > detectors {
                return Err(invalid("Invalid sparse sample offset or shot length").into());
            }
            previous = current;
        }
        if previous != total_events { return Err(invalid("Final sparse sample offset differs from event count").into()); }
        offsets.seek(SeekFrom::Start(40))?;
        let mut events = BufReader::new(File::open(&path)?);
        events.seek(SeekFrom::Start(event_start))?;
        Ok(Self { inline: Vec::new(), sidecar: Some(Sidecar {
            offsets, events, shots, total_events, event_start, next_shot: 0, previous_offset: 0, map, bytes: Vec::new(),
        }) })
    }

    pub fn len(&self) -> usize {
        match &self.sidecar { Some(s) => s.shots, None => self.inline.len() }
    }

    pub fn load(&mut self, shot: usize) -> io::Result<SyndromePattern> {
        if shot >= self.len() { return Err(invalid("Sparse sample index out of range")); }
        let s = match &mut self.sidecar {
            Some(s) => s,
            None => return Ok(SyndromePattern::new_vertices(self.inline[shot].defect_vertices.clone())),
        };
        if shot != s.next_shot {
            s.offsets.seek(SeekFrom::Start(32 + shot as u64 * 8))?;
            s.previous_offset = read_u64(&mut s.offsets)?;
            s.events.seek(SeekFrom::Start(s.event_start + s.previous_offset * 8))?;
        }
        let end = read_u64(&mut s.offsets)?;
        if end < s.previous_offset || end > s.total_events || end - s.previous_offset > s.map.len() as u64 {
            return Err(invalid("Sparse sample offsets changed or are invalid"));
        }
        let length = usize::try_from(end - s.previous_offset).map_err(|_| invalid("Sparse shot exceeds addressable memory"))?;
        s.bytes.resize(length.checked_mul(8).ok_or_else(|| invalid("Sparse shot size overflow"))?, 0);
        s.events.read_exact(&mut s.bytes)?;
        let mut vertices = Vec::with_capacity(length);
        let mut previous = None;
        for bytes in s.bytes.chunks_exact(8) {
            let detector = u64::from_le_bytes(bytes.try_into().unwrap());
            if detector >= s.map.len() as u64 || previous.map_or(false, |v| detector <= v) {
                return Err(invalid("Sample detector IDs must be bounded, sorted and unique"));
            }
            // Preserve the historical mapped order; sorting would change tie traversal.
            vertices.push(s.map[detector as usize]);
            previous = Some(detector);
        }
        s.previous_offset = end;
        s.next_shot = shot + 1;
        Ok(SyndromePattern::new_vertices(vertices))
    }
}
