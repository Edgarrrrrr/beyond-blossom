#!/usr/bin/env python3
"""Run the saved-input generic regression matrix or recompute its summary."""
import argparse
import csv
import hashlib
import io
import json
import pathlib
import platform
import statistics
import subprocess
import time


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def summarize(output):
    rows = list(csv.DictReader((output / 'raw.csv').open()))
    groups = {}
    for row in rows:
        groups.setdefault(row['workload'], {}).setdefault(row['variant'], []).append(float(row['ns_per_shot']))
    summary = []
    for name, variants in groups.items():
        medians = {variant: statistics.median(values) for variant, values in variants.items()}
        summary.append({'workload': name, 'median_ns_per_shot': medians,
                        'min_ns_per_shot': {key: min(value) for key, value in variants.items()},
                        'max_ns_per_shot': {key: max(value) for key, value in variants.items()},
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
if args.repeats < 1:
    parser.error('repeats must be positive')
args.output.mkdir(parents=True, exist_ok=False)
cases = json.loads((args.corpora / 'manifest.json').read_text())
if args.surface_dem:
    cases.insert(0, {'name': 'surface_saved_shots', 'kind': 'dem',
                     'graph': str(args.surface_dem.resolve()), 'shots': str(args.surface_shots.resolve())})
manifest = {'scope': 'exploratory migration regression', 'started_unix': time.time(),
            'host': platform.node(), 'platform': platform.platform(), 'cpu_affinity': args.cpu,
            'python_version': platform.python_version(), 'status': 'running',
            'baseline_revision': '241160314087e5ae698c6e76c3c055a11c3ad422',
            'candidate_identity': args.candidate_identity, 'repeats': args.repeats,
            'timer': 'steady_clock; entire saved shot batch; graph construction, parsing, warmup excluded',
            'correctness': 'Every candidate warmup weight equals its source-shot weight; signed int64 little-endian vectors.',
            'baseline_binary_sha256': sha256(args.baseline_bin),
            'candidate_binary_sha256': sha256(args.candidate_bin), 'cases': cases, 'commands': []}
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
                binary = args.baseline_bin if variant == 'baseline' else args.candidate_bin
                command = [str(binary.resolve()), case['kind'], case['graph'], case['shots'], variant, '1', case['name']]
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
                if len(rows) != 1:
                    raise RuntimeError('Expected one timing record per invocation')
                row = rows[0]
                row['repeat'] = repeat
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
