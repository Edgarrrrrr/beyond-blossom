#!/usr/bin/env python3
"""Archive completed paper acquisition; never accept smoke as paper evidence."""
import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import shutil

def read(path):
    return json.loads(path.read_text())

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.work.resolve()
    run = root / 'slurm-paper'
    output = args.output.resolve()
    done, plan = read(run / 'complete.json'), read(run / 'plan.json')
    require(done['complete'] and done['profile'] == 'paper' and done['cases'] == 21, 'Full paper completion required')
    require(plan['source_identity'] == {'commit': '75aedd433d1b129e5a973635320f199b5f26cbae', 'status': ''}, 'Unexpected source identity')
    export = Path(done['export']).resolve()
    require(export.is_relative_to(run), 'Export must belong to this run')
    require(done['plan_sha256'] == digest(run / 'plan.json'), 'Final plan hash mismatch')
    require(done['shards_sha256'] == digest(run / 'shards.json'), 'Final shard hash mismatch')
    require(done['export_receipt_sha256'] == digest(export / 'complete.json'), 'Final export receipt hash mismatch')
    require(read(run / 'shards.json')['plan_sha256'] == done['plan_sha256'], 'Shard plan differs')
    states = read(run / 'scheduler/states.json')
    submitted = read(run / 'submissions.json')
    current_jobs = {items[-1]['job_id'] for items in submitted['workers'].values()} | {submitted['exports'][-1]['job_id']}
    require(set(states) == current_jobs and len(states) == 7 and all(x['state'] == 'COMPLETED' and x['exit_code'] == '0:0' for x in states.values()), 'All current scheduler jobs must complete successfully')
    canonical = read(export / 'canonical.json')
    require(canonical['complete'] and canonical['profile'] == 'paper', 'Completed paper canonical data required')
    expected = {(d, p) for d in (9, 13, 17, 21, 25, 29, 33) for p in ('0.0005', '0.001', '0.003')}
    stages = set(canonical['ablation_stages'])
    require(len(canonical['native']) == 126 and len(canonical['external']) == 105 and len(canonical['predecoder']) == 21, 'Incorrect matrix row counts')
    for group, shots, methods in [('native', 1000000, stages), ('external', 10000, {'parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf'})]:
        rows = canonical[group]
        keys = {(x['distance'], str(x['error_rate']), x['method']) for x in rows}
        require(keys == {(d, p, m) for d, p in expected for m in methods}, group + ' matrix mismatch')
        for row in rows:
            require(row['shots'] == shots and row['repeats'] == 3, group + ' shot/repeat mismatch')
            actual = sum(row['repeat_elapsed_ns']) / (shots * 3 * 1000)
            require(math.isclose(actual, row['mean_us'], rel_tol=1e-12), 'Mean does not match integer timings')
    for row in canonical['predecoder']:
        require(row['shots'] == 1000000 and row['repeats'] == 3 and row['execution'] == 'serial', 'Invalid predecoder scope')
        require(row['physical_policy'] == {0: 0, 1: 1, 2: 3}[row['policy']], 'Wrong physical predecoder policy')
    require({(r['distance'], str(r['error_rate']), r['policy']) for r in canonical['predecoder']} ==
            {(d, '0.003', p) for d in (9, 13, 17, 21, 25, 29, 33) for p in (0, 1, 2)}, 'Predecoder matrix mismatch')
    receipt = read(export / 'complete.json')
    require(receipt['sources']['shards_manifest']['manifest']['sha256'] == done['shards_sha256'], 'Exporter consumed different shards')
    for name, sha in receipt['files'].items():
        require(digest(export / name) == sha, 'Export hash mismatch: ' + name)
    require(not (output / 'canonical.json').exists(), 'Archive already exists; do not overwrite')
    output.mkdir(parents=True, exist_ok=True)
    sources = {}
    def copy(source, target):
        target = output / target
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        sources[str(target.relative_to(output))] = {'original_path': str(source), 'sha256': digest(source)}
    copy(Path(__file__).resolve(), 'evidence/archive-driver.py')
    for name in receipt['files']:
        copy(export / name, name)
    copy(export / 'complete.json', 'export-complete.json')
    # Preserve native raw batch rows and external validated summaries; full
    # per-shot inputs and outputs remain at their original durable paths.
    keep = {'measurement.csv', 'measurement_scope.json', 'correctness.json',
            'completion.json', 'process_usage.json', 'pmu.json', 'syndromes.json',
            'execution.csv', 'execution_threads.csv', 'kernel_selection.csv',
            'warmup.json', 'placement.json', 'clock.json', 'native_blocks.csv',
            'setup.json', 'storage.json', 'differences.csv', 'case.csv',
            'command.json', 'exit.json', 'run.json', 'summary.json', 'summary.csv',
            'input-provenance.json', 'failure.json', 'process.log',
            'witness_edges.csv', 'witness_checks.csv'}
    for case in sorted((run / 'cases').iterdir()):
        for name in ('config.json', 'state.json', 'complete.json'):
            copy(case / name, Path('evidence/cases') / case.name / name)
        for category in ('runs', 'capabilities'):
            if not (case / category).is_dir():
                continue
            for stage in sorted((case / category).iterdir()):
                prefix = Path('evidence/cases') / case.name / category / stage.name
                for name in ('plan.json', 'complete.json'):
                    if (stage / name).is_file():
                        copy(stage / name, prefix / name)
                for source in sorted(stage.rglob('*')):
                    if source.is_file() and (source.name in keep or source.name.endswith(('.result.json', '-command.json'))):
                        copy(source, prefix / source.relative_to(stage))
        for source in (case / 'logs').glob('*.log'):
            copy(source, Path('evidence/cases') / case.name / 'logs' / source.name)
    for name in ('plan.json', 'submissions.json', 'shards.json', 'complete.json'):
        if (run / name).is_file():
            copy(run / name, Path('evidence/slurm') / name)
    for source in (run / 'scheduler').iterdir():
        if source.is_file():
            copy(source, Path('evidence/slurm/scheduler') / source.name)
    build_root = root / 'shared-build'
    copy(build_root / 'builds.json', 'evidence/builds/builds.json')
    for source in sorted((build_root / 'builds').rglob('build.json')):
        copy(source, Path('evidence/builds') / source.relative_to(build_root / 'builds'))
    for source in sorted((build_root / 'builds').rglob('reference.json')):
        copy(source, Path('evidence/builds') / source.relative_to(build_root / 'builds'))
    for name in ('receipt.json', 'python-requirements.log', 'python-imports.log'):
        copy(root / 'package-build-attempt3' / name, Path('evidence/python') / name)
    for name in ('build-frozen-202606.log', 'build-frozen-202608.log', 'build-frozen-202609.log'):
        copy(root / name, Path('evidence/build-attempts') / name)
    for name in ('receipt.json', 'wheel.log'):
        source = root / 'package-build-attempt2' / name
        if source.is_file():
            copy(source, Path('evidence/build-attempts/attempt2') / name)
    for name in ('portable-ae-clean-env-tests.log', 'external-tests-with-oracle.log', 'frozen-wheel-tests.log', 'run-audit.json', 'repository-self-containment-audit.json', 'main-matrix-audit.json', 'main-matrix-audit.py', 'policy-matrix-audit.json', 'pmu-audit.json'):
        if (root / name).is_file():
            copy(root / name, Path('evidence/validation') / name)
    historical = read(root / 'main-matrix-audit.json')['historical_role_comparison']
    require(digest(Path(historical['path'])) == historical['sha256'], 'Historical comparison source changed')
    copy(Path(historical['path']), 'evidence/validation/historical-role-reference.csv')
    copy(root / 'native-mode3-validation/receipt.json', 'evidence/validation/mode3-native.json')
    for name in ('local-smoke/complete.json', 'slurm-smoke/complete.json', 'archived-figures-clean-env/complete.json'):
        copy(root / name, Path('evidence/validation') / name)
    for source in (root / 'slurm-smoke').glob('job-*.txt'):
        copy(source, Path('evidence/validation/slurm-smoke') / source.name)
    provenance = {'schema': 1, 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'source_commit': '75aedd433d1b129e5a973635320f199b5f26cbae',
                  'persistent_raw_root': str(root), 'files': sources,
                  'scope': 'Byte-preserved native raw batch rows, external validated summaries and compact receipts. Large inputs, per-shot outputs and binaries stay in the durable raw run. Recollection uses bundled sources and recorded seeds.'}
    (output / 'archive-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    manifest = {'schema': 1, 'source_commit': provenance['source_commit'],
                'exporter_sha256': digest(root / 'source/AE/export_results.py'),
                'files': {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob('*')) if p.is_file() and p != output / 'manifest.json' and '__pycache__' not in p.parts}}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Archived', len(sources), 'evidence files into', output)

if __name__ == '__main__':
    main()
