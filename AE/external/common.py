"""Identity and validation helpers for the external native benchmarks."""
from __future__ import annotations

from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent
REFERENCE_REVISION = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
METHODS = ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf')
SPARSE_MAGIC = 0x4242535041525331
SPARSE_HEADER = struct.Struct('<4Q')
SPARSE_FORMAT = 'bb-sparse-u64-v1'


class SparseSamples:
    """A bounded-memory view of indexed sparse shots; no event array is retained."""
    def __init__(self, directory, metadata, num_detectors):
        self.fd = None
        if not isinstance(metadata, dict):
            raise ValueError('Invalid sparse sidecar metadata')
        name = metadata.get('file')
        if (metadata.get('format') != SPARSE_FORMAT or not isinstance(name, str) or
                not name or name in ('.', '..') or Path(name).name != name or '/' in name or '\\' in name):
            raise ValueError('Invalid sparse sidecar identity or filename')
        path = Path(directory) / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('Sparse sidecar must be a regular sibling file')
        if type(num_detectors) is not int or not 0 < num_detectors < 1 << 31:
            raise ValueError('Invalid sparse detector count')
        self.num_detectors = num_detectors
        self.fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        try:
            raw = os.pread(self.fd, SPARSE_HEADER.size, 0)
            if len(raw) != SPARSE_HEADER.size:
                raise ValueError('Truncated sparse sidecar header')
            magic, version, self.shots, self.events = SPARSE_HEADER.unpack(raw)
            if (magic != SPARSE_MAGIC or version != 1 or not self.shots or
                    type(metadata.get('shots')) is not int or metadata['shots'] != self.shots or
                    type(metadata.get('total_events')) is not int or metadata['total_events'] != self.events):
                raise ValueError('Sparse sidecar header disagrees with metadata')
            self.events_start = SPARSE_HEADER.size + 8 * (self.shots + 1)
            if os.fstat(self.fd).st_size != self.events_start + 8 * self.events:
                raise ValueError('Sparse sidecar size differs from header')
            previous = None
            for start in range(0, self.shots + 1, 65536):
                count = min(65536, self.shots + 1 - start)
                raw = os.pread(self.fd, 8 * count, SPARSE_HEADER.size + 8 * start)
                if len(raw) != 8 * count:
                    raise ValueError('Truncated sparse offsets')
                for (offset,) in struct.iter_unpack('<Q', raw):
                    if (previous is None and offset != 0 or previous is not None and
                            not previous <= offset <= min(self.events, previous + num_detectors)):
                        raise ValueError('Invalid sparse shot offsets')
                    previous = offset
            if previous != self.events:
                raise ValueError('Sparse final offset disagrees with event count')
        except BaseException:
            self.close()
            raise

    def __len__(self):
        return self.shots

    def __getitem__(self, index):
        if type(index) is not int or not 0 <= index < self.shots:
            raise IndexError('Sparse shot index out of range')
        raw = os.pread(self.fd, 16, SPARSE_HEADER.size + 8 * index)
        if len(raw) != 16:
            raise ValueError('Truncated sparse offsets')
        begin, end = struct.unpack('<2Q', raw)
        if not 0 <= begin <= end <= self.events or end - begin > self.num_detectors:
            raise ValueError('Invalid sparse shot offsets')
        raw = os.pread(self.fd, 8 * (end - begin), self.events_start + 8 * begin)
        if len(raw) != 8 * (end - begin):
            raise ValueError('Truncated sparse events')
        sample = list(struct.unpack(f'<{end - begin}Q', raw))
        if any(v >= self.num_detectors for v in sample) or any(a >= b for a, b in zip(sample, sample[1:])):
            raise ValueError('Sparse detector IDs must be sorted, unique and bounded')
        return sample

    def __iter__(self):
        for index in range(self.shots):
            yield self[index]

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __del__(self):
        self.close()


class MappedSamples:
    """Apply the frozen vertex permutation without changing detector order."""
    def __init__(self, samples, mapping):
        self.samples, self.mapping = samples, mapping

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, index):
        return [self.mapping[v] for v in self.samples[index]]

    def __iter__(self):
        for sample in self.samples:
            yield [self.mapping[v] for v in sample]


def attach_sparse_samples(payload, directory):
    """Attach lazy views to the small JSON; old inline payloads stay unchanged."""
    if not any(key in payload for key in ('sample_storage', 'warmup_storage')):
        return payload
    mapping = payload.get('blossom_vertex_map')
    if (not isinstance(mapping, list) or len(mapping) != payload['num_detectors'] or
            any(type(v) is not int or v < 0 for v in mapping) or len(set(mapping)) != len(mapping)):
        raise ValueError('Invalid sparse Blossom vertex mapping')
    try:
        for storage, key in [('sample_storage', 'samples'), ('warmup_storage', 'warmup')]:
            if storage not in payload:
                continue
            if key in payload or 'blossom_' + key in payload:
                raise ValueError('Ambiguous inline and sidecar samples')
            payload[key] = SparseSamples(directory, payload[storage], payload['num_detectors'])
            payload['blossom_' + key] = MappedSamples(payload[key], mapping)
    except BaseException:
        close_sparse_samples(payload)
        raise
    return payload


def close_sparse_samples(payload):
    for key in ('samples', 'warmup'):
        if isinstance(payload.get(key), SparseSamples):
            payload[key].close()
PAPER_COUNT_POLICY = 'manuscript-comparison-markers-v1'
PAPER_DISTANCES = (9, 13, 17, 21, 25, 29, 33)
PAPER_RATES = (Decimal('.0005'), Decimal('.001'), Decimal('.003'))
# Sample-count assignments come from the manuscript caption and marker fills.
# They do not supply or reconstruct the missing original timing measurements.
PAPER_REDUCED = {
    'parity': ({33}, {25, 29, 33}, {17, 21, 25, 29, 33}),
    'fusion': ({25, 29, 33}, {17, 21, 25, 29, 33}, {13, 17, 21, 25, 29, 33}),
    'ldpc-uf': ({33}, {29, 33}, {21, 25, 29, 33}),
}


def paper_shots(distance, rate, method):
    rate = Decimal(str(rate))
    if distance not in PAPER_DISTANCES or rate not in PAPER_RATES or method not in METHODS:
        raise ValueError('No manuscript shot assignment for this setting/method; specify custom counts')
    if method == 'parity-uf':
        return 10000  # Supplementary method, absent from the manuscript figure.
    reduced = PAPER_REDUCED.get(method)
    return 10000 if reduced and distance in reduced[PAPER_RATES.index(rate)] else 1000000


def shot_case(distance, rate):
    return f'd{distance:02d}-p{float(rate):g}'


def resolve_shot_counts(profile, settings, uniform=None, overrides=None, smoke_default=32):
    """Resolve and project method/case overrides to the selected input cases."""
    settings = tuple(settings)
    overrides = {} if overrides is None else overrides
    if not isinstance(overrides, dict):
        raise ValueError('Shot counts must contain a JSON object')
    known_cases = {shot_case(d, p) for d in PAPER_DISTANCES for p in PAPER_RATES}
    known_cases.update(shot_case(d, p) for d, p in settings)
    for name, value in overrides.items():
        if name in METHODS:
            values = [value]
        elif name in known_cases and isinstance(value, dict) and not set(value) - set(METHODS):
            values = value.values()
        else:
            raise ValueError('Unknown shot policy method/case: ' + str(name))
        if any(type(n) is not int or n <= 0 or (profile == 'paper' and n not in (10000, 1000000)) for n in values):
            raise ValueError('Unsupported distinct shot count')
    resolved = {}
    for d, p in settings:
        case = shot_case(d, p)
        resolved[case] = {}
        for method in METHODS:
            if method in overrides.get(case, {}):
                value = overrides[case][method]
            elif method in overrides:
                value = overrides[method]
            else:
                value = uniform if uniform is not None else (paper_shots(d, p, method) if profile == 'paper' else smoke_default)
            if type(value) is not int or value <= 0 or (profile == 'paper' and value not in (10000, 1000000)):
                raise ValueError('Unsupported distinct shot count')
            resolved[case][method] = value
    return resolved


def classify_shot_counts(rows):
    """Describe measured counts without relabeling older custom-policy results."""
    mismatches = []
    displayed = 0
    for row in sorted(rows, key=lambda r: (r['distance'], Decimal(str(r['error_rate'])), r['method'])):
        if row['method'] == 'parity-uf':
            continue
        displayed += 1
        try:
            expected = paper_shots(row['distance'], row['error_rate'], row['method'])
        except ValueError:
            expected = None
        if row['shots'] != expected:
            mismatches.append(dict(distance=row['distance'], error_rate=str(row['error_rate']),
                method=row['method'], actual_shots=row['shots'], manuscript_shots=expected))
    return dict(policy=PAPER_COUNT_POLICY if displayed and not mismatches else 'custom-counts',
                displayed_points=displayed, mismatched_points=len(mismatches), mismatches=mismatches,
                scope='Sample counts only; fresh timing values are not the recovered original paper measurements')
TIMING_SCOPES = {
    'parity': 'Rust SolverSerial.solve + subgraph; clear, input, validation and output excluded',
    'fusion': 'Rust SolverParallel.solve + subgraph; clear, input, validation and output excluded',
    'parity-uf': 'Rust SolverSerial.solve + subgraph with max_tree_size=0; clear, input, validation and output excluded',
    'unionfind': 'C++ Decoder.decode including returned correction copy; syndrome preparation, edge projection and clear excluded',
    'ldpc-uf': 'C++ PreparedUfDecoder.decode_prepared; cluster initialization, correction extraction and cleanup excluded',
}


def sha256(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def write_json(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write('\n')


def verify_files(root, files):
    root = Path(root).resolve()
    for relative, expected in files.items():
        path = (root / relative).resolve()
        if root not in path.parents or not path.is_file() or sha256(path) != expected:
            raise ValueError(f'Source or artifact identity mismatch: {relative}')


def verify_sources():
    lock = json.loads((HERE / 'sources.lock.json').read_text())
    verify_files(HERE, lock['files'])
    return lock


def source_identity():
    repo = HERE.parent.parent
    try:
        commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], stderr=subprocess.DEVNULL).decode().strip()
        status = subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain', '--untracked-files=all']).decode()
        return dict(commit=commit, dirty=bool(status), status=status)
    except subprocess.CalledProcessError:
        return dict(commit=None, dirty=True, status='No committed repository identity')


def check_chain(chain, sample, edges, detectors):
    if any(type(i) is not int or not 0 <= i < len(edges) for i in chain) or len(set(chain)) != len(chain):
        raise ValueError('Invalid or repeated correction edge index')
    parity = set(sample)
    weight, integer_weight, observable = 0.0, 0, 0
    for i in chain:
        u, v, w, obs, q = edges[i]
        for node in (u, v):
            if node == -1:
                continue
            if not 0 <= node < detectors:
                raise ValueError('Correction endpoint is outside the graph')
            if node in parity:
                parity.remove(node)
            else:
                parity.add(node)
        weight += w
        integer_weight += q
        observable ^= obs
    if parity:
        raise ValueError('Correction does not reproduce the input syndrome')
    return weight, integer_weight, observable
