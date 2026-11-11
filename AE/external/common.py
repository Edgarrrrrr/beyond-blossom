"""Identity and validation helpers for the external native benchmarks."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
REFERENCE_REVISION = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
METHODS = ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf')
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
