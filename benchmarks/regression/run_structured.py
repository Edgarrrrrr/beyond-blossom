#!/usr/bin/env python3
"""Compare source and candidate native adapters on one saved, independently checked matrix."""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import struct
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'AE'))
import reproduce as ae

SOURCE_REVISION = '241160314087e5ae698c6e76c3c055a11c3ad422'
REFERENCE_REVISION = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
METHODS = tuple(name for name in ae.METHODS if name != 'reference')
RAW_FIELDS = ['case', 'distance', 'error_rate', 'method', 'repeat', 'variant', 'backend',
              'threads', 'shots', 'elapsed_ns', 'ns_per_shot', 'observable_differences',
              'actual_candidate_witnesses', 'dataset_sha256', 'directory']


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, help='Directory with build_structured.py build.json and binaries')
    parser.add_argument('--reference-binary', type=Path)
    parser.add_argument('--reference-record', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpus', help='Distinct physical CPUs on one socket; default selects available cores')
    parser.add_argument('--source-identity', type=Path, help='Optional JSON identity receipt for the migration source')
    parser.add_argument('--candidate-identity', type=Path, help='Optional JSON identity receipt for the built candidate snapshot')
    parser.add_argument('--distances', default='9,13,17,21,25,29,33')
    parser.add_argument('--error-rates', default='0.0005,0.001,0.003')
    parser.add_argument('--methods', default=','.join(METHODS))
    parser.add_argument('--backend', choices=['scalar', 'avx2', 'avx512vl', 'avx512'], default='avx512vl')
    parser.add_argument('--shots', type=int, default=10000)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--seed', type=int, default=20261112)
    parser.add_argument('--require-clean', action='store_true', help='Reject a dirty controller checkout; snapshot receipts still identify the compiled binaries')
    parser.add_argument('--summarize-only', action='store_true')
    return parser


def validate_cpus(cpus, needed):
    if len(cpus) < needed:
        raise ValueError(f'Need {needed} physical CPUs for the selected methods')
    if platform.system() == 'Darwin':
        if any(cpu != -1 for cpu in cpus):
            raise ValueError('Darwin requires -1 QoS slots instead of physical CPU IDs')
        return
    if platform.system() != 'Linux' or len(set(cpus)) != len(cpus):
        raise ValueError('Expected distinct physical Linux CPUs')
    if not set(cpus) <= os.sched_getaffinity(0):
        raise ValueError('Requested CPUs are outside the process affinity set')
    keys = []
    for cpu in cpus:
        base = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
        keys.append(((base / 'physical_package_id').read_text().strip(), (base / 'core_id').read_text().strip()))
    if len(set(keys)) != len(keys) or len({key[0] for key in keys}) != 1:
        raise ValueError('Use distinct physical cores on one socket')


def snapshot_receipt(path):
    if path is None:
        return None
    record = json.loads(path.read_text())
    if not isinstance(record, dict):
        raise ValueError('Snapshot identities must be JSON objects')
    return dict(path=str(path.resolve()), sha256=ae.digest(path), record=record)


def dataset_header(path, distance, probability, shots):
    with path.open('rb') as stream:
        data = stream.read(96)
    if len(data) != 96:
        raise ValueError('Truncated prepared dataset header')
    header = struct.unpack('<12Q', data)
    if (header[0] != 0x5354454E43494C31 or header[1] not in (2, 3) or header[2] != distance or
            struct.unpack('<d', struct.pack('<Q', header[3]))[0] != probability or header[4] != shots):
        raise ValueError('Prepared dataset does not match the selected workload')
    return header


def artifact_files(root, directory):
    return {str(p.relative_to(root)): ae.digest(p) for p in sorted(directory.rglob('*')) if p.is_file()}


def measurement(directory, *, shots, options, header):
    correct = json.loads((directory / 'correctness.json').read_text())
    if correct.get('status') != 'passed' or correct.get('weight_differences') != 0 or correct.get('shots') != shots:
        raise ValueError(f'Native integer-weight correctness failed: {directory}')
    differences = correct.get('observable_differences', 0)
    witnesses = correct.get('actual_candidate_witnesses', 0)
    if differences < 0 or witnesses < differences:
        raise ValueError('Equal-weight observable differences lack candidate correction witnesses')
    with (directory / 'measurement.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise ValueError('Expected one complete native batch timing row')
    row = rows[0]
    mode = 'pipe-batch' if options['execution'] == 'pipeline' else 'batch'
    elapsed = int(row['elapsed_ns'])
    if (int(row['shots']) != shots or elapsed <= 0 or row['mode'] != mode or row['execution'] != options['execution'] or
            int(row['total_threads']) != options['threads'] or int(row['weight_diff']) != 0 or
            int(row['observable_diff']) != differences or int(row['syndrome_hash']) != header[7] or
            int(row['reference_result_hash']) != header[8]):
        raise ValueError('Native timing row differs from its workload or correctness receipt')
    warmup = json.loads((directory / 'warmup.json').read_text())
    if warmup.get('requested_seconds') != 2 or warmup.get('actual_elapsed_ns', 0) < 2000000000 or warmup.get('shots', 0) < 1:
        raise ValueError('Native warmup did not complete the requested two seconds')
    scope = json.loads((directory / 'measurement_scope.json').read_text())
    if scope.get('mode') != mode or scope.get('internal_instrumentation') is not False:
        raise ValueError('Expected an uninstrumented native batch measurement')
    return elapsed, differences, witnesses


def summarize(root):
    root = Path(root).resolve()
    complete = json.loads((root / 'complete.json').read_text())
    if not complete.get('complete') or complete['plan_sha256'] != ae.digest(root / 'plan.json') or complete['raw_sha256'] != ae.digest(root / 'raw.csv'):
        raise ValueError('Incomplete run or changed raw measurements')
    plan = json.loads((root / 'plan.json').read_text())
    ae.verify_relative_files(root, plan['build_artifacts'])
    for item in complete['inputs'] + complete['calls']:
        ae.verify_relative_files(root, item['files'])
        if 'preparation_files' in item:
            ae.verify_relative_files(root, item['preparation_files'])
    ae.verify_relative_files(root, complete['binary_files'])
    with (root / 'raw.csv').open(newline='') as stream:
        raw = list(csv.DictReader(stream))
    expected = {(case['case'], method, repeat, variant) for case in plan['cases'] for method in plan['methods']
                for repeat in range(1, plan['repeats'] + 1) for variant in ('source', 'candidate')}
    actual = {(r['case'], r['method'], int(r['repeat']), r['variant']) for r in raw}
    if actual != expected or len(raw) != len(expected):
        raise ValueError('Missing or repeated structured regression measurement')
    groups = {}
    for row in raw:
        if int(row['shots']) != plan['shots'] or int(row['elapsed_ns']) <= 0:
            raise ValueError('Invalid raw workload or elapsed time')
        groups.setdefault((row['case'], row['method']), {}).setdefault(row['variant'], {})[int(row['repeat'])] = int(row['elapsed_ns']) / int(row['shots'])
    results = []
    for case in plan['cases']:
        for method in plan['methods']:
            group = groups[case['case'], method]
            source, candidate = list(group['source'].values()), list(group['candidate'].values())
            sm, cm = statistics.median(source), statistics.median(candidate)
            paired = [group['source'][r] / group['candidate'][r] for r in range(1, plan['repeats'] + 1)]
            results.append(dict(case=case['case'], distance=case['distance'], error_rate=case['error_rate'], method=method,
                independent_shots=plan['shots'], repeats=plan['repeats'], source_median_ns=sm, source_min_ns=min(source), source_max_ns=max(source),
                candidate_median_ns=cm, candidate_min_ns=min(candidate), candidate_max_ns=max(candidate),
                speedup_source_over_candidate=sm / cm, candidate_change_percent=(cm / sm - 1) * 100,
                paired_speedup_median=statistics.median(paired), paired_speedup_min=min(paired), paired_speedup_max=max(paired)))
    aggregates = []
    for method in plan['methods']:
        speedups = [row['speedup_source_over_candidate'] for row in results if row['method'] == method]
        aggregates.append(dict(method=method, cases=len(speedups), geometric_mean_speedup=math.exp(statistics.mean(math.log(v) for v in speedups)),
                               minimum_speedup=min(speedups), maximum_speedup=max(speedups), slower_cases=sum(v < 1 for v in speedups)))
    result = dict(schema=1, scope=plan['scope'], source_revision=SOURCE_REVISION, candidate_identity=plan['candidate_identity'],
                  units='Nanoseconds per shot; repetition distributions are batch means, not shot latency percentiles',
                  comparison='speedup = median(source batch means) / median(candidate batch means); values below one expose a slowdown',
                  raw_sha256=complete['raw_sha256'], rows=results, by_method=aggregates)
    if (root / 'summary.json').exists():
        if json.loads((root / 'summary.json').read_text()) != result:
            raise ValueError('Existing summary disagrees with the sealed raw measurements')
    else:
        ae.write(root / 'summary.json', result)
        with (root / 'summary.csv').open('x', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(results[0]))
            writer.writeheader()
            writer.writerows(results)
    return result


def collect(args):
    if not all((args.build, args.reference_binary, args.reference_record)):
        raise ValueError('--build, --reference-binary and --reference-record are required')
    root = args.output.resolve()
    if root.exists():
        raise ValueError('Use a fresh output directory; existing measurements are never overwritten')
    distances = [int(value) for value in args.distances.split(',')]
    rates = [float(value) for value in args.error_rates.split(',')]
    methods = args.methods.split(',')
    if (args.shots < 1 or args.repeats < 1 or args.seed < 0 or not methods or len(set(methods)) != len(methods) or set(methods) - set(METHODS) or
            len(set(distances)) != len(distances) or len(set(rates)) != len(rates) or any(d < 3 or d % 2 == 0 for d in distances) or
            any(not math.isfinite(p) or not 0 < p < .5 for p in rates)):
        raise ValueError('Invalid structured regression workload')
    cpus = [int(value) for value in args.cpus.split(',')] if args.cpus else ae.physical_cpus()
    needed = max(ae.METHODS[m]['threads'] for m in methods)
    cpus = cpus[:needed]
    validate_cpus(cpus, needed)
    identity = ae.source_identity()
    if args.require_clean and (not identity['commit'] or identity['status']):
        raise ValueError('Clean collection requires a committed, unchanged controller checkout')
    build = json.loads((args.build / 'build.json').read_text())
    builds = {record['variant']: record for record in build['builds']}
    if build.get('kind') != 'pure_timing' or set(builds) != {'source', 'candidate'} or len(build['builds']) != 2:
        raise ValueError('Expected two uninstrumented structured adapter build records')
    for variant, record in builds.items():
        if record.get('lto') is not False or ae.digest(record['binary']) != record['binary_sha256']:
            raise ValueError(f'Changed executable or enabled LTO: {variant}')
    reference = json.loads(args.reference_record.read_text())
    if not reference.get('complete') or reference.get('upstream_revision') != REFERENCE_REVISION or ae.digest(args.reference_binary) != reference['binary']['sha256']:
        raise ValueError('The preparation executable must match the fixed official reference receipt')
    if len({builds[v]['binary_sha256'] for v in builds} | {reference['binary']['sha256']}) != 3:
        raise ValueError('Source, candidate and independent reference executables must be distinct')
    cases = [dict(case=f'd{d:02d}-p{p:g}', distance=d, error_rate=p, stream=stream)
             for stream, (d, p) in enumerate(((d, p) for p in rates for d in distances), 1)]
    root.mkdir(parents=True)
    (root / 'bin').mkdir()
    for variant, record in builds.items():
        shutil.copy2(record['binary'], root / 'bin' / variant)
    shutil.copy2(args.reference_binary, root / 'bin/reference')
    (root / 'build').mkdir()
    shutil.copyfile(args.build / 'build.json', root / 'build/build.json')
    shutil.copyfile(args.reference_record, root / 'build/reference.json')
    build_artifacts = {}
    for variant, record in builds.items():
        directory = root / 'build' / variant
        directory.mkdir()
        flags = Path(record['flags_make'])
        if flags.is_file():
            if ae.digest(flags) != record['flags_make_sha256']:
                raise ValueError('Build flags changed after adapter compilation')
            shutil.copyfile(flags, directory / 'flags.make')
            library_build = flags.parents[2]
            for name in ('CMakeCache.txt', 'compile_commands.json'):
                if (library_build / name).is_file():
                    shutil.copyfile(library_build / name, directory / name)
        build_artifacts.update(artifact_files(root, directory))
    candidate_receipt = snapshot_receipt(args.candidate_identity)
    source_receipt = snapshot_receipt(args.source_identity)
    for variant, receipt in [('source', source_receipt), ('candidate', candidate_receipt)]:
        if receipt:
            shutil.copyfile(receipt['path'], root / 'build' / (variant + '-identity.json'))
    build_artifacts = artifact_files(root, root / 'build')
    controller = {str(Path(__file__).relative_to(REPO)): ae.digest(__file__), 'AE/reproduce.py': ae.digest(REPO / 'AE/reproduce.py')}
    plan = dict(schema=1, scope='Development performance-preservation comparison; dirty snapshots are labeled and no formal paper cohort is claimed',
                created_utc=datetime.now(timezone.utc).isoformat(), host=platform.node(), platform=platform.platform(),
                python_version=platform.python_version(), controller_identity=identity, controller_sha256=controller,
                source_revision=SOURCE_REVISION, source_identity=source_receipt,
                candidate_identity=candidate_receipt or dict(observed_checkout=identity,
                    qualification='Checkout observed at collection; build receipt and executable hash identify the compiled snapshot'),
                build=build, build_artifacts=build_artifacts, reference=reference,
                binaries={v: dict(path=str(root / 'bin' / v), sha256=ae.digest(root / 'bin' / v)) for v in ('source', 'candidate', 'reference')},
                cases=cases, methods={m: ae.METHODS[m] for m in methods}, backend=args.backend, shots=args.shots,
                repeats=args.repeats, seed=args.seed, cpus=cpus, warmup_seconds=2,
                scheduling='All native calls serialized; rotate methods each repeat; reverse adjacent source/candidate pairs on alternate repeats',
                timing='Normal decode and result store for serial/shared; pipeline caller completion and final drain inside batch; input preparation, warmup and correctness checks excluded',
                correctness='Fixed official saved integer weights; native untimed validation; equal-weight observable differences require candidate correction witnesses')
    ae.write(root / 'plan.json', plan)
    calls, inputs = [], []
    try:
        with (root / 'raw.csv').open('x', newline='') as raw:
            writer = csv.DictWriter(raw, fieldnames=RAW_FIELDS)
            writer.writeheader()
            raw.flush()
            for case in cases:
                name, d, p = case['case'], case['distance'], case['error_rate']
                data = root / 'inputs' / name
                data.mkdir(parents=True)
                dataset = data / 'dataset.bin'
                common = ['--distance', str(d), '--error-rate', str(p), '--shots', str(args.shots), '--dataset', str(dataset),
                          '--root-seed', str(args.seed), '--stream-id', str(case['stream'])]
                prepared = ae.invoke([str(root / 'bin/reference'), *common, '--mode', 'prepare', '--main-cpu', str(cpus[0])], root / 'preparation' / name)
                for filename in ('model.dem', 'circuit.stim'):
                    shutil.copyfile(prepared / filename, data / filename)
                header = dataset_header(dataset, d, p, args.shots)
                corpus_files = artifact_files(root, data)
                inputs.append(dict(case=name, header=list(header), files=corpus_files,
                                   preparation_files=artifact_files(root, prepared)))
                for repeat in range(args.repeats):
                    order = methods[repeat % len(methods):] + methods[:repeat % len(methods)]
                    variants = ('source', 'candidate') if repeat % 2 == 0 else ('candidate', 'source')
                    for method in order:
                        options = dict(ae.METHODS[method])
                        options.setdefault('backend', args.backend)
                        threads = options['threads']
                        for variant in variants:
                            mode = 'pipe-batch' if options['execution'] == 'pipeline' else 'batch'
                            command = [str(root / 'bin' / variant), *common, '--mode', mode, '--cpus', ','.join(map(str, cpus[:threads])),
                                       '--warmup-seconds', '2', '--verify', 'weight', '--block-size', '1024', '--repeats', '1']
                            for key, value in options.items():
                                if key != 'threads':
                                    command += ['--' + key.replace('_', '-'), str(value)]
                            directory = root / 'calls' / name / f'{repeat + 1:02d}' / method / variant
                            native = ae.invoke(command, directory)
                            elapsed, differences, witnesses = measurement(native, shots=args.shots, options=options, header=header)
                            row = dict(case=name, distance=d, error_rate=p, method=method, repeat=repeat + 1, variant=variant,
                                backend=options['backend'], threads=threads, shots=args.shots, elapsed_ns=elapsed, ns_per_shot=elapsed / args.shots,
                                observable_differences=differences, actual_candidate_witnesses=witnesses,
                                dataset_sha256=corpus_files[str(dataset.relative_to(root))], directory=str(native.relative_to(root)))
                            writer.writerow(row)
                            raw.flush()
                            calls.append(dict(case=name, method=method, repeat=repeat + 1, variant=variant,
                                              directory=str(native.relative_to(root)), files=artifact_files(root, directory)))
                        print(f'{name} {method} repeat {repeat + 1}: both variants passed', flush=True)
                ae.verify_relative_files(root, corpus_files)
        ae.verify_relative_files(REPO, controller)
        ae.verify_relative_files(root, build_artifacts)
        binaries = {f'bin/{v}': item['sha256'] for v, item in plan['binaries'].items()}
        ae.verify_relative_files(root, binaries)
        ae.write(root / 'complete.json', dict(complete=True, plan_sha256=ae.digest(root / 'plan.json'),
                 raw_sha256=ae.digest(root / 'raw.csv'), inputs=inputs, calls=calls, binary_files=binaries))
        result = summarize(root)
        print(f"Validated {len(cases)} cases, {len(methods)} methods, {args.repeats} repeats: {root / 'summary.csv'}")
        return result
    except BaseException as error:
        ae.write(root / 'failure.json', dict(complete=False, completed_calls=len(calls), inputs=inputs, calls=calls, error=str(error)))
        raise


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    if args.summarize_only:
        result = summarize(args.output)
        print(json.dumps(result['by_method'], indent=2))
    else:
        collect(args)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
