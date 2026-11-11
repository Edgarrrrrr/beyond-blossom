#!/usr/bin/env python3
"""Collect native baseline timings on frozen inputs and verify correction witnesses."""
from __future__ import annotations

import argparse
import csv
import ctypes
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

import numpy as np

from common import (HERE, METHODS, REFERENCE_REVISION, TIMING_SCOPES, check_chain,
                    sha256, source_identity, verify_files, verify_sources, write_json)
from input_adapter import partition_graph


def physical_cpus(requested=None):
    if sys.platform != 'linux':
        if requested:
            raise ValueError('--cpus requires Linux CPU affinity')
        return []
    allowed = os.sched_getaffinity(0)
    groups = {}
    for cpu in sorted(allowed):
        topology = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
        package = int((topology / 'physical_package_id').read_text())
        core = int((topology / 'core_id').read_text())
        groups.setdefault(package, {}).setdefault(core, cpu)
    if requested:
        selected = [int(c) for c in requested.split(',')]
        if len(selected) != len(set(selected)) or any(c not in allowed for c in selected):
            raise ValueError('Requested CPUs must be distinct and available in this process cpuset')
        keys = []
        for cpu in selected:
            topology = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
            keys.append((int((topology / 'physical_package_id').read_text()), int((topology / 'core_id').read_text())))
        if len({k[0] for k in keys}) != 1 or len(set(keys)) != len(keys):
            raise ValueError('Select distinct physical cores on one socket')
        return selected
    return max((list(cores.values()) for cores in groups.values()), key=len)


def placement(cpus):
    if sys.platform == 'linux':
        return lambda: os.sched_setaffinity(0, cpus)
    if sys.platform == 'darwin':
        library = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
        library.pthread_set_qos_class_self_np.argtypes = [ctypes.c_uint, ctypes.c_int]
        def set_qos():
            if library.pthread_set_qos_class_self_np(0x19, 0):  # QOS_CLASS_USER_INITIATED
                raise RuntimeError('Cannot set Darwin main-thread QoS')
        return set_qos
    raise ValueError('Supported collection platforms are Linux and Darwin')


def validate_input(payload):
    m, obs = payload['num_detectors'], payload['num_observables']
    if type(m) is not int or not 0 < m < 1 << 31 or type(obs) is not int or not 0 <= obs <= 64:
        raise ValueError('Invalid graph dimensions')
    if m + len(payload['edges']) >= 1 << 31:
        raise ValueError('Graph exceeds the native 32-bit index range')
    if type(payload['repeats']) is not int or payload['repeats'] < 1:
        raise ValueError('Invalid repeat count')
    if not math.isfinite(payload['warmup_seconds']) or payload['warmup_seconds'] < 0:
        raise ValueError('Invalid warmup duration')
    endpoints = set()
    for edge in payload['edges']:
        if len(edge) != 5:
            raise ValueError('Invalid edge tuple')
        u, v, w, mask, q = edge
        if type(u) is not int or type(v) is not int or not 0 <= u < m or not -1 <= v < m or u == v:
            raise ValueError('Invalid edge endpoint')
        if not math.isfinite(w) or w <= 0 or type(mask) is not int or not 0 <= mask < 1 << obs:
            raise ValueError('Invalid edge weight or observable mask')
        if type(q) is not int or not 0 < q <= 20000 or q % 2:
            raise ValueError('Expected original positive even-integer quantization')
        key = (u, v) if v < 0 else tuple(sorted((u, v)))
        if key in endpoints:
            raise ValueError('Parallel edges must be merged before native collection')
        endpoints.add(key)
    if not endpoints:
        raise ValueError('Empty graph')
    for label in ('samples', 'warmup'):
        if not payload[label]:
            raise ValueError('Empty workload')
        for sample in payload[label]:
            if any(type(v) is not int or not 0 <= v < m for v in sample) or len(set(sample)) != len(sample):
                raise ValueError('Invalid or repeated detection event')


def validate_prepared_graph(payload, graph, provenance):
    for key in ('num_detectors', 'num_observables', 'edges'):
        if payload[key] != graph[key]:
            raise ValueError(f'Prepared graph and native input disagree: {key}')
    initializer, partition, inverse = partition_graph(graph, provenance['source']['distance'])
    expected = dict(initializer=initializer, partition=partition,
                    blossom_edge_to_original=list(range(len(graph['edges']))))
    for key, value in expected.items():
        if payload[key] != value:
            raise ValueError(f'Prepared graph mapping changed: {key}')
    for key in ('samples', 'warmup'):
        mapped = payload['blossom_' + key]
        if len(mapped) != len(payload[key]):
            raise ValueError('Blossom input sequence length mismatch')
        for sample, remapped in zip(payload[key], mapped):
            if remapped != [inverse[v] for v in sample]:
                raise ValueError('Blossom input sequence does not match the shared samples')
    if len(payload['samples']) != provenance['selected_shots'] or payload['repeats'] != provenance['repeats']:
        raise ValueError('Prepared shot/repeat counts changed')
    if len(payload['warmup']) != provenance['warmup']['selected_shots'] or payload['warmup_seconds'] != provenance['warmup']['seconds']:
        raise ValueError('Prepared warmup policy changed')


def load_oracle(path, payload):
    result = np.empty(len(payload['samples']), dtype=np.int64)
    with Path(path).open() as stream:
        for shot, sample in enumerate(payload['samples']):
            row = json.loads(next(stream))
            if row['shot'] != shot:
                raise ValueError('Independent oracle shot order mismatch')
            weight, q, obs = check_chain(row['edge_ids'], sample, payload['edges'], payload['num_detectors'])
            if q != row['integer_weight'] or obs != row['observable'] or not math.isclose(weight, row['weight'], rel_tol=1e-12, abs_tol=1e-9):
                raise ValueError('Independent oracle correction and reported metrics disagree')
            result[shot] = q
        if stream.read().strip():
            raise ValueError('Extra independent oracle records')
    return result


def validate_result(prefix, payload, optimum, method):
    n, repeats = len(payload['samples']), payload['repeats']
    totals = [0] * repeats
    gaps, nonoptimal = [0] * repeats, [0] * repeats
    ns = np.empty(n * repeats, dtype=np.uint64)
    with prefix.with_suffix('.csv').open(newline='') as stream, prefix.with_suffix('.corrections.jsonl').open() as chains:
        rows = csv.DictReader(stream)
        if rows.fieldnames != ['repeat', 'shot', 'decode_ns', 'weight', 'integer_weight', 'observable']:
            raise ValueError('Unexpected native CSV schema')
        for index, row in enumerate(rows):
            repeat, shot = divmod(index, n)
            if repeat >= repeats or (int(row['repeat']), int(row['shot'])) != (repeat, shot):
                raise ValueError('Native repeat or shot order mismatch')
            latency, q, obs = int(row['decode_ns']), int(row['integer_weight']), int(row['observable'])
            weight = float(row['weight'])
            if latency < 0 or latency >= 1 << 64 or not math.isfinite(weight):
                raise ValueError('Invalid native latency or weight')
            if repeat == 0:
                chain = json.loads(next(chains))
                cw, cq, co = check_chain(chain, payload['samples'][shot], payload['edges'], payload['num_detectors'])
                if (cq, co) != (q, obs) or not math.isclose(cw, weight, rel_tol=1e-12, abs_tol=1e-9):
                    raise ValueError('Native correction and reported metrics disagree')
            gap = q - int(optimum[shot])
            if gap < 0 or (method in ('parity', 'fusion') and gap):
                raise ValueError('Native result violates the independent integer MWPM optimum')
            if not 0 <= obs < 1 << payload['num_observables']:
                raise ValueError('Invalid native observable mask')
            ns[index] = latency
            totals[repeat] += latency
            gaps[repeat] += gap
            nonoptimal[repeat] += bool(gap)
        if 'index' not in locals() or index + 1 != n * repeats or chains.read().strip():
            raise ValueError('Incomplete or extra native records')
    means = [t / n / 1000 for t in totals]
    return dict(shots=n, repeats=repeats, independent_measurement_inputs=n,
                mean_us_per_shot=sum(totals) / (n * repeats) / 1000, repeat_mean_us=means,
                repeat_decode_seconds=[t / 1e9 for t in totals],
                std_repeat_mean_us=float(np.std(means, ddof=1)) if repeats > 1 else 0,
                p50_us=float(np.quantile(ns, .5, method='inverted_cdf')) / 1000,
                p99_us=float(np.quantile(ns, .99, method='inverted_cdf')) / 1000,
                max_us=int(ns.max()) / 1000,
                weight_gaps=[dict(mean_integer_gap=g / n, nonoptimal_shots=k) for g, k in zip(gaps, nonoptimal)],
                independent_chain_checks=n, native_parity_checks=n * repeats,
                integer_optimality='required' if method in ('parity', 'fusion') else 'measured gap; not an exact decoder',
                physical_observable_truth_available=False, logical_failures=None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Prepared directory from input_adapter.py')
    parser.add_argument('--build', type=Path, required=True, help='Directory containing build.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--methods', nargs='+', choices=METHODS, default=list(METHODS))
    parser.add_argument('--profile', choices=['smoke', 'paper'], default='smoke')
    parser.add_argument('--cpus', help='Comma-separated physical Linux CPUs on one socket')
    parser.add_argument('--timeout', type=float, default=0, help='Per-process seconds; zero has no deadline')
    args = parser.parse_args()
    if args.timeout < 0 or not math.isfinite(args.timeout) or len(set(args.methods)) != len(args.methods):
        parser.error('Invalid timeout or repeated method')
    root = args.output.resolve()
    if root.exists():
        parser.error('Use a fresh output directory; partial measurements are retained')
    verify_sources()
    identity = source_identity()
    build = json.loads((args.build / 'build.json').read_text())
    if not build.get('complete') or build['sources_lock_sha256'] != sha256(HERE / 'sources.lock.json'):
        raise ValueError('Build does not match the locked decoder sources')
    reference = build['reference']
    if not reference or reference.get('upstream_revision') != REFERENCE_REVISION:
        raise ValueError('Collection requires the fixed independent native oracle')
    if build['oracle_source_sha256'] != sha256(HERE / 'native/oracle.cpp'):
        raise ValueError('Oracle adapter changed after build')
    if args.profile == 'paper' and (identity['dirty'] or build['source']['dirty'] or identity['commit'] != build['source']['commit']):
        raise ValueError('Paper collection requires a clean committed source and a build from that same commit')
    if args.profile == 'paper' and not reference.get('stim_source_files'):
        raise ValueError('Paper collection requires a reference receipt with verified Stim source identities')
    provenance = json.loads((args.input / 'input-provenance.json').read_text())
    if not provenance.get('complete'):
        raise ValueError('Input preparation is incomplete')
    verify_files(args.input, provenance['files'])
    payload = json.loads((args.input / 'native-input.json').read_text())
    validate_input(payload)
    validate_prepared_graph(payload, json.loads((args.input / 'graph.json').read_text()), provenance)
    cpus = physical_cpus(args.cpus)
    if sys.platform == 'linux' and len(cpus) < (4 if 'fusion' in args.methods else 1):
        raise ValueError('Fusion collection requires four available physical cores on one socket')
    binary_names = {'oracle'} | {'blossom' if m in ('parity', 'fusion', 'parity-uf') else m for m in args.methods}
    for name in binary_names:
        binary = build['binaries'][name]
        if sha256(binary['path']) != binary['sha256']:
            raise ValueError(f'Binary changed after build: {name}')
    root.mkdir(parents=True)
    (root / 'bin').mkdir()
    for name in binary_names:
        shutil.copy2(build['binaries'][name]['path'], root / 'bin' / name)
    controller = {p.name: sha256(p) for p in [Path(__file__), HERE / 'common.py', HERE / 'input_adapter.py']}
    configuration = dict(schema=1, profile=args.profile, source=identity, build=build,
                         input_provenance=provenance, input_path=str(args.input.resolve()),
                         input_provenance_sha256=sha256(args.input / 'input-provenance.json'),
                         methods=args.methods, platform=platform.platform(), machine=platform.machine(),
                         host=platform.node(), placement='Linux inherited process affinity' if sys.platform == 'linux' else 'Darwin main-thread USER_INITIATED QoS; physical placement is not pinned',
                         controller_sha256=controller)
    write_json(root / 'run.json', configuration)

    def execute(name, command, selected_cpus):
        write_json(root / (name + '-command.json'), dict(argv=command, cpus=selected_cpus))
        started = time.monotonic()
        with (root / (name + '.log')).open('x') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True,
                           timeout=args.timeout or None, preexec_fn=placement(selected_cpus))
        return time.monotonic() - started

    try:
        execute('oracle', [str(root / 'bin/oracle'), str((args.input / 'native-input.json').resolve()), str(root / 'oracle.jsonl')], cpus[:1])
        optimum = load_oracle(root / 'oracle.jsonl', payload)
        summary = []
        for method in args.methods:
            name = 'blossom' if method in ('parity', 'fusion', 'parity-uf') else method
            prefix = root / method
            command = [str(root / 'bin' / name), str((args.input / 'native-input.json').resolve()), str(prefix)]
            if name == 'blossom':
                command.append(method)
            selected = cpus[:4 if method == 'fusion' else 1]
            wall_seconds = execute(method, command, selected)
            native_status = json.loads(prefix.with_suffix('.log').read_text().strip().splitlines()[-1])
            if not native_status.get('completed'):
                raise ValueError('Native runner did not report completion')
            result = validate_result(prefix, payload, optimum, method)
            result.update(method=method, timing_scope=TIMING_SCOPES[method], cpus=selected,
                          wall_seconds=wall_seconds, native_status=native_status,
                          files={p.name: sha256(p) for p in root.glob(method + '.*') if p.is_file()})
            write_json(root / (method + '.result.json'), result)
            summary.append(result)
            print(f"{method}: {result['mean_us_per_shot']:.6f} us/shot", flush=True)
        verify_files(args.input, provenance['files'])
        verify_files(HERE, controller)
        for name in binary_names:
            if sha256(root / 'bin' / name) != build['binaries'][name]['sha256']:
                raise ValueError('Executed binary changed during collection')
        write_json(root / 'summary.json', dict(complete=True, configuration=configuration,
                   oracle=dict(revision=REFERENCE_REVISION, records=len(optimum), sha256=sha256(root / 'oracle.jsonl')),
                   results=summary))
    except BaseException as error:
        write_json(root / 'failure.json', dict(complete=False, error=str(error)))
        raise


if __name__ == '__main__':
    main()
