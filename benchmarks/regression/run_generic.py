#!/usr/bin/env python3
"""Run the saved-input generic regression matrix or recompute its summary."""
import argparse
import csv
import hashlib
import io
import json
import pathlib
import platform
import shutil
import statistics
import subprocess
import time


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def summarize(output):
    rows = list(csv.DictReader((output / 'raw.csv').open()))
    groups = {}
    for row in rows:
        # Repeated timing batches reuse the same fixed corpus within one
        # process; summarize that process before comparing balanced launches.
        group = groups.setdefault(row['workload'], {}).setdefault(row['variant'], {})
        group.setdefault(row.get('matrix_repeat', row['repeat'] if 'repeat' in row else '0'), []).append(float(row['ns_per_shot']))
    summary = []
    for name, variants in groups.items():
        process_values = {variant: [statistics.median(batches) for batches in repeats.values()]
                          for variant, repeats in variants.items()}
        medians = {variant: statistics.median(values) for variant, values in process_values.items()}
        summary.append({'workload': name, 'median_ns_per_shot': medians,
                        'min_ns_per_shot': {key: min(value) for key, value in process_values.items()},
                        'max_ns_per_shot': {key: max(value) for key, value in process_values.items()},
                        'process_medians_ns_per_shot': process_values,
                        'speedup_on_vs_source': medians['baseline'] / medians['on'],
                        'speedup_off_vs_source': medians['baseline'] / medians['off'],
                        'speedup_on_vs_off': medians['off'] / medians['on']})
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


parser = argparse.ArgumentParser()
parser.add_argument('--baseline-bin', type=pathlib.Path)
parser.add_argument('--candidate-bin', type=pathlib.Path)
parser.add_argument('--corpora', type=pathlib.Path)
parser.add_argument('--surface-dem', type=pathlib.Path)
parser.add_argument('--surface-shots', type=pathlib.Path)
parser.add_argument('--output', type=pathlib.Path, required=True)
parser.add_argument('--repeats', type=int, default=7)
parser.add_argument('--batches-per-process', type=int, default=1)
parser.add_argument('--cpu', type=int)
parser.add_argument('--candidate-identity')
parser.add_argument('--summarize-only', action='store_true')
args = parser.parse_args()
if args.summarize_only:
    summarize(args.output)
    raise SystemExit
if not all([args.baseline_bin, args.candidate_bin, args.corpora, args.candidate_identity]):
    parser.error('Binary paths, corpora directory, and actual candidate identity are required')
if bool(args.surface_dem) != bool(args.surface_shots):
    parser.error('Both surface input paths are required together')
if args.repeats < 1 or args.batches_per_process < 1:
    parser.error('repeats and batches-per-process must be positive')
args.output.mkdir(parents=True, exist_ok=False)
artifacts = args.output / 'artifacts'
artifacts.mkdir()
binaries = {}
for variant, binary in [('baseline', args.baseline_bin), ('candidate', args.candidate_bin)]:
    binaries[variant] = artifacts / variant
    shutil.copy2(binary, binaries[variant])
    receipt = binary.parent / 'build.json'
    if receipt.is_file():
        shutil.copy2(receipt, artifacts / f'{variant}-build.json')
shutil.copy2(__file__, artifacts / 'run_generic.py')
cases = json.loads((args.corpora / 'manifest.json').read_text())
if args.surface_dem:
    cases.insert(0, {'name': 'surface_saved_shots', 'kind': 'dem',
                     'graph': str(args.surface_dem.resolve()), 'shots': str(args.surface_shots.resolve())})
manifest = {'scope': 'exploratory migration regression', 'started_unix': time.time(),
            'host': platform.node(), 'platform': platform.platform(), 'cpu_affinity': args.cpu,
            'python_version': platform.python_version(), 'status': 'running',
            'baseline_revision': '241160314087e5ae698c6e76c3c055a11c3ad422',
            'candidate_identity': args.candidate_identity, 'repeats': args.repeats,
            'batches_per_process': args.batches_per_process,
            'summary_unit': 'median of process medians; each process repeats the same fixed shot corpus',
            'timer': 'steady_clock; entire saved shot batch; graph construction, parsing, warmup excluded',
            'correctness': 'Every candidate warmup weight equals its source-shot weight; signed int64 little-endian vectors.',
            'baseline_binary_sha256': sha256(args.baseline_bin),
            'candidate_binary_sha256': sha256(args.candidate_bin),
            'archived_binaries': {key: str(value.resolve()) for key, value in binaries.items()},
            'controller_sha256': sha256(__file__), 'cases': cases, 'commands': []}
for case in cases:
    case['graph_sha256'] = sha256(case['graph'])
    case['shots_sha256'] = sha256(case['shots'])
(args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
with (args.output / 'raw.csv').open('w', newline='') as raw:
    writer = None
    expected_weights = {}
    for repeat in range(args.repeats):
        # Reverse the order every round to expose drift instead of assigning it
        # systematically to the candidate. Every process warms the entire corpus.
        variants = ['baseline', 'off', 'on'] if repeat % 2 == 0 else ['on', 'off', 'baseline']
        for case in cases:
            for variant in variants:
                binary = binaries['baseline' if variant == 'baseline' else 'candidate']
                command = [str(binary.resolve()), case['kind'], case['graph'], case['shots'], variant,
                           str(args.batches_per_process), case['name']]
                weights_path = args.output / f"{case['name']}.weights.i64le"
                if variant == 'baseline' and not weights_path.exists():
                    command += ['--write-weights', str(weights_path.resolve())]
                else:
                    if not weights_path.exists():
                        raise RuntimeError('Source weights must exist before candidate execution')
                    command += ['--expected-weights', str(weights_path.resolve())]
                if args.cpu is not None:
                    command = ['taskset', '-c', str(args.cpu), *command]
                manifest['commands'].append(command)
                process = subprocess.run(command, text=True, capture_output=True)
                (args.output / f"{case['name']}-{variant}-{repeat}.stderr").write_text(process.stderr)
                if process.returncode:
                    manifest['status'] = 'failed'
                    manifest['failed_command'] = command
                    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
                    raise RuntimeError(process.stderr or f'Benchmark exited {process.returncode}')
                case['expected_weights_sha256'] = sha256(weights_path)
                rows = list(csv.DictReader(io.StringIO(process.stdout)))
                if len(rows) != args.batches_per_process:
                    raise RuntimeError('Unexpected number of timing batches from native process')
                for native_repeat, row in enumerate(rows):
                    if int(row['repeat']) != native_repeat:
                        raise RuntimeError('Native timing batches are missing or out of order')
                    row['native_repeat'] = row.pop('repeat')
                    row['matrix_repeat'] = repeat
                    weight = (row['weight_checksum'], row['weight_sequence_hash'])
                    if writer is None:
                        writer = csv.DictWriter(raw, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    raw.flush()
                    if case['name'] in expected_weights and weight != expected_weights[case['name']]:
                        manifest['status'] = 'weight_mismatch'
                        manifest['failed_command'] = command
                        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
                        raise RuntimeError(f"Matching weight mismatch for {case['name']}: {variant}")
                    expected_weights[case['name']] = weight
manifest['finished_unix'] = time.time()
manifest['status'] = 'complete'
manifest['raw_sha256'] = sha256(args.output / 'raw.csv')
(args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
summarize(args.output)
