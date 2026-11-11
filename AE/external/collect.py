#!/usr/bin/env python3
"""Replay an AE input matrix through the five locked external native methods."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import subprocess
import sys

from common import HERE, METHODS, sha256, source_identity, verify_files, write_json
from input_adapter import SparseDataset


def input_matrix(ae_run):
    """Require the exact completed plan and every sealed input file."""
    ae_run = Path(ae_run).resolve()
    plan = json.loads((ae_run / 'plan.json').read_text())
    complete = json.loads((ae_run / 'complete.json').read_text())
    if not complete.get('complete') or complete['plan_sha256'] != sha256(ae_run / 'plan.json'):
        raise ValueError('AE run is incomplete or its plan changed')
    cases = [(f'd{d:02d}-p{p:g}', d, p) for p in plan['error_rates'] for d in plan['distances']]
    if len({c for c, _, _ in cases}) != len(cases):
        raise ValueError('AE plan contains duplicate cases')
    inputs = {item['case']: item for item in complete['inputs']}
    if len(inputs) != len(complete['inputs']) or set(inputs) != {c for c, _, _ in cases}:
        raise ValueError('AE input matrix is incomplete or duplicated')
    for case, _, _ in cases:
        files = inputs[case]['files']
        required = {f'inputs/{case}/{name}' for name in ('dataset.bin', 'model.dem', 'circuit.stim')}
        if not required <= set(files):
            raise ValueError(f'AE input seal omits required artifacts: {case}')
        verify_files(ae_run, files)
    return plan, complete, cases


def shot_count(policy, case, method, default):
    value = policy.get(case, {}).get(method, policy.get(method, default))
    if type(value) is not int or value < 1:
        raise ValueError(f'Invalid distinct shot count: {case}/{method}')
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ae-run', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', choices=['smoke', 'paper'], default='smoke')
    parser.add_argument('--methods', nargs='+', choices=METHODS, default=list(METHODS))
    parser.add_argument('--cpus')
    parser.add_argument('--limit', type=int, help='Common distinct-shot prefix; paper accepts 10000 or 1000000')
    parser.add_argument('--shot-counts', type=Path, help='JSON method->count defaults and/or case->{method:count} overrides')
    parser.add_argument('--repeats', type=int)
    parser.add_argument('--warmup-seconds', type=float)
    parser.add_argument('--warmup-prefix', type=int, default=256)
    parser.add_argument('--warmup-root', type=Path, help='Separate saved inputs directory containing case/dataset.bin and case/model.dem')
    parser.add_argument('--timeout', type=float, default=0)
    args = parser.parse_args(argv)
    root = args.output.resolve()
    if root.exists():
        parser.error('Use a fresh output directory')
    if len(set(args.methods)) != len(args.methods):
        parser.error('Repeated method')
    repeats = args.repeats if args.repeats is not None else (3 if args.profile == 'paper' else 1)
    warmup = args.warmup_seconds if args.warmup_seconds is not None else (2.0 if args.profile == 'paper' else .01)
    if repeats < 1 or (args.profile == 'paper' and repeats != 3):
        parser.error('Paper collection uses three timing repeats; smoke needs at least one')
    if not math.isfinite(warmup) or warmup < 0 or args.warmup_prefix < 1 or args.timeout < 0 or not math.isfinite(args.timeout):
        parser.error('Invalid warmup or timeout')
    identity = source_identity()
    if args.profile == 'paper' and (identity['dirty'] or not identity['commit']):
        parser.error('Paper collection requires a clean committed source tree')
    plan, complete, cases = input_matrix(args.ae_run)
    policy = json.loads(args.shot_counts.read_text()) if args.shot_counts else {}
    if not isinstance(policy, dict):
        parser.error('--shot-counts must contain a JSON object')
    case_names = {case for case, _, _ in cases}
    for key, value in policy.items():
        if key in METHODS:
            if type(value) is not int or value < 1:
                parser.error('Method shot counts must be positive integers')
        elif key in case_names:
            if not isinstance(value, dict) or set(value) - set(METHODS):
                parser.error('Case shot overrides must map known methods to positive integers')
        else:
            parser.error(f'Unknown method or case in shot counts: {key}')
    schedule = []
    for case, distance, error_rate in cases:
        saved = SparseDataset(args.ae_run / 'inputs' / case / 'dataset.bin')
        if saved.distance != distance or saved.error_rate != error_rate or saved.shots != plan['shots']:
            raise ValueError(f'Saved input dimensions disagree with the AE plan: {case}')
        default = args.limit if args.limit is not None else (1000000 if args.profile == 'paper' else min(64, saved.shots))
        grouped = {}
        for method in args.methods:
            count = shot_count(policy, case, method, default)
            if count > saved.shots or (args.profile == 'paper' and count not in (10000, 1000000)):
                raise ValueError(f'Insufficient saved inputs or unsupported paper shot count: {case}/{method}/{count}')
            grouped.setdefault(count, []).append(method)
        schedule.extend(dict(case=case, distance=distance, error_rate=error_rate, shots=count,
                             source_shots=saved.shots, methods=methods) for count, methods in grouped.items())
    root.mkdir(parents=True)
    controller = {name: sha256(HERE / name) for name in ('collect.py', 'input_adapter.py', 'run.py', 'common.py')}
    configuration = dict(schema=1, source=identity, profile=args.profile,
                         ae_run=str(args.ae_run.resolve()), ae_plan_sha256=sha256(args.ae_run / 'plan.json'),
                         ae_complete_sha256=sha256(args.ae_run / 'complete.json'),
                         external_build=str(args.build.resolve()), external_build_sha256=sha256(args.build / 'build.json'),
                         repeats=repeats, warmup_seconds=warmup, warmup_prefix=args.warmup_prefix,
                         warmup_root=str(args.warmup_root.resolve()) if args.warmup_root else None,
                         shot_counts=policy, requested_limit=args.limit, schedule=schedule,
                         sampling_policy='Saved source order; every reduced workload is a prefix of the identical saved case',
                         historical_claim='Fresh collection policy; no historical million/ten-thousand assignment is inferred',
                         controller_sha256=controller)
    write_json(root / 'plan.json', configuration)
    results, completed = [], []
    try:
        for task in schedule:
            case, count = task['case'], task['shots']
            directory = root / case / f'n{count}'
            directory.mkdir(parents=True)
            source = args.ae_run.resolve() / 'inputs' / case
            command = [sys.executable, str(HERE / 'input_adapter.py'), '--dataset', str(source / 'dataset.bin'),
                       '--dem', str(source / 'model.dem'), '--output', str(directory / 'input'),
                       '--limit', str(count), '--repeats', str(repeats), '--warmup-seconds', str(warmup)]
            if args.warmup_root:
                command += ['--warmup-dataset', str(args.warmup_root.resolve() / case / 'dataset.bin')]
            else:
                command += ['--warmup-prefix', str(args.warmup_prefix)]
            write_json(directory / 'prepare-command.json', command)
            with (directory / 'prepare.log').open('x') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
            command = [sys.executable, str(HERE / 'run.py'), '--input', str(directory / 'input'),
                       '--build', str(args.build.resolve()), '--output', str(directory / 'run'),
                       '--profile', args.profile, '--timeout', str(args.timeout), '--methods', *task['methods']]
            if args.cpus:
                command += ['--cpus', args.cpus]
            write_json(directory / 'run-command.json', command)
            with (directory / 'run.log').open('x') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
            summary_path = directory / 'run/summary.json'
            summary = json.loads(summary_path.read_text())
            if not summary.get('complete') or {r['method'] for r in summary['results']} != set(task['methods']):
                raise ValueError('External method group did not complete')
            for item in summary['results']:
                if item['shots'] != count or item['repeats'] != repeats:
                    raise ValueError('Measured shot/repeat counts disagree with the collection plan')
                results.append(dict(case=case, distance=task['distance'], error_rate=task['error_rate'],
                                    source_shots=task['source_shots'], workload='full' if count == task['source_shots'] else 'prefix',
                                    summary=str(summary_path.relative_to(root)), **item))
            completed.append(dict(case=case, shots=count, methods=task['methods'],
                                  summary=str(summary_path.relative_to(root)), sha256=sha256(summary_path)))
            print(f"{case}: {count} independent inputs × {repeats} repeats; {', '.join(task['methods'])}", flush=True)
        verify_files(HERE, controller)
        if sha256(args.ae_run / 'plan.json') != configuration['ae_plan_sha256'] or sha256(args.ae_run / 'complete.json') != configuration['ae_complete_sha256']:
            raise ValueError('AE input manifest changed during collection')
        for item in complete['inputs']:
            verify_files(args.ae_run, item['files'])
        columns = ['case', 'distance', 'error_rate', 'method', 'shots', 'source_shots', 'workload', 'repeats',
                   'mean_us_per_shot', 'p50_us', 'p99_us', 'max_us', 'timing_scope', 'summary']
        with (root / 'summary.csv').open('x', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=columns, extrasaction='ignore')
            writer.writeheader()
            writer.writerows(results)
        write_json(root / 'summary.json', dict(complete=True, configuration=configuration, results=results))
        write_json(root / 'complete.json', dict(complete=True, plan_sha256=sha256(root / 'plan.json'), groups=completed,
                    summary_sha256=sha256(root / 'summary.json'), summary_csv_sha256=sha256(root / 'summary.csv')))
    except BaseException as error:
        write_json(root / 'failure.json', dict(complete=False, completed_groups=completed, error=str(error)))
        raise
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
