#!/usr/bin/env python3
"""Convert frozen AE sparse inputs to the historical external native graph format.

Graph construction and input conversion are outside every decoder timing window.
No decoder is called here. The sparse input's oracle weights belong to a different
quantization than the external graph and are never presented as its optimum.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from typing import Iterator

try:
    from .common import SPARSE_MAGIC, SPARSE_HEADER, SPARSE_FORMAT
except ImportError:
    from common import SPARSE_MAGIC, SPARSE_HEADER, SPARSE_FORMAT

MAGIC = 0x5354454E43494C31
MASK64 = (1 << 64) - 1
HEADER = struct.Struct('<12Q')
U64 = struct.Struct('<Q')
I64 = struct.Struct('<q')


def sha256(path: Path) -> str:
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def mix(state: int, value: int) -> int:
    state ^= (value + 0x9E3779B97F4A7C15 + ((state << 6) & MASK64) + (state >> 2)) & MASK64
    state ^= state >> 30
    state = (state * 0xBF58476D1CE4E5B9) & MASK64
    state ^= state >> 27
    state = (state * 0x94D049BB133111EB) & MASK64
    return state ^ (state >> 31)


class SparseDataset:
    """Read the version-2/3 little-endian native AE format without dense expansion."""

    def __init__(self, path: Path):
        self.path = Path(path).resolve()
        with self.path.open('rb') as stream:
            raw = stream.read(HEADER.size)
        if len(raw) != HEADER.size:
            raise ValueError('Truncated sparse dataset header')
        self.header = HEADER.unpack(raw)
        h = self.header
        if h[0] != MAGIC or h[1] not in (2, 3):
            raise ValueError('Unknown sparse dataset format')
        self.version, self.distance, self.shots, self.num_detectors = h[1], h[2], h[4], h[5]
        self.error_rate = struct.unpack('<d', struct.pack('<Q', h[3]))[0]
        self.integer_scale = struct.unpack('<d', struct.pack('<Q', h[10]))[0]
        if self.distance < 3 or self.distance % 2 == 0 or not self.shots or not self.num_detectors:
            raise ValueError('Invalid sparse dataset dimensions')
        if not math.isfinite(self.error_rate) or not 0 < self.error_rate < .5:
            raise ValueError('Invalid sparse dataset error rate')
        if not math.isfinite(self.integer_scale) or self.integer_scale <= 0:
            raise ValueError('Invalid sparse dataset integer scale')
        self.verified_sha256: str | None = None

    def records(self, limit: int | None = None) -> Iterator[tuple[int, list[int], int, int]]:
        """Yield selected records; still verify all source records after the prefix."""
        if limit is not None and (limit <= 0 or limit > self.shots):
            raise ValueError('Record limit must be between one and the saved shot count')
        selected = self.shots if limit is None else limit
        syndrome_hash = result_hash = event_count = 0
        original_ids: set[int] = set()
        digest = hashlib.sha256()
        with self.path.open('rb') as stream:
            def read(count):
                data = stream.read(count)
                if len(data) != count:
                    raise ValueError('Truncated sparse dataset record')
                digest.update(data)
                return data
            if HEADER.unpack(read(HEADER.size)) != self.header:
                raise ValueError('Sparse dataset header changed during conversion')
            for index in range(self.shots):
                shot_id = U64.unpack(read(8))[0] if self.version == 3 else index
                if self.version == 3:
                    if shot_id in original_ids:
                        raise ValueError('Duplicate original shot ID')
                    original_ids.add(shot_id)
                    syndrome_hash = mix(syndrome_hash, shot_id)
                count = U64.unpack(read(8))[0]
                if count > self.num_detectors:
                    raise ValueError('Sparse record exceeds detector count')
                hits = list(struct.unpack(f'<{count}Q', read(8 * count)))
                if any(hit >= self.num_detectors for hit in hits) or any(a >= b for a, b in zip(hits, hits[1:])):
                    raise ValueError('Sparse detector IDs must be bounded, sorted and unique')
                observable = U64.unpack(read(8))[0]
                weight = I64.unpack(read(8))[0]
                event_count += count
                syndrome_hash = mix(syndrome_hash, count)
                for hit in hits:
                    syndrome_hash = mix(syndrome_hash, hit)
                result_hash = mix(mix(result_hash, observable), weight & MASK64)
                if index < selected:
                    yield shot_id, hits, observable, weight
            if stream.read(1):
                raise ValueError('Unexpected trailing sparse dataset bytes')
        if (event_count, syndrome_hash, result_hash) != (self.header[6], self.header[7], self.header[8]):
            raise ValueError('Sparse dataset count or checksum mismatch')
        actual_sha256 = digest.hexdigest()
        if self.verified_sha256 is not None and self.verified_sha256 != actual_sha256:
            raise ValueError('Sparse dataset changed between conversion passes')
        self.verified_sha256 = actual_sha256

    def identity(self) -> dict:
        if self.verified_sha256 is None:
            raise ValueError('Sparse dataset has not been completely verified')
        return dict(path=str(self.path), sha256=self.verified_sha256, format_version=self.version,
                    distance=self.distance, error_rate=self.error_rate, saved_shots=self.shots,
                    num_detectors=self.num_detectors, oracle_integer_scale=self.integer_scale,
                    stream_id=self.header[11], mixed_seed=self.header[9])


def export_graph(dem_path: Path) -> dict:
    """Retain the original independent-edge merge and even-integer quantization."""
    import beyond_blossom as bb
    import stim

    dem = stim.DetectorErrorModel.from_file(str(dem_path))
    if dem.num_observables > 64:
        raise ValueError('External adapters support at most 64 observables')
    labels: dict[tuple[int, ...], tuple[int, ...]] = {}
    detector_free = 0
    for instruction in dem.flattened():
        if instruction.type != 'error':
            continue
        groups = [[]]
        for target in instruction.targets_copy():
            if target.is_separator():
                groups.append([])
            else:
                groups[-1].append(target)
        for group in groups:
            ds = tuple(sorted(t.val for t in group if t.is_relative_detector_id()))
            ls = tuple(sorted(t.val for t in group if t.is_logical_observable_id()))
            if len(ds) > 2:
                raise ValueError('Undecomposed hyperedge cannot enter the external graph')
            if not ds:
                detector_free += 1
                continue
            if ds in labels and labels[ds] != ls:
                raise ValueError(f'Conflicting parallel logical labels: {ds}')
            labels[ds] = ls
    matching = bb.Matching.from_detector_error_model(dem)
    edges = []
    for u, v, attrs in matching.edges():
        if v is not None and u > v:
            u, v = v, u
        weight = float(attrs['weight'])
        if not math.isfinite(weight) or weight <= 0:
            raise ValueError('External graph requires finite strictly positive weights')
        mask = sum(1 << int(k) for k in attrs['fault_ids'])
        edges.append([int(u), -1 if v is None else int(v), weight, mask])
    edges.sort(key=lambda edge: (edge[0], edge[1]))
    if not edges:
        raise ValueError('External graph has no edges')
    scale = 10000.0 / max(edge[2] for edge in edges)
    for edge in edges:
        edge.append(2 * max(1, round(edge[2] * scale)))
    coordinates = dem.get_detector_coordinates()
    if any(i not in coordinates or len(coordinates[i]) < 3 for i in range(dem.num_detectors)):
        raise ValueError('Every detector needs three coordinates for the time partition')
    ordered_coordinates = [coordinates[i] for i in range(dem.num_detectors)]
    if any(not all(math.isfinite(x) for x in c) for c in ordered_coordinates):
        raise ValueError('Nonfinite detector coordinates')
    converter_files = [Path(bb.__file__).with_name('matching.py'),
                       Path(sys.modules['beyond_blossom._cpp_beyond_blossom'].__file__)]
    return dict(num_detectors=dem.num_detectors, num_observables=dem.num_observables,
                edge_fields=['u', 'v_or_minus_one', 'log_odds_weight', 'observable_mask', 'even_weight'],
                edges=edges, integer_scale=2 * scale, detector_free_components=detector_free,
                detector_coordinates=ordered_coordinates,
                graph_policy='non-correlated decomposed DEM; independent parallel-edge merge',
                quantization='2 * max(1, Python round(weight * 10000 / max_log_odds_weight))',
                converter_version=bb.__version__, stim_version=stim.__version__,
                converter_artifacts={p.name: sha256(p) for p in converter_files})


def partition_graph(graph: dict, distance: int) -> tuple[dict, dict, list[int]]:
    """Create private boundary vertices and the original middle-time fusion gap."""
    m = graph['num_detectors']
    coordinates = graph['detector_coordinates']
    if len(coordinates) != m or any(len(c) < 3 for c in coordinates):
        raise ValueError('Detector coordinates do not cover the graph')
    times = [c[2] for c in coordinates]
    if any(not math.isfinite(t) for t in times):
        raise ValueError('Invalid detector time')
    endpoints = []
    virtuals = []
    for edge in graph['edges']:
        u, v = edge[:2]
        if not 0 <= u < m or v < -1 or v >= m or u == v:
            raise ValueError('Invalid external graph endpoint')
        if v < 0:
            v = len(times)
            times.append(times[u])
            virtuals.append(v)
        endpoints.append((u, v))
    order = sorted(range(len(times)), key=lambda vertex: (times[vertex], vertex))
    inverse = [0] * len(order)
    for index, vertex in enumerate(order):
        inverse[vertex] = index
    cut = distance // 2
    left = sum(t < cut for t in times)
    right = sum(t <= cut for t in times)
    if not 0 < left < right < len(times):
        raise ValueError('Invalid middle-time fusion partition')
    if any(min(times[u], times[v]) < cut < max(times[u], times[v]) for u, v in endpoints):
        raise ValueError('Edge bypasses the Fusion interface')
    initializer = dict(vertex_num=len(times), virtual_vertices=[inverse[v] for v in virtuals],
                       weighted_edges=[[inverse[u], inverse[v], edge[4]]
                                       for (u, v), edge in zip(endpoints, graph['edges'])])
    partition = dict(vertex_num=len(times), partitions=[[0, left], [right, len(times)]], fusions=[[0, 1]])
    return initializer, partition, inverse


def write_sparse_samples(saved, selected, path, references=None):
    """Write an indexed sidecar in one verified pass with one-shot event memory."""
    events = shots = 0
    with Path(path).open('xb') as data:
        data.write(SPARSE_HEADER.pack(SPARSE_MAGIC, 1, selected, 0))
        data.flush()
        data.seek(SPARSE_HEADER.size + 8 * (selected + 1))
        with Path(path).open('r+b') as offsets:
            offsets.seek(SPARSE_HEADER.size)
            offsets.write(U64.pack(0))
            for index, (shot_id, hits, observable, weight) in enumerate(saved.records(selected)):
                data.write(struct.pack(f'<{len(hits)}Q', *hits))
                events += len(hits)
                offsets.write(U64.pack(events))
                shots += 1
                if references is not None:
                    references.write(json.dumps(dict(execution_index=index, original_shot_id=shot_id,
                        source_oracle_observable=observable, source_oracle_integer_weight=weight),
                        separators=(',', ':')) + '\n')
            if shots != selected:
                raise ValueError('Sparse conversion emitted an incomplete shot sequence')
            data.flush()
            offsets.seek(0)
            offsets.write(SPARSE_HEADER.pack(SPARSE_MAGIC, 1, selected, events))
    return dict(format=SPARSE_FORMAT, file=Path(path).name, shots=shots, total_events=events)


def copy_sparse_prefix(source, target, selected):
    """Copy the verified measurement prefix as warmup with a fixed byte buffer."""
    with Path(source).open('rb') as src, Path(target).open('xb') as dst:
        magic, version, shots, events = SPARSE_HEADER.unpack(src.read(SPARSE_HEADER.size))
        if magic != SPARSE_MAGIC or version != 1 or not 0 < selected <= shots:
            raise ValueError('Invalid sparse warmup prefix')
        src.seek(SPARSE_HEADER.size + 8 * selected)
        selected_events = U64.unpack(src.read(8))[0]
        if selected_events > events:
            raise ValueError('Invalid sparse warmup prefix offset')
        dst.write(SPARSE_HEADER.pack(magic, version, selected, selected_events))
        for start, length in [(SPARSE_HEADER.size, 8 * (selected + 1)),
                              (SPARSE_HEADER.size + 8 * (shots + 1), 8 * selected_events)]:
            src.seek(start)
            while length:
                block = src.read(min(length, 1 << 20))
                if not block:
                    raise ValueError('Truncated sparse warmup prefix')
                dst.write(block)
                length -= len(block)
    return dict(format=SPARSE_FORMAT, file=Path(target).name, shots=selected, total_events=selected_events)


def convert(dataset: Path, dem: Path, output: Path, *, repeats: int = 3,
            warmup_seconds: float = 2, limit: int | None = None,
            warmup_dataset: Path | None = None, warmup_prefix: int = 256) -> dict:
    """Write small native JSON and sparse sidecars; refuse reuse and partial inputs."""
    output, dem = Path(output).resolve(), Path(dem).resolve()
    if output.exists():
        raise FileExistsError('Use a new external input directory')
    if repeats <= 0 or not math.isfinite(warmup_seconds) or warmup_seconds < 0 or warmup_prefix <= 0:
        raise ValueError('Invalid repeats or warm-up configuration')
    source = SparseDataset(dataset)
    graph = export_graph(dem)
    if source.num_detectors != graph['num_detectors']:
        raise ValueError('Saved inputs and DEM detector counts differ')
    source_dem = source.path.parent / 'model.dem'
    if source_dem.is_file() and sha256(source_dem) != sha256(dem):
        raise ValueError('DEM differs from the saved input companion model')
    count = source.shots if limit is None else limit
    if not 0 < count <= source.shots:
        raise ValueError('Selected shot count exceeds the saved dataset')
    initializer, partition, inverse = partition_graph(graph, source.distance)
    warm = SparseDataset(warmup_dataset) if warmup_dataset else source
    if warmup_dataset and warm.path == source.path:
        raise ValueError('Independent warm-up must use a separate saved dataset')
    warm_count = warm.shots if warmup_dataset else min(warmup_prefix, count)
    if (warm.num_detectors, warm.distance, warm.header[3], warm.header[10]) != (
            source.num_detectors, source.distance, source.header[3], source.header[10]):
        raise ValueError('Warm-up dataset and measured dataset graph identities differ')
    if warmup_dataset:
        companion = warm.path.parent / 'model.dem'
        if not companion.is_file() or sha256(companion) != sha256(dem):
            raise ValueError('Independent warm-up inputs need the identical companion model.dem')
    output.mkdir(parents=True)
    graph_path = output / 'graph.json'
    graph_path.write_text(json.dumps(graph, indent=2, allow_nan=False) + '\n')
    payload_path = output / 'native-input.json'
    references = output / 'shot-reference.jsonl'
    try:
        sample_path, warm_path = output / 'samples.bin', output / 'warmup.bin'
        with references.open('x') as reference_stream:
            sample_storage = write_sparse_samples(source, count, sample_path, reference_stream)
        warmup_storage = (write_sparse_samples(warm, warm_count, warm_path) if warmup_dataset else
                          copy_sparse_prefix(sample_path, warm_path, warm_count))
        payload = dict(num_detectors=graph['num_detectors'], num_observables=graph['num_observables'],
            edges=graph['edges'], sample_storage=sample_storage, warmup_storage=warmup_storage,
            repeats=repeats, warmup_seconds=warmup_seconds, initializer=initializer, partition=partition,
            blossom_vertex_map=inverse[:graph['num_detectors']], blossom_edge_to_original=list(range(len(graph['edges']))))
        with payload_path.open('x') as stream:
            json.dump(payload, stream, separators=(',', ':'), allow_nan=False)
            stream.write('\n')
        if warmup_dataset and warm.verified_sha256 == source.verified_sha256:
            raise ValueError('Independent warm-up dataset duplicates the measurement dataset')
        manifest = dict(schema=1, complete=True, source=source.identity(), dem=dict(path=str(dem),sha256=sha256(dem)),
                        selected_shots=count, independent_measurement_inputs=count, repeats=repeats,
                        warmup=dict(source=warm.identity(), selected_shots=warm_count, seconds=warmup_seconds,
                                    policy='separate_saved_sequence' if warmup_dataset else 'reused_measurement_prefix'),
                        input_adapter_sha256=sha256(Path(__file__)), graph_converter=dict(
                            package='beyond_blossom',version=graph['converter_version'],stim_version=graph['stim_version'],
                            artifacts=graph.get('converter_artifacts', {})),
                        external_integer_scale=graph['integer_scale'],
                        source_oracle_scope='Original decoder quantization only; not an external integer-MWPM oracle',
                        external_integer_oracle_status='pending_native_validation',
                        physical_observable_truth_available=False,
                        sample_storage=sample_storage, warmup_storage=warmup_storage,
                        loading_scope='Indexed sparse sidecars; one-shot read/remapping/preparation outside decode timers; one decoder, complete ordered repeats, one warmup phase. Cache behavior may differ from historical resident inline arrays.',
                        partition_policy='Stable time order; private boundary per boundary edge; time distance//2 is the interface',
                        files={p.name:sha256(p) for p in [graph_path,payload_path,references,sample_path,warm_path]})
        (output/'input-provenance.json').write_text(json.dumps(manifest,indent=2,allow_nan=False)+'\n')
        return manifest
    except BaseException as error:
        (output/'failure.json').write_text(json.dumps(dict(complete=False,error=str(error)),indent=2)+'\n')
        raise


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset',type=Path,required=True)
    parser.add_argument('--dem',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repeats',type=int,default=3)
    parser.add_argument('--warmup-seconds',type=float,default=2)
    parser.add_argument('--limit',type=int)
    warm=parser.add_mutually_exclusive_group()
    warm.add_argument('--warmup-dataset',type=Path)
    warm.add_argument('--warmup-prefix',type=int,default=256)
    args=parser.parse_args(argv)
    try:
        convert(args.dataset,args.dem,args.output,repeats=args.repeats,warmup_seconds=args.warmup_seconds,
                limit=args.limit,warmup_dataset=args.warmup_dataset,warmup_prefix=args.warmup_prefix)
    except (OSError,ValueError) as error:
        print(str(error),file=sys.stderr)
        return 1
    print(args.output/'input-provenance.json')
    return 0

if __name__=='__main__':raise SystemExit(main())
