"""Identity and validation helpers for the external native benchmarks."""
from __future__ import annotations

from decimal import Decimal
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
REFERENCE_REVISION = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
METHODS = ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf')
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
