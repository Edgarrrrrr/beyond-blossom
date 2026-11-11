#!/usr/bin/env python3
"""Freeze or execute the isolated five-arm, three-case, eight-repeat protocol.

Only --execute calls native executables. --prepare-only and --self-test do not.
"""
from __future__ import annotations
import argparse
import contextlib
import csv
from datetime import datetime, timezone
import hashlib
import importlib.util
import itertools
import io
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True
BASE = Path(__file__).resolve().parent
C5DA = 'c5da46e12b3865e20d5a8c38157959ffefe8f417'
EDD = 'edd29b6780713dc78f6a99d67c4c4d192e9c52b5'
OFFICIAL = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
ARMS = ('source-sealed', 'current-sealed', 'rebuilt-current', 'outlined-boundary', 'official-sealed')
CASES = (('d17-p0.003', 'component-parallel'), ('d17-p0.001', 'pipelined'), ('d33-p0.0005', 'parallel-predecode'))
REPEATS = 8
PATCH_TARGET = 'src/beyond_blossom/structured/matcher/mwpm.inl'
HELPERS = ('AE/reproduce.py', 'benchmarks/regression/run_structured.py')
FIELDS = ('schedule_index', 'repeat', 'case', 'method', 'arm', 'arm_position', 'threads', 'shots', 'elapsed_ns', 'ns_per_shot', 'observable_differences', 'actual_candidate_witnesses', 'dataset_sha256', 'directory')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''): h.update(part)
    return h.hexdigest()


def read(path): return json.loads(Path(path).read_text())


def write(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False); stream.write('\n')


def need(condition, message):
    if not condition: raise ValueError(message)


def utc(): return datetime.now(timezone.utc).isoformat()


def arm_orders():
    rotations = [ARMS[i:] + ARMS[:i] for i in range(5)]
    orders = rotations + [tuple(reversed(row)) for row in rotations[:3]]
    assert len(orders) == REPEATS
    assert all(sum(row[p] == arm for row in orders) in (1, 2) for arm in ARMS for p in range(5))
    return orders


def schedule():
    rows = []
    for repeat, arms in enumerate(arm_orders(), 1):
        offset = (repeat - 1) % len(CASES)
        for case, method in CASES[offset:] + CASES[:offset]:
            for position, arm in enumerate(arms, 1):
                rows.append(dict(schedule_index=len(rows) + 1, repeat=repeat, case=case,
                                 method=method, arm=arm, arm_position=position))
    return rows


def helper_module(directory):
    # Import only our sealed/copy-on-prepare helper bytes, never the mutable repo.
    sys.dont_write_bytecode = True
    sys.modules.pop('reproduce', None)
    path = Path(directory) / 'benchmarks/regression/run_structured.py'
    spec = importlib.util.spec_from_file_location('confirmatory_structured_helpers', path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def clean_placement(path):
    with Path(path).open(newline='') as stream:
        return [{k: v for k, v in row.items() if k != 'tid'} for row in csv.DictReader(stream)]


def native_contract(directory):
    directory = Path(directory)
    contract = {'measurement_scope': read(directory / 'measurement_scope.json'),
                'placement': clean_placement(directory / 'execution_threads.csv')}
    kernel = directory / 'kernel_selection.csv'
    if kernel.exists(): contract['kernel_selection'] = kernel.read_text()
    return contract


def check_patch(current, variant):
    for record in (current, variant):
        need(record.get('base_commit') == C5DA and record.get('runtime_commit') == EDD, 'Unexpected rebuild source identity')
        need(record.get('source_files'), 'Rebuild receipt lacks source files')
    a, b = current['source_files'], variant['source_files']
    need(set(a) == set(b), 'Variant added or removed source paths')
    need([p for p in a if a[p] != b[p]] == [PATCH_TARGET], 'Variant must change only the declared boundary file')
    need(current.get('variant_patch_sha256') is None, 'Rebuilt control has a patch')
    patch = Path(variant['variant_patch'])
    need(digest(patch) == variant.get('variant_patch_sha256'), 'Variant patch hash mismatch')
    text = patch.read_text(); lines = text.splitlines()
    targets = [line[4:].split('\t')[0].removeprefix('b/') for line in lines if line.startswith('+++ ')]
    need(targets == [PATCH_TARGET], 'Patch target does not match the declared file')
    added = [line[1:] for line in lines if line.startswith('+') and not line.startswith('+++')]
    removed = [line[1:] for line in lines if line.startswith('-') and not line.startswith('---')]
    normalized = [line.replace('[[gnu::noinline]]', '').strip() for line in added]
    need(sum(line.count('[[gnu::noinline]]') for line in added) == 1, 'Expected exactly one noinline attribute')
    need([line for line in normalized if line] == [line.strip() for line in removed if line.strip()], 'Patch changes more than the noinline attribute')
    need('Mwpm::handle_tree_hitting_boundary(' in text, 'Patch does not identify the selected boundary function')
    return patch


def make_parser():
    p = argparse.ArgumentParser(description=__doc__)
    action = p.add_mutually_exclusive_group(required=True)
    action.add_argument('--prepare-only', action='store_true')
    action.add_argument('--execute', action='store_true')
    action.add_argument('--summarize-only', action='store_true')
    action.add_argument('--self-test', action='store_true')
    p.add_argument('--output', type=Path)
    p.add_argument('--arms-root', type=Path, default=BASE / 'arms')
    p.add_argument('--source-snapshot', type=Path, default=BASE / 'source')
    p.add_argument('--source-record', type=Path, default=BASE / 'source-snapshot.json')
    p.add_argument('--original-run', type=Path, default=Path('/tmp/beyond-build/structured-final-run'))
    return p


def prepare(args):
    root = args.output.resolve()
    need(not root.exists(), 'Prepare requires a new output directory')
    source = args.original_run.resolve(); old_plan = read(source / 'plan.json'); old_done = read(source / 'complete.json')
    need(old_done.get('complete') and old_done['plan_sha256'] == digest(source / 'plan.json') and old_done['raw_sha256'] == digest(source / 'raw.csv'), 'Original run is not sealed')
    need(old_plan['reference']['upstream_revision'] == OFFICIAL and old_plan['shots'] == 10000, 'Original input/reference protocol differs')
    need(old_plan['host'] == platform.node(), 'Confirmatory run requires the original host')
    snap = read(args.source_record)
    need(snap['base_commit'] == C5DA and snap['runtime_commit'] == EDD, 'Wrong helper source snapshot')
    for rel in HELPERS:
        need(digest(args.source_snapshot / rel) == snap['source_files'][rel], 'Frozen AE helper differs: ' + rel)
    identities = {arm: read(args.arms_root / arm / 'identity.json') for arm in ARMS}
    for arm, identity in identities.items():
        need(digest(args.arms_root / arm / 'benchmark') == identity['binary_sha256'], 'Arm binary differs: ' + arm)
    for arm, role in (('source-sealed', 'source'), ('current-sealed', 'candidate'), ('official-sealed', 'reference')):
        need(identities[arm]['binary_sha256'] == old_plan['binaries'][role]['sha256'], 'Sealed arm differs from original run: ' + arm)
    patch = check_patch(identities['rebuilt-current'], identities['outlined-boundary'])
    root.mkdir(parents=True)
    try:
        for rel in HELPERS:
            dest = root / 'helpers' / rel; dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.source_snapshot / rel, dest)
        h = helper_module(root / 'helpers'); ae = h.ae
        h.validate_cpus(old_plan['cpus'], 8)
        files = {}
        def copy(src, rel, executable=False):
            dst = root / rel; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(src, dst)
            if executable: dst.chmod(0o755)
            files[rel] = digest(dst)
            return dst
        for rel in HELPERS: files['helpers/' + rel] = digest(root / 'helpers' / rel)
        copy(__file__, 'controller/run_confirmatory.py'); copy(BASE / 'PROTOCOL.md', 'controller/PROTOCOL.md')
        copy(BASE / 'PROTOCOL.r1-four-arm.md', 'controller/PROTOCOL.r1-four-arm.md')
        copy(args.source_record, 'provenance/source-snapshot.json')
        copy(source / 'plan.json', 'provenance/original-plan.json'); copy(source / 'complete.json', 'provenance/original-complete.json')
        for arm in ARMS:
            copy(args.arms_root / arm / 'benchmark', f'bin/{arm}', True)
            copy(args.arms_root / arm / 'identity.json', f'identities/{arm}.json')
        copy(patch, 'identities/outlined-boundary.patch')
        cases = {}
        old_cases = {c['case']: c for c in old_plan['cases']}; old_inputs = {c['case']: c for c in old_done['inputs']}
        for case, method in CASES:
            original = old_cases[case]; record = old_inputs[case]
            ae.verify_relative_files(source, record['files'])
            for filename in ('dataset.bin', 'model.dem', 'circuit.stim'):
                rel = f'inputs/{case}/{filename}'
                need(rel in record['files'], 'Original input file lacks a seal: ' + rel)
                copy(source / rel, rel)
            header = h.dataset_header(root / 'inputs' / case / 'dataset.bin', original['distance'], original['error_rate'], 10000)
            options = dict(old_plan['methods'][method]); options.setdefault('backend', old_plan['backend'])
            need(options == dict(ae.METHODS[method], backend=old_plan['backend']), 'Frozen helper method differs from original')
            normal = source / 'calls' / case / '01' / method / 'source' / 'native'
            reference = source / 'calls' / case / '01' / 'reference' / 'reference' / 'native'
            for native in (normal, reference):
                rel = str(native.relative_to(source))
                sealed = [call for call in old_done['calls'] if call['directory'] == rel]
                need(len(sealed) == 1, 'Original native contract lacks a unique call receipt: ' + rel)
                ae.verify_relative_files(source, sealed[0]['files'])
                for filename in ('measurement_scope.json', 'execution_threads.csv'):
                    need(rel + '/' + filename in sealed[0]['files'], 'Original native contract file lacks a seal')
                if (native / 'kernel_selection.csv').exists():
                    need(rel + '/kernel_selection.csv' in sealed[0]['files'], 'Original kernel contract lacks a seal')
            cases[case] = dict(original, method=method, options=options, cpus=old_plan['cpus'][:options['threads']],
                               header=list(header), dataset_sha256=files[f'inputs/{case}/dataset.bin'],
                               native_contract=native_contract(normal), official_contract=native_contract(reference))
        calls = schedule()
        for item in calls:
            case = cases[item['case']]
            options = dict(ae.METHODS['reference']) if item['arm'] == 'official-sealed' else case['options']
            cpus = old_plan['cpus'][:options['threads']]
            command = [str(root / 'bin' / item['arm']), '--distance', str(case['distance']), '--error-rate', str(case['error_rate']),
                       '--shots', '10000', '--dataset', str(root / 'inputs' / item['case'] / 'dataset.bin'),
                       '--root-seed', str(old_plan['seed']), '--stream-id', str(case['stream']),
                       '--mode', 'pipe-batch' if options['execution'] == 'pipeline' else 'batch',
                       '--cpus', ','.join(map(str, cpus)), '--warmup-seconds', '2', '--verify', 'weight', '--block-size', '1024', '--repeats', '1']
            for key, value in options.items():
                if key != 'threads': command += ['--' + key.replace('_', '-'), str(value)]
            item['command'] = command; item['options'] = options
            item['directory'] = f'calls/{item["repeat"]:02d}/{item["case"]}/{item["arm"]}'
        plan = dict(schema=2, revision='five-arm-r2', created_utc=utc(), state='prepared', host=platform.node(), platform=platform.platform(),
                    base_commit=C5DA, runtime_commit=EDD, official_revision=OFFICIAL, original_run=str(source),
                    arms=list(ARMS), repeats=REPEATS, shots=10000, expected_calls=len(calls), cases=cases, schedule=calls,
                    comparisons=[['source-sealed', 'current-sealed'], ['current-sealed', 'rebuilt-current'], ['rebuilt-current', 'outlined-boundary']],
                    official_comparisons=[['official-sealed', arm] for arm in ARMS if arm != 'official-sealed'],
                    bit_identical_current_controls=identities['current-sealed']['binary_sha256'] == identities['rebuilt-current']['binary_sha256'],
                    schedule_scope='Five cyclic rotations followed by reversals of the first three; every arm/position appears once or twice, not perfect order balance',
                    timing=old_plan['timing'], correctness=old_plan['correctness'], files=files,
                    collector_sha256=digest(__file__), protocol_sha256=files['controller/PROTOCOL.md'])
        write(root / 'plan.json', plan)
        write(root / 'prepared.json', dict(state='prepared', complete=True, native_calls=0, plan_sha256=digest(root / 'plan.json'), files=files))
        print(f'Prepared {len(calls)} calls without executing native: {root}')
        return plan
    except Exception as error:
        write(root / 'prepare-failure.json', dict(state='prepare_failed', error=str(error), native_calls=0))
        raise


def verify_prepared(root):
    plan = read(root / 'plan.json'); receipt = read(root / 'prepared.json')
    need(plan['schema'] == 2 and receipt['state'] == 'prepared' and receipt['complete'] and receipt['native_calls'] == 0, 'Invalid prepared state')
    need(receipt['plan_sha256'] == digest(root / 'plan.json') and receipt['files'] == plan['files'], 'Prepared plan changed')
    need(plan['collector_sha256'] == digest(__file__), 'Collector changed since preparation')
    h = helper_module(root / 'helpers'); h.ae.verify_relative_files(root, plan['files'])
    need([{k: row[k] for k in schedule()[0]} for row in plan['schedule']] == schedule(), 'Frozen arm/case order differs')
    return plan, h


def validate_native(h, native, plan, item):
    case = plan['cases'][item['case']]; options = item['options']
    measured = h.measurement(native, shots=plan['shots'], options=options, header=case['header'])
    expected = case['official_contract'] if item['arm'] == 'official-sealed' else case['native_contract']
    need(native_contract(native) == expected, 'Actual kernel/scope/thread placement differs from frozen original contract')
    completion = read(native / 'completion.json')
    need(completion['final_drain_called'] == (options['execution'] == 'pipeline') and completion['final_drain_ns'] is None, 'Final drain timing contract differs')
    with (native / 'measurement.csv').open(newline='') as stream: row = next(csv.DictReader(stream))
    need(int(row['block_size']) == 1024, 'Native block size changed')
    with (native / 'native_blocks.csv').open(newline='') as stream: blocks = list(csv.DictReader(stream))
    need(len(blocks) == 10 and sum(int(b['count']) for b in blocks) == plan['shots'] and
         sum(int(b['elapsed_ns']) for b in blocks) == measured[0], 'Native block accounting differs')
    return measured


def execute(root, invoke=None):
    root = Path(root).resolve(); plan, h = verify_prepared(root); ae = h.ae
    need(not any((root / p).exists() for p in ('started.json', 'raw.csv', 'complete.json', 'failure.json')), 'Prepared run may execute once; use a fresh output after any started attempt')
    need(plan['host'] == platform.node(), 'Prepared host changed')
    h.validate_cpus(plan['cases'][CASES[-1][0]]['cpus'], 8)
    write(root / 'started.json', dict(state='running', started_utc=utc(), plan_sha256=digest(root / 'plan.json')))
    calls = []; item = None
    try:
        with (root / 'raw.csv').open('x', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=FIELDS); writer.writeheader(); stream.flush()
            for item in plan['schedule']:
                case = plan['cases'][item['case']]
                for rel in (f'bin/{item["arm"]}', f'inputs/{item["case"]}/dataset.bin'):
                    ae.verify_relative_files(root, {rel: plan['files'][rel]})
                native = (invoke or ae.invoke)(item['command'], root / item['directory'])
                elapsed, differences, witnesses = validate_native(h, native, plan, item)
                row = {k: item[k] for k in ('schedule_index', 'repeat', 'case', 'method', 'arm', 'arm_position')}
                row.update(threads=item['options']['threads'], shots=plan['shots'], elapsed_ns=elapsed, ns_per_shot=elapsed / plan['shots'],
                           observable_differences=differences, actual_candidate_witnesses=witnesses, dataset_sha256=case['dataset_sha256'],
                           directory=str(native.relative_to(root)))
                writer.writerow(row); stream.flush()
                calls.append(dict(schedule_index=item['schedule_index'], directory=item['directory'], files=h.artifact_files(root, root / item['directory'])))
                print(f'{item["schedule_index"]}/{plan["expected_calls"]} {item["case"]} {item["arm"]} passed', flush=True)
        ae.verify_relative_files(root, plan['files'])
        write(root / 'complete.json', dict(state='complete', complete=True, ended_utc=utc(), plan_sha256=digest(root / 'plan.json'),
                                          raw_sha256=digest(root / 'raw.csv'), prepared_sha256=digest(root / 'prepared.json'), calls=calls))
        return summarize(root)
    except Exception as error:
        write(root / 'failure.json', dict(state='failed', complete=False, failed_utc=utc(), completed_calls=len(calls),
                                         failed_call=item, error=str(error), calls=calls))
        raise


def summarize(root):
    root = Path(root).resolve(); plan, h = verify_prepared(root); done = read(root / 'complete.json')
    need(done.get('complete') and done['state'] == 'complete' and not (root / 'failure.json').exists(), 'Incomplete/failed run cannot be summarized')
    need(done['plan_sha256'] == digest(root / 'plan.json') and done['raw_sha256'] == digest(root / 'raw.csv') and
         done['prepared_sha256'] == digest(root / 'prepared.json'), 'Completed run seal changed')
    for call in done['calls']: h.ae.verify_relative_files(root, call['files'])
    with (root / 'raw.csv').open(newline='') as stream: raw = list(csv.DictReader(stream))
    need(len(raw) == len(done['calls']) == plan['expected_calls'] == 120, 'Missing native calls')
    groups = {}
    for row, item in zip(raw, plan['schedule']):
        need(all(str(item[k]) == row[k] for k in ('schedule_index', 'repeat', 'case', 'method', 'arm', 'arm_position')), 'Raw execution order differs from plan')
        need(int(row['shots']) == plan['shots'] and int(row['elapsed_ns']) > 0 and row['dataset_sha256'] == plan['cases'][row['case']]['dataset_sha256'], 'Invalid raw input/time')
        need(int(row['actual_candidate_witnesses']) >= int(row['observable_differences']) >= 0, 'Raw observable witness validation failed')
        groups.setdefault(row['case'], {}).setdefault(row['arm'], {})[int(row['repeat'])] = int(row['elapsed_ns']) / plan['shots']
    results = []
    for case, method in CASES:
        values = groups[case]; medians = {arm: statistics.median(values[arm].values()) for arm in ARMS}
        comparisons = []
        for baseline, candidate in plan['comparisons'] + plan['official_comparisons']:
            pairs = [values[baseline][k] / values[candidate][k] for k in range(1, REPEATS + 1)]
            comparisons.append(dict(baseline=baseline, candidate=candidate, baseline_median_ns=medians[baseline], candidate_median_ns=medians[candidate],
                                    speedup_ratio_of_medians=medians[baseline] / medians[candidate],
                                    candidate_change_percent=100 * (medians[candidate] / medians[baseline] - 1),
                                    paired_speedups=pairs, paired_speedup_median=statistics.median(pairs), paired_speedup_range=[min(pairs), max(pairs)]))
        results.append(dict(case=case, method=method, distinct_saved_records=plan['shots'], process_repeats=REPEATS,
                            arm_ns_per_shot={arm: [values[arm][k] for k in range(1, REPEATS + 1)] for arm in ARMS},
                            arm_median_ns=medians, arm_observed_range_ns={arm: [min(values[arm].values()), max(values[arm].values())] for arm in ARMS}, comparisons=comparisons))
    result = dict(schema=2, complete=True, scope='Three selected regression cases; fixed eight process repeats; no outlier deletion or early performance stopping',
                  plan_sha256=done['plan_sha256'], raw_sha256=done['raw_sha256'], official_revision=OFFICIAL,
                  bit_identical_current_controls=plan['bit_identical_current_controls'],
                  interpretation='Official/candidate comparisons retain the fixed official normal path; single-function attribution uses rebuilt-current/outlined-boundary. Identical current binaries, when observed, control repeated-run variation rather than code generation.',
                  aggregation='Median of eight complete-batch means; ratios of medians and paired ratios are distinct. Ranges are descriptive, not confidence intervals.', results=results)
    path = root / 'summary.json'
    if path.exists(): need(read(path) == result, 'Existing summary disagrees with sealed measurements')
    else: write(path, result)
    print(f'Complete 120-call summary: {path}')
    return result


def self_test():
    rows = schedule()
    assert len(rows) == 120
    assert len({(r['repeat'], r['case'], r['arm']) for r in rows}) == 120
    for start in range(0, 120, 5):
        assert len({r['case'] for r in rows[start:start + 5]}) == 1
    counts = {arm: [sum(order[p] == arm for order in arm_orders()) for p in range(5)] for arm in ARMS}
    assert all(n in (1, 2) for positions in counts.values() for n in positions)
    assert all(sum(order.index(a) < order.index(b) for order in arm_orders()) in (3, 4, 5)
               for a, b in itertools.combinations(ARMS, 2))
    with tempfile.TemporaryDirectory(prefix='boundary-confirmatory-mock-') as temp:
        root = Path(temp); target = root / PATCH_TARGET; target.parent.mkdir(parents=True)
        patch = root / 'change.patch'
        patch.write_text('--- a/' + PATCH_TARGET + '\n+++ b/' + PATCH_TARGET + '\n@@ -1 +1 @@\n-inline void Mwpm::handle_tree_hitting_boundary(const X& x) {\n+[[gnu::noinline]] inline void Mwpm::handle_tree_hitting_boundary(const X& x) {\n')
        a = dict(base_commit=C5DA, runtime_commit=EDD, source_files={PATCH_TARGET:'a'}, variant_patch_sha256=None)
        b = dict(base_commit=C5DA, runtime_commit=EDD, source_files={PATCH_TARGET:'b'}, variant_patch=str(patch), variant_patch_sha256=digest(patch))
        assert check_patch(a, b) == patch
        b['source_files']['another.cc'] = 'unexpected'
        try: check_patch(a, b)
        except ValueError: pass
        else: raise AssertionError('Additional source path accepted')
    mock_checks = mock_control_test()
    print(json.dumps({'self_test':'passed','native_executions':0,'calls':len(rows),'position_counts':counts,'mock_checks':mock_checks}))


def mock_control_test():
    """Exercise full control flow with tiny files and Python-generated receipts."""
    real_helper = helper_module(BASE / 'source')
    helper = SimpleNamespace(ae=real_helper.ae, measurement=real_helper.measurement,
                             artifact_files=real_helper.artifact_files, validate_cpus=lambda *a: None)
    original_verifier = globals()['verify_prepared']
    checks = []
    with tempfile.TemporaryDirectory(prefix='boundary-confirmatory-flow-') as temp:
        def fixture(name):
            root = Path(temp) / name; root.mkdir(); (root / 'bin').mkdir()
            files = {}
            for arm in ARMS:
                path = root / 'bin' / arm; path.write_text('mock binary ' + arm)
                files[str(path.relative_to(root))] = digest(path)
            cases = {}
            for case, method in CASES:
                path = root / 'inputs' / case / 'dataset.bin'; path.parent.mkdir(parents=True); path.write_text('mock saved input ' + case)
                files[str(path.relative_to(root))] = digest(path)
                options = dict(helper.ae.METHODS[method], backend='avx512vl')
                def contract(mode): return dict(measurement_scope={'mode':mode, 'internal_instrumentation':False}, placement=[])
                cases[case] = dict(options=options, cpus=[2,4,6,8,10,12,14,16][:options['threads']],
                                   header=[0]*7+[123,456,0,0,0], dataset_sha256=digest(path),
                                   native_contract=contract('pipe-batch' if options['execution']=='pipeline' else 'batch'), official_contract=contract('batch'))
            calls = schedule()
            for item in calls:
                options = dict(helper.ae.METHODS['reference']) if item['arm']=='official-sealed' else cases[item['case']]['options']
                item.update(options=options, directory=f'calls/{item["schedule_index"]:03d}', command=[str(root/'bin'/item['arm'])])
            plan = dict(schema=2, host=platform.node(), cases=cases, files=files, schedule=calls, expected_calls=120,
                        shots=10000, bit_identical_current_controls=False,
                        comparisons=[['source-sealed','current-sealed'],['current-sealed','rebuilt-current'],['rebuilt-current','outlined-boundary']],
                        official_comparisons=[['official-sealed',a] for a in ARMS if a!='official-sealed'])
            write(root/'plan.json',plan);write(root/'prepared.json',{'mock':True})
            return root,plan
        def exercise(name, fail_at=None, tamper=None):
            root,plan=fixture(name);seen=[]
            globals()['verify_prepared']=lambda _: (plan,helper)
            def invoke(command,directory):
                item=plan['schedule'][len(seen)];seen.append(item['schedule_index']);directory.mkdir(parents=True)
                native=directory/'native';native.mkdir();options=item['options'];mode='pipe-batch' if options['execution']=='pipeline' else 'batch'
                write(directory/'command.json',command);write(directory/'exit.json',{'returncode':0})
                write(native/'correctness.json',dict(status='passed',shots=10000,weight_differences=1 if fail_at==len(seen) else 0,observable_differences=0,actual_candidate_witnesses=0))
                write(native/'warmup.json',dict(requested_seconds=2,actual_elapsed_ns=2000000000,shots=1024))
                write(native/'measurement_scope.json',dict(mode=mode,internal_instrumentation=False))
                write(native/'completion.json',dict(final_drain_called=options['execution']=='pipeline',final_drain_ns=None))
                (native/'execution_threads.csv').write_text('tid,cpu\n')
                row=dict(shots=10000,elapsed_ns=100000,mode=mode,execution=options['execution'],total_threads=options['threads'],weight_diff=0,observable_diff=0,syndrome_hash=123,reference_result_hash=456,block_size=1024)
                with (native/'measurement.csv').open('w',newline='') as stream:
                    w=csv.DictWriter(stream,fieldnames=row);w.writeheader();w.writerow(row)
                with (native/'native_blocks.csv').open('w',newline='') as stream:
                    w=csv.DictWriter(stream,fieldnames=['count','elapsed_ns']);w.writeheader()
                    for n in range(10):w.writerow({'count':1024 if n<9 else 784,'elapsed_ns':10000})
                return native
            if tamper:
                changed = root/'bin'/ARMS[0] if tamper == 'binary' else root/'inputs'/CASES[0][0]/'dataset.bin'
                changed.write_text('changed')
            with contextlib.redirect_stdout(io.StringIO()):
                try: result=execute(root,invoke=invoke)
                except ValueError:
                    if fail_at is None and not tamper:raise
                    assert (root/'failure.json').exists() and not (root/'summary.json').exists() and not (root/'complete.json').exists()
                    assert len(seen)==(0 if tamper else fail_at)
                    return
            assert fail_at is None and not tamper and result['complete'] and seen==list(range(1,121))
            assert len(result['results'])==3 and len(result['results'][0]['arm_ns_per_shot']['official-sealed'])==8
            with contextlib.redirect_stdout(io.StringIO()):
                try:execute(root,invoke=invoke)
                except ValueError:pass
                else:raise AssertionError('Completed output executed twice')
        try:
            exercise('success');checks.append('all120 ordered calls, official arm, complete summary, repeat-execute rejection')
            exercise('correctness-failure',fail_at=4);checks.append('correctness failure stops at call4 and suppresses summary')
            exercise('changed-binary',tamper='binary');checks.append('changed binary stops before native call')
            exercise('changed-input',tamper='input');checks.append('changed saved input stops before native call')
        finally:globals()['verify_prepared']=original_verifier
    return checks


def main():
    args = make_parser().parse_args()
    if args.self_test: self_test(); return
    if not args.output: raise ValueError('--output is required')
    if args.prepare_only: prepare(args)
    elif args.summarize_only: summarize(args.output)
    else:
        if not args.output.exists(): prepare(args)
        execute(args.output)


if __name__ == '__main__':
    try: main()
    except Exception as error:
        print(f'error: {error}', file=sys.stderr); raise SystemExit(1)
