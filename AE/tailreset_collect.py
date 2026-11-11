#!/usr/bin/env python3
"""Freeze/build/collect/analyze current TailReset's three compile-time predecode policies."""
import argparse
import csv
import itertools
import math
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import traceback
import pmu_runtime as R

HERE = Path(__file__).resolve().parent
POLICIES = {'degree': 0, 'skip': 1, 'coordinate': 2}
METHODS = ['skip', 'coordinate', 'degree', 'Official']
NODES = [f'ares{i}' for i in range(1, 7)]
DISTANCES = [9, 13, 17, 21, 25, 29, 33]


def git(*args):
    return subprocess.check_output(['git', *args], cwd=HERE, text=True).strip()


def table(path):
    with Path(path).open(newline='') as stream:
        return list(csv.DictReader(stream))


def write_csv(path, rows):
    rows = list(rows)
    with Path(path).open('x', newline='') as stream:
        writer = csv.DictWriter(stream, list(rows[0]), lineterminator='\n')
        writer.writeheader(); writer.writerows(rows)


def freeze(args):
    root = args.output.resolve()
    nodes = args.nodes.split(',')
    R.need(nodes and len(nodes) == len(set(nodes)) and set(nodes) <= set(NODES), 'Choose unique nodes from ares1–6')
    R.need(len(nodes) >= 3, 'At least three nodes for the separate policy builds')
    R.need(not root.exists() and not args.cache_dir.exists(), 'Use fresh output/cache directories')
    R.need(not git('status', '--porcelain'), 'Commit implementation and collector before freezing')
    tool_runtime = None
    if args.tool_runtime:
        tool_root = args.tool_runtime.resolve()
        manifest = R.load(tool_root/'manifest.json')
        for name, digest in manifest['files'].items(): R.ref(tool_root/name,digest)
        tool_runtime = dict(root=str(tool_root),manifest=R.ref(tool_root/'manifest.json'),files=manifest['files'])
    repo = Path(git('rev-parse', '--show-toplevel'))
    R.need(len(args.implementation)==40 and git('rev-parse',args.implementation+':src')==git('rev-parse','HEAD:src'), 'Implementation commit must identify the current source tree')
    reference = args.reference_run.resolve()
    parent = R.load(reference / 'plan.json')
    R.need(parent['shots'] == 1_000_000 and parent['protocol']['lto'] == 'OFF', 'Reference protocol differs')
    cases = [c for c in parent['cases'] if c['p'] == .003]
    R.need([c['d'] for c in cases] == DISTANCES and all(c['rounds'] == c['d'] and c['sigma'] == 0 for c in cases), 'Need seven uniform p=0.003 settings')
    old_build = R.load(parent['build']['path'])
    R.ref(parent['build']['path'], parent['build']['sha256'])
    stim = old_build['libraries']['stim']
    R.ref(stim['path'], stim['sha256'])
    official = parent['variants']['official']
    R.ref(official['binary']['path'], official['binary']['sha256'])
    R.need(official['implementation_commit'] == 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd', 'Require fixed stock official baseline')
    root.mkdir(parents=True)
    source = root / 'source'
    source.mkdir()
    checkout = source / 'checkout'
    checkout.mkdir()
    shutil.copytree(repo / 'src', checkout / 'src', ignore=shutil.ignore_patterns('__pycache__','*.pyc','*.so'))
    shutil.copytree(repo / 'tests', checkout / 'tests')
    shutil.copytree(repo / 'data', checkout / 'data')
    shutil.copy2(repo / 'CMakeLists.txt', checkout / 'CMakeLists.txt')
    ae = source / 'AE'; ae.mkdir()
    for name in ['tailreset_collect.py', 'tailreset_statistics.h', 'tailreset.sbatch', 'pmu_runtime.py']:
        shutil.copy2(HERE / name, ae / name)
    for name in ['pmu_native', 'tailreset_native']:
        shutil.copytree(HERE / name, ae / name)
    # Disabled instrumentation is checked against the adopted source, not a historical ELF.
    baseline = source / 'baseline'; baseline.mkdir()
    changed = ['predecode/one_hop_predecoder.cc', 'driver/shared_component_decoder.cc']
    base = '6b29edfd251e84a5cb752c7048a71d98dc53f76d'
    for name in changed:
        target = baseline / Path(name).name
        target.write_bytes(subprocess.check_output(['git', 'show', base + ':src/pymatching/sparse_blossom/structured/' + name], cwd=repo))
    units = []
    for case in cases:
        for repeat in range(1, 4):
            units.append(dict(case=case['id'], phase='batch', repeat=repeat, cost=case['historical_batch_us'] * 2.2))
        units.append(dict(case=case['id'], phase='latency', repeat=1, cost=case['historical_batch_us'] * 2.2))
        units.append(dict(case=case['id'], phase='statistics', repeat=1, cost=case['historical_batch_us'] * 1.2))
        inputs = root / 'inputs' / case['id']; inputs.mkdir(parents=True)
        (inputs / 'dataset.bin').symlink_to(case['inputs']['dataset']['path'])
        model = reference / 'inputs' / case['id'] / 'model.dem'
        R.ref(model, case['original_dem_sha256'])
        shutil.copy2(model, inputs / 'model.dem')
    lanes = [dict(node=n, units=[], cost=0) for n in nodes]
    for unit in sorted(units, key=lambda u: (-u['cost'], u['case'], u['phase'], u['repeat'])):
        lane = min(lanes, key=lambda x: (x['cost'], x['node']))
        lane['units'].append(unit); lane['cost'] += unit['cost']
    plan = dict(experiment_id='AE-TAILRESET-20261110', run_id=root.name, created_utc=R.utc(),
        acquisition_commit=git('rev-parse', 'HEAD'), implementation_commit=args.implementation,
        source_tree=git('rev-parse', 'HEAD:src'), adopted_base=base,
        reference_plan=R.ref(reference / 'plan.json'), official=official, stim=stim,
        stim_source=str(args.stim_source.resolve()), gtest_source=str(args.gtest_source.resolve()),
        dependency_files={str(p.resolve()):R.ref(p) for p in [args.stim_source/'CMakeLists.txt',args.gtest_source/'CMakeLists.txt',
            *sorted((args.stim_source/'src').rglob('*.h')),*sorted((args.stim_source/'src').rglob('*.inl')),
            *sorted((args.gtest_source/'googletest/include').rglob('*.h')),
            *sorted((args.gtest_source/'googlemock/include').rglob('*.h'))]},
        dependency_revisions=dict(stim='1320ad7eac7de34d2e9c70daa44fbc6d84174450', googletest='58d77fa8070e8cec2dc1ed015d66b454c8d78850'),
        python=sys.executable, cache_dir=str(args.cache_dir.resolve()), cases=cases,
        tool_runtime=tool_runtime,
        policies=POLICIES, build_nodes=dict(zip(POLICIES, nodes[:3])), lanes=lanes, analysis_node=nodes[0],
        source_files={str(p.relative_to(source)):R.ref(p) for p in sorted(source.rglob('*')) if p.is_file()},
        protocol=dict(shots=1_000_000, batch_repeats=3, latency_repeats=1, statistics_repeats=1,
            lto='OFF', backend='avx512vl', timing_statistics_macro=0, statistics_macro=1,
            warmup_seconds=2, warmup_prefix=1024, block_size=1024, measurement_cpus=[0,1,2,3],
            controller_cpu=8, numa_node=0, exclusive=True, allowed_nodes=NODES,
            timing_scope='synchronous full decode, dispatch/waits/result stores and reset; final drain inside last batch block; latency final drain separately included in complete total; one resident OpenMP team; setup/warmup/checking/CSV excluded',
            remaining_scope='normalized unmatched seeds in both X/Z components after actual pipeline joined prefix, before MWPM; separate instrumented full replay; summed and averaged per independent shot',
            ordering='same unit pairs all methods on one host; cyclic first method by distance and repeat; one job per node',
            correctness='all shot IDs/events/weights checked; equal optimum observable ties require actual candidate parity/weight/observable witness; all differences retained',
            latency_summary='nearest-rank p50/p90/p99/p99.9/max, raw per-shot CSV retained; one latency pass, no Min3 claim',
            stopping='all seven settings; all three policies; no performance-dependent stopping or outlier removal'),
        timeout_seconds_per_call=7200)
    R.write(root / 'plan.json', plan)
    R.write(root / 'freeze.json', dict(complete=True, plan=R.ref(root / 'plan.json')))
    print(root / 'plan.json', flush=True)


def load(root):
    receipt = R.load(root / 'freeze.json')
    R.ref(root / 'plan.json', receipt['plan']['sha256'])
    plan = R.load(root / 'plan.json')
    R.need(plan['policies'] == POLICIES and plan['protocol']['shots'] == 1_000_000, 'Wrong protocol')
    for name, ref in plan['source_files'].items():
        R.ref(root / 'source' / name, ref['sha256'])
    for ref in plan['dependency_files'].values(): R.ref(ref['path'],ref['sha256'])
    if plan.get('tool_runtime'):
        t=plan['tool_runtime']; R.ref(t['manifest']['path'],t['manifest']['sha256'])
        for name,digest in t['files'].items(): R.ref(Path(t['root'])/name,digest)
    return plan


def machine(node):
    R.need(node in NODES and os.uname().nodename.split('.')[0] == node, 'Forbidden/wrong node')
    job = subprocess.check_output(['scontrol', 'show', 'job', os.environ['SLURM_JOB_ID']], text=True)
    R.need('OverSubscribe=NO' in job, 'Exclusive allocation required')
    allocation = sorted(os.sched_getaffinity(0))
    R.need(set([0,1,2,3,8]) <= set(allocation), 'Frozen CPU lane unavailable')
    identities = []
    for cpu in [0,1,2,3,8]:
        p = Path(f'/sys/devices/system/cpu/cpu{cpu}')
        R.need((p / 'node0').exists(), 'Require NUMA0')
        identities.append(((p/'topology/physical_package_id').read_text(), (p/'topology/core_id').read_text()))
    R.need(len(set(identities)) == 5, 'Five distinct physical cores required')
    info = Path('/proc/cpuinfo').read_text()
    R.need('Intel(R) Xeon(R) Gold 6230R' in info, 'Frozen hardware differs')
    os.sched_setaffinity(0, {8})
    return dict(host=node, job_id=os.environ['SLURM_JOB_ID'], job=job, cpuinfo=info,
        allocation_affinity=allocation, numa_node=0, kernel=os.uname().release,
        topology=subprocess.check_output(['lscpu'], text=True))


def command(argv, log, cwd=None, env=None):
    with Path(log).open('x') as stream:
        p = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    R.need(p.returncode == 0, 'Command failed; see ' + str(log))


def disabled_check(root, cache, out):
    entries = R.load(cache / 'compile_commands.json')
    checks = []
    for name in ['one_hop_predecoder.cc', 'shared_component_decoder.cc']:
        entry = next(e for e in entries if e['file'].endswith('/' + name))
        argv = shlex.split(entry['command'])
        argv.remove('-c')
        argv += ['-E', '-P']
        products = []
        for variant, source in [('new', Path(entry['file'])), ('base', root/'source/baseline'/name)]:
            args = [str(source) if x == entry['file'] else x for x in argv]
            product = out / (name + '.' + variant + '.ii')
            args[args.index('-o')+1] = str(product)
            command(args, out/(name+'.'+variant+'.log'), entry['directory'])
            # Ignore preprocessing whitespace only, preserve all token/string bytes.
            products.append(' '.join(product.read_text().split()))
        R.need(products[0] == products[1], 'Disabled build changed preprocessed tokens: ' + name)
        checks.append(dict(file=name, token_stream_equal=True))
    symbols = subprocess.check_output(['nm', '-C', str(cache/'pymatching/libpymatching.a')], text=True)
    R.need('syndrome_statistics' not in symbols and 'TailResetSyndromeCounts' not in symbols, 'Statistics symbol leaked into pure core')
    R.write(out / 'disabled-instrumentation.json', dict(complete=True, comparisons=checks, statistics_symbols_absent=True))


def build(root, policy):
    plan = load(root); host = machine(plan['build_nodes'][policy])
    out = root/'build'/policy; out.mkdir(parents=True, exist_ok=False)
    R.write(out/'machine.json', host)
    R.ref(plan['stim']['path'], plan['stim']['sha256'])
    variants = {}
    build_env=os.environ.copy()
    if plan.get('tool_runtime'):
        tools=Path(plan['tool_runtime']['root'])
        build_env.update(PATH=str(tools/'bin')+':/usr/bin:/bin:/usr/local/bin',LD_LIBRARY_PATH=str(tools/'lib'))
        cmake=str(tools/'bin/cmake')
    else: cmake=shutil.which('cmake')
    R.need(cmake,'CMake is unavailable; pass --tool-runtime when freezing on the server')
    os.sched_setaffinity(0, set(host['allocation_affinity']))
    for stats in [False, True]:
        label = 'statistics' if stats else 'timing'
        cache = Path(plan['cache_dir'])/policy/label
        R.need(not cache.exists(), 'Build cache must be fresh')
        args = [cmake, '-S', str(root/'source/AE/tailreset_native'), '-B', str(cache),
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=/usr/bin/c++',
            '-DPYMATCHING_SOURCE_DIR='+str(root/'source/checkout'),
            '-DFETCHCONTENT_SOURCE_DIR_STIM='+plan['stim_source'],
            '-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST='+plan['gtest_source'],
            '-DCACHED_STIM_ARCHIVE='+plan['stim']['path'],
            '-DPYMATCHING_TAILRESET_PREDECODE_MODE='+str(POLICIES[policy]),
            '-DPYMATCHING_TAILRESET_SYNDROME_STATS='+('ON' if stats else 'OFF'),
            '-DPYMATCHING_BUILD_TESTS='+('ON' if policy == 'degree' and not stats else 'OFF')]
        command(args, out/(label+'-configure.log'), env=build_env)
        targets = ['tailreset_benchmark', 'tailreset_policy_test']
        if policy == 'degree' and not stats: targets += ['pymatching_tests']
        compile_argv = [cmake,'--build',str(cache),'--parallel','10','--target',*targets]
        command(compile_argv, out/(label+'-build.log'), env=build_env)
        command([str(cache/'bin/tailreset_policy_test')], out/(label+'-policy-test.log'))
        if policy == 'degree' and not stats:
            command([str(cache/'pymatching/pymatching_tests')], out/'core-tests.log', root/'source/checkout')
            disabled_check(root, cache, out)
        binary = out / label
        shutil.copy2(cache/'bin/tailreset_benchmark', binary)
        R.need('.gnu.lto_' not in subprocess.check_output(['readelf','-SW',str(binary)],text=True), 'LTO sections found')
        variants[label] = dict(binary=R.ref(binary), core=R.ref(cache/'pymatching/libpymatching.a'),
            configure=args, build=compile_argv, compile_commands=R.ref(cache/'compile_commands.json'),
            statistics=stats, policy_mode=POLICIES[policy])
        print('Built',policy,label,flush=True)
    os.sched_setaffinity(0, {8})
    R.write(out/'built.json', dict(complete=True, policy=policy, variants=variants))
    smoke = []
    for case in [plan['cases'][0], plan['cases'][-1]]:
        verify_inputs(root, case)
        for phase in ['batch','latency','statistics']:
            smoke.append(call(root, plan, host, case, policy, phase, 0, 1025, variants,
                root/'smoke'/policy/case['id']/phase))
    R.write(out/'ready.json', dict(complete=True, policy=policy, job_id=host['job_id'],
        plan=R.ref(root/'plan.json'), build=R.ref(out/'built.json'), smoke=smoke))


def verify_inputs(root, case):
    for ref in case['inputs'].values(): R.ref(ref['path'], ref['sha256'])
    R.need((root/'inputs'/case['id']/'dataset.bin').resolve() == Path(case['inputs']['dataset']['path']).resolve(), 'Dataset symlink changed')
    R.ref(root/'inputs'/case['id']/'model.dem', case['original_dem_sha256'])


def validate(out, case, method, phase, shots):
    stats = phase == 'statistics'
    correctness = R.load(out/'correctness.json')
    R.need(correctness['status'] == 'passed' and correctness['shots'] == shots and correctness['weight_differences'] == 0, 'Optimum validation failed')
    R.need(correctness['actual_candidate_witnesses'] >= correctness['observable_differences'], 'Missing observable tie witnesses')
    warmup = R.load(out/'warmup.json')
    R.need(warmup['requested_seconds'] == 2 and warmup['actual_elapsed_ns'] >= 2_000_000_000, 'Warmup insufficient')
    expected_totals = [dict(normalized=0,residual=0,pairs=0,boundaries=0) for _ in range(2)]
    completed = count = obs = elapsed_sum = 0
    elapsed = []; largest = []
    raw = out/('syndromes.csv' if stats else 'native_shots.csv')
    with raw.open(newline='') as stream, Path(case['inputs']['reference']['path']).open(newline='') as oracle:
        expected = csv.DictReader(oracle)
        for row in csv.DictReader(stream):
            ref = next(expected)
            R.need(int(row['shot_index']) == int(ref['shot_index']) == int(row['shot_execution_index']) == count, 'Shot order differs')
            R.need(int(row['input_events']) == int(ref['detection_events']) and int(row['integer_weight']) == int(ref['integer_weight']), 'Input/optimum differs')
            obs += int(row['observable_mask']) != int(ref['observable_mask'])
            if stats:
                residual = normalized = 0
                for c in range(2):
                    r = {k:int(row[f'c{c}_{k}']) for k in expected_totals[c]}
                    R.need(r['normalized'] == r['residual'] + 2*r['pairs'] + r['boundaries'], 'Component seed conservation failed')
                    if method == 'skip': R.need(r['normalized'] == r['residual'] and r['pairs'] == r['boundaries'] == 0, 'Skip consumed seeds')
                    for k, value in r.items(): expected_totals[c][k] += value
                    residual += r['residual']; normalized += r['normalized']
                # Frozen circuit/DEM has no negative-edge normalization events.
                R.need(normalized == int(row['input_events']), 'Normalized/input count differs')
                completed += residual == 0
            elif phase == 'latency':
                value = int(row['elapsed_ns']); R.need(value > 0, 'Missing latency')
                elapsed.append(value); elapsed_sum += value
                largest.append((value,count,int(row['input_events'])))
                if len(largest)>10: largest.sort(reverse=True); del largest[5:]
            else: R.need(row['elapsed_ns'] == '', 'Batch has per-shot clocks')
            count += 1
        R.need(count == shots and (shots != 1_000_000 or next(expected,None) is None), 'Incomplete shot coverage')
    R.need(obs == correctness['observable_differences'], 'Observable counts differ')
    result = dict(complete=True, shots=shots, method=method, phase=phase, observable_ties=obs,
        weight_differences=0, reference=case['inputs']['reference'])
    if stats:
        meta = R.load(out/'syndromes.json')
        R.need(meta['complete'] and meta['timed'] is False and meta['mode'] == POLICIES[method] and meta['shots'] == shots, 'Statistics configuration differs')
        R.need(meta['components'] == expected_totals and meta['completed_shots'] == completed, 'Statistics totals differ')
        R.need(not (out/'measurement.csv').exists(), 'Instrumented replay emitted timing')
        result.update(components=expected_totals, completed_shots=completed,
            normalized=sum(x['normalized'] for x in expected_totals), residual=sum(x['residual'] for x in expected_totals))
    else:
        measure = table(out/'measurement.csv'); R.need(len(measure)==1, 'One native aggregate required')
        m = measure[0]; total = int(m['elapsed_ns'])
        physical = ('pipe-' if method != 'Official' else '') + phase
        R.need(m['mode']==physical and int(m['shots'])==shots and total>0 and int(m['weight_diff'])==0 and int(m['observable_diff'])==obs, 'Measurement aggregate differs')
        R.need(int(m['total_threads']) == (1 if method=='Official' else 4), 'Wrong worker count')
        completion = R.load(out/'completion.json')
        drain = completion['final_drain_ns'] or 0
        R.need(completion['sample_elapsed_total_ns']==total and completion['complete_elapsed_ns']==total+drain, 'Drain total differs')
        if method != 'Official': R.need(completion['final_drain_called'], 'Pipeline must drain')
        if phase == 'batch':
            offset = ns = 0
            for b in table(out/'native_blocks.csv'):
                n = min(1024,shots-offset)
                R.need(int(b['first_shot_index'])==int(b['first_shot_execution_index'])==offset and int(b['count'])==n, 'Block coverage differs')
                offset += n; ns += int(b['elapsed_ns'])
            R.need(offset==shots and ns==total and drain==0, 'Batch total/drain differs')
        else:
            R.need(elapsed_sum==total, 'Latency sum differs')
            elapsed.sort()
            result['latency_ns'] = {label:elapsed[math.ceil(q*shots)-1] for label,q in [('p50',.5),('p90',.9),('p99',.99),('p99_9',.999),('max',1)]}
            result['latency_ns']['mean'] = total/shots
            result['tail_cases'] = [dict(elapsed_ns=v,shot_index=i,input_events=n) for v,i,n in sorted(largest,reverse=True)[:5]]
        result.update(elapsed_ns=total, complete_elapsed_ns=total+drain, final_drain_ns=drain, mean_us=total/shots/1000)
    result['files'] = {p.name:R.ref(p) for p in sorted(out.iterdir()) if p.is_file() and p.name!='validated.json'}
    R.write(out/'validated.json', result)
    return result


def call(root, plan, host, case, method, phase, repeat, shots, variants, out):
    if method == 'Official': binary = plan['official']['binary']
    else: binary = variants['statistics' if phase=='statistics' else 'timing']['binary']
    R.ref(binary['path'], binary['sha256'])
    mode = ('pipe-statistics' if phase=='statistics' else ('pipe-' if method!='Official' else '')+phase)
    cpus = [0] if method=='Official' else [0,1,2,3]
    argv = [binary['path'],'--mode',mode,'--distance',str(case['d']),'--error-rate',str(case['p']),
        '--shots','1000000','--root-seed','2026101402','--stream-id',str(case['stream_id']),
        '--dataset',str(root/'inputs'/case['id']/'dataset.bin'),'--warmup-seconds','2',
        '--block-size','1024','--repeats','1','--backend','generic' if method=='Official' else 'avx512vl',
        '--execution','serial' if method=='Official' else 'pipeline','--predecode','0' if method=='Official' else '1',
        '--verify','exact' if method=='Official' else 'weight','--witness-all','0', '--model-source','frozen', '--replay-kind','none']
    if method!='Official':
        argv += ['--partitions','1','--component-parallelism','2','--component-threshold','64','--shot-threshold','32',
            '--conflict-width','3','--parallel-import','0','--prefix-schedule','component_sequential',
            '--pipeline-schedule','overlap','--continuation-placement','component_captain','--conflict-geometry','symmetric_width']
    if shots!=1_000_000: argv += ['--limit',str(shots)]
    R.invoke(plan,host,argv,out,cpus,'TailReset',dict(case=case['id'],method=method,phase=phase,repeat=repeat,shots=shots))
    result = validate(out,case,method,phase,shots)
    result.update(case=case['id'],d=case['d'],p=case['p'],node=host['host'],repeat=repeat,
        validation=R.ref(out/'validated.json'),binary=binary)
    print(case['id'],method,phase,repeat,result.get('mean_us',result.get('residual')),flush=True)
    return result


def worker(root, node):
    plan=load(root); host=machine(node)
    lane=next(x for x in plan['lanes'] if x['node']==node)
    out=root/'lanes'/node; out.mkdir(parents=True,exist_ok=False)
    R.write(out/'machine.json',host)
    builds={}
    for policy in POLICIES:
        folder=root/'build'/policy
        ready=R.load(folder/'ready.json')
        R.need(ready['complete'] and (root/f"exit-{ready['job_id']}.txt").read_text().strip()=='0','Policy build not complete')
        builds[policy]=R.load(folder/'built.json')['variants']
    verified=set(); passes=[]
    for unit in lane['units']:
        case=next(c for c in plan['cases'] if c['id']==unit['case'])
        if case['id'] not in verified: verify_inputs(root,case); verified.add(case['id'])
        methods=METHODS[:-1] if unit['phase']=='statistics' else METHODS
        offset=(case['d']+unit['repeat'])%len(methods)
        for method in methods[offset:]+methods[:offset]:
            target=out/case['id']/(unit['phase']+'-'+str(unit['repeat']))/method
            passes.append(call(root,plan,host,case,method,unit['phase'],unit['repeat'],1_000_000,builds.get(method),target))
        R.write(out/'progress.json',dict(complete=False,passes=passes,last_unit=unit,updated_utc=R.utc()))
    R.write(out/'complete.json',dict(complete=True,node=node,job_id=host['job_id'],plan=R.ref(root/'plan.json'),passes=passes))


def analyze(root):
    plan=load(root); machine(plan['analysis_node'])
    output=root/'analysis-r01'; output.mkdir(exist_ok=False)
    records={}; lanes=[]
    for lane in plan['lanes']:
        path=root/'lanes'/lane['node']/'complete.json'
        done=R.load(path)
        R.need(done['complete'] and done['node']==lane['node'],'Incomplete lane')
        exit_file=root/f"exit-{done['job_id']}.txt"
        R.need(exit_file.read_text().strip()=='0','Lane did not exit successfully')
        expected={(u['case'],m,u['phase'],u['repeat']) for u in lane['units'] for m in (METHODS[:-1] if u['phase']=='statistics' else METHODS)}
        actual=set()
        for row in done['passes']:
            key=row['case'],row['method'],row['phase'],row['repeat']
            R.need(key in expected and key not in records,'Duplicate/unplanned call')
            R.ref(row['validation']['path'],row['validation']['sha256'])
            v=R.load(row['validation']['path'])
            R.need(v['complete'] and v['shots']==1_000_000 and v['weight_differences']==0,'Incomplete validation')
            for ref in v['files'].values(): R.ref(ref['path'],ref['sha256'])
            records[key]=row; actual.add(key)
        R.need(actual==expected,'Incomplete lane coverage')
        lanes.append(dict(complete=R.ref(path),exit=R.ref(exit_file)))
    R.need(len(records)==133,'Expected 84 timing, 28 latency and 21 statistics calls')
    rows=[]; latency=[]
    for case in plan['cases']:
        official=sum(records[(case['id'],'Official','batch',r)]['elapsed_ns'] for r in range(1,4))/3e9
        for method in METHODS:
            rounds=[records[(case['id'],method,'batch',r)] for r in range(1,4)]
            mean=sum(r['elapsed_ns'] for r in rounds)/3e9
            stats=records.get((case['id'],method,'statistics',1))
            rows.append(dict(distance=case['d'],physical_error_rate=case['p'],rounds=case['rounds'],shots=1_000_000,
                strategy=method,threads=1 if method=='Official' else 4,mean_decoding_us_per_shot=mean,
                batch1_us=rounds[0]['mean_us'],batch2_us=rounds[1]['mean_us'],batch3_us=rounds[2]['mean_us'],
                official_speedup=official/mean,normalized_syndromes_per_shot=stats['normalized']/1e6 if stats else '',
                remaining_syndromes_per_shot=stats['residual']/1e6 if stats else '',
                remaining_syndromes_total=stats['residual'] if stats else '',completed_shots=stats['completed_shots'] if stats else '',
                batch_observable_ties=sum(r['observable_ties'] for r in rounds),statistics_observable_ties=stats['observable_ties'] if stats else ''))
            v=records[(case['id'],method,'latency',1)]
            latency.append(dict(distance=case['d'],strategy=method,**v['latency_ns'],final_drain_ns=v['final_drain_ns']))
    write_csv(output/'summary.csv',rows); write_csv(output/'latency.csv',latency)
    # Existing renderer's exact ten-metric schema, generated only from this run.
    def row(metric,label,unit,values):
        return dict(metric=metric,label=label,unit=unit,**{f'd{c["d"]:02d}':value for c,value in zip(plan['cases'],values)})
    chosen=lambda method:[next(r for r in rows if r['distance']==c['d'] and r['strategy']==method) for c in plan['cases']]
    data=[row('initial_mean','预解码前平均 syndrome 数','syndromes/shot',[x['normalized_syndromes_per_shot'] for x in chosen('skip')])]
    for policy,legacy in [('skip','skip'),('coordinate','coordinate'),('degree','current')]:
        data.append(row('speed_'+legacy,policy+' 总解码时间','us/shot',[x['mean_decoding_us_per_shot'] for x in chosen(policy)]))
    for policy,legacy in [('coordinate','coordinate'),('degree','current')]:
        values=chosen(policy)
        data.extend([row('residual_'+legacy,policy+' 预解码后平均剩余 syndrome 数','syndromes/shot',[x['remaining_syndromes_per_shot'] for x in values]),
            row('complete_count_'+legacy,policy+' 整 shot 直接完成数','shots / 1000000',[x['completed_shots'] for x in values]),
            row('complete_percent_'+legacy,policy+' 整 shot 直接完成比例','%',[x['completed_shots']/10000 for x in values])])
    write_csv(output/'predecode.csv',data)
    summary=dict(complete=True,experiment_id=plan['experiment_id'],implementation_commit=plan['implementation_commit'],
        acquisition_commit=plan['acquisition_commit'],plan=R.ref(root/'plan.json'),lanes=lanes,protocol=plan['protocol'],
        formal_calls=133,independent_inputs=7_000_000,measured_decodes=133_000_000,
        weight_differences=0,observable_ties=sum(v['observable_ties'] for v in records.values()),
        rows=rows,latency=latency,calls=list(records.values()))
    R.write(output/'summary.json',summary)
    R.write(root/'complete.json',dict(complete=True,summary=R.ref(output/'summary.json'),created_utc=R.utc()))
    print(output/'summary.csv',flush=True)


def submit(root, stage):
    plan=load(root); script=root/'source/AE/tailreset_collect.py'
    jobs=[]
    if stage=='build':
        calls=[(plan['build_nodes'][p],['build','--policy',p]) for p in POLICIES]; dependency=[]
    elif stage=='formal':
        receipt=R.load(root/'submission-build.json')
        calls=[(lane['node'],['worker','--node',lane['node']]) for lane in plan['lanes']]
        dependency=['--dependency=afterok:'+':'.join(j['job_id'] for j in receipt['jobs'])]
    else:
        receipt=R.load(root/'submission-formal.json')
        calls=[(plan['analysis_node'],['analyze'])]
        dependency=['--dependency=afterok:'+':'.join(j['job_id'] for j in receipt['jobs'])]
    R.need(not (root/f'submission-{stage}.json').exists(),'Stage already submitted')
    for node,argv in calls:
        args=['sbatch','--parsable','--nodelist='+node,'--exclude=ares7,ares8','--kill-on-invalid-dep=yes',
            '--job-name=ae-tailreset-'+stage,'--chdir='+str(root),'--output='+str(root/'slurm-%j.log'),*dependency,
            str(root/'source/AE/tailreset.sbatch'),str(root),plan['python'],str(script),*argv,'--output',str(root)]
        job=subprocess.check_output(args,text=True).strip().split(';')[0]
        R.need(job.isdigit(),'Invalid submission receipt')
        jobs.append(dict(job_id=job,node=node,argv=args)); print(stage,node,job,flush=True)
        R.write(root/f'submission-{stage}.partial.json',dict(jobs=jobs))
    R.write(root/f'submission-{stage}.json',dict(complete=True,stage=stage,jobs=jobs,created_utc=R.utc()))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['freeze','build','worker','analyze','submit'])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--reference-run',type=Path)
    p.add_argument('--cache-dir',type=Path)
    p.add_argument('--implementation')
    p.add_argument('--stim-source',type=Path)
    p.add_argument('--gtest-source',type=Path)
    p.add_argument('--tool-runtime',type=Path,help='Optional pinned CMake/CTest/Git runtime directory with manifest.json')
    p.add_argument('--nodes',default=','.join(NODES))
    p.add_argument('--node',choices=NODES)
    p.add_argument('--policy',choices=POLICIES)
    p.add_argument('--stage',choices=['build','formal','analysis'])
    a=p.parse_args(); root=a.output.resolve()
    try:
        if a.action=='freeze':
            R.need(all([a.reference_run,a.cache_dir,a.implementation,a.stim_source,a.gtest_source]),'freeze requires source/dependency/cache/implementation arguments')
            freeze(a)
        elif a.action=='build': build(root,a.policy)
        elif a.action=='worker': worker(root,a.node)
        elif a.action=='analyze': analyze(root)
        else: submit(root,a.stage)
    except BaseException:
        traceback.print_exc(); return 1
    return 0


if __name__=='__main__': raise SystemExit(main())
