"""AE L2/L3 million-shot PMU acquisition; freeze, build/smoke, collect and analyze through Slurm."""
import argparse
import csv
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import traceback
import sys
import pmu_runtime as R
from pmu_data import LEVELS, counters

HERE = Path(__file__).resolve().parent
NODES = [f'ares{k}' for k in range(1, 7)]
METHODS = ['Upstream', 'GridSIMD']
IMPLEMENTATION = 'eb66e671947d066adafab1b2a504d45b2f0530d7'


def git(*args):
    return subprocess.check_output(['git', *args], cwd=HERE, text=True).strip()


def freeze(root, nodes, reference_run, level, cache_dir=None):
    R.need(not root.exists(), 'Choose a fresh run directory')
    R.need(nodes and len(nodes)==len(set(nodes)) and set(nodes).issubset(NODES), 'Choose unique allowed worker nodes')
    R.need(not git('status', '--porcelain'), 'Commit collector before freezing')
    reference_run = reference_run.resolve()
    parent = R.load(reference_run / 'plan.json')
    R.need(parent['implementation_commit']==IMPLEMENTATION, 'AE PMU adapter requires the frozen eb66e671 implementation')
    R.need(len(parent['cases'])==21 and parent['shots']==1000000 and parent['protocol']['lto']=='OFF', 'Reference matrix differs')
    spec = LEVELS[level]
    lanes = [dict(node=node, cases=[], historical_work=0) for node in nodes]
    for case in sorted(parent['cases'], key=lambda c: -c['historical_batch_us']):
        lane = min(lanes, key=lambda v: (v['historical_work'], v['node']))
        lane['cases'].append(case['id'])
        lane['historical_work'] += case['historical_batch_us']
    root.mkdir(parents=True)
    source=root/'source';source.mkdir()
    for name in ['pmu_collect.py','pmu_runtime.py','pmu_data.py','pmu.sbatch']:
        shutil.copy2(HERE/name,source/name)
    shutil.copytree(HERE/'pmu_native',source/'pmu_native')
    plan = dict(experiment_id='AE-PMU-'+level, run_id=root.name, created_utc=R.utc(),
        acquisition_commit=git('rev-parse', 'HEAD'), implementation_commit=parent['implementation_commit'],
        parent_plan=R.ref(reference_run / 'plan.json'),reference_run=str(reference_run),
        level=level, python=sys.executable,cache_dir=str((cache_dir or HERE.parent/'.local/cache/ae-pmu'/root.name).resolve()), parent_build=parent['build'],
        reference_document='https://wcncbam07gf8.feishu.cn/wiki/UQ90wEJcciPrHLkbbmhc6zc1n8H',
        reference_document_revision=6, cases=parent['cases'], methods=METHODS,
        method_settings={m:parent['method_settings'][m] for m in METHODS},
        parent_variants={k:parent['variants'][k] for k in ['direct', 'structured']},
        parent_native_sources=parent['native_sources'], lanes=lanes, shots=1000000,
        build_node=nodes[0], analysis_node=nodes[0],
        smoke_cases=[parent['cases'][0]['id'], parent['cases'][-1]['id']],
        source_files={str(p.relative_to(root / 'source')):R.ref(p) for p in sorted((root / 'source').rglob('*')) if p.is_file()},
        protocol=dict(metric=spec['metric'], formula='100 * '+spec['events'][1]['name']+' / '+spec['events'][0]['name'],
            events=spec['events'],
            lto='OFF', repeats=1, latency_collected=False, warmup_seconds=2, warmup_prefix=1024,
            counted_scope='continuous normal decode loop and result stores; user-space calling thread only',
            excluded='loading, graph/model construction, warmup, checking, result persistence, kernel and hypervisor',
            counter_policy='pinned group; reject zero enabled time, multiplexing, partial reads or unsupported events; no scaling or substitutions',
            ordering='paired methods on same node, cyclic first method by stream_id; one million each',
            allowed_nodes=NODES, worker_nodes=nodes, exclusive=True, measurement_cpus=[0], controller_cpu=8, numa_node=0,
            correctness='all original shot IDs, event counts, integer weights and observable masks exactly match frozen reference',
            stopping='all 21 settings x 2 methods; retain failures; no performance-dependent selection'),
        timeout_seconds_per_call=7200)
    R.write(root / 'plan.json', plan)
    print(R.ref(root / 'plan.json'), flush=True)


def load(root):
    frozen = R.load(root / 'freeze.json')
    R.ref(root / 'plan.json', frozen['plan_sha256'])
    plan = R.load(root / 'plan.json')
    R.need(plan['methods'] == METHODS and plan['shots'] == 1000000, 'Wrong scope')
    for ref in plan['source_files'].values(): R.ref(ref['path'], ref['sha256'])
    return plan


def machine(node):
    host = os.uname().nodename.split('.')[0]
    R.need(host == node and host in NODES, 'Unexpected or forbidden host')
    R.need({0, 8}.issubset(os.sched_getaffinity(0)), 'CPU lane unavailable')
    job = subprocess.check_output(['scontrol', 'show', 'job', os.environ['SLURM_JOB_ID']], text=True)
    R.need('OverSubscribe=NO' in job, 'Exclusive node required')
    cpuinfo = Path('/proc/cpuinfo').read_text()
    R.need('Intel(R) Xeon(R) Gold 6230R' in cpuinfo, 'Hardware differs from report')
    cpu_fields = dict((line.split(':',1)[0].strip(),line.split(':',1)[1].strip()) for line in cpuinfo.split('\n\n')[0].splitlines() if ':' in line)
    R.need(cpu_fields['vendor_id']=='GenuineIntel' and cpu_fields['cpu family']=='6' and cpu_fields['model']=='85', 'L2 event CPU model mismatch')
    topology = []
    for cpu in [0, 8]:
        p = Path(f'/sys/devices/system/cpu/cpu{cpu}')
        R.need((p / 'node0').exists(), 'Expected NUMA0')
        topology.append(((p / 'topology/physical_package_id').read_text(), (p / 'topology/core_id').read_text()))
    R.need(len(set(topology)) == 2, 'Controller must use another physical core')
    result = dict(host=host, job_id=os.environ['SLURM_JOB_ID'], job=job, cpuinfo=cpuinfo,
        lscpu=subprocess.check_output(['lscpu'], text=True), kernel=os.uname().release,
        perf_event_paranoid=Path('/proc/sys/kernel/perf_event_paranoid').read_text().strip(),
        allocation_affinity=sorted(os.sched_getaffinity(0)), numa_node=0, measurement_cpus=[0], controller_cpu=8)
    os.sched_setaffinity(0, {8})
    return result


def verify_input(case, plan):
    parent=Path(plan['reference_run'])
    for ref in case['inputs'].values(): R.ref(ref['path'], ref['sha256'])
    R.need((parent / 'inputs' / case['id'] / 'dataset.bin').resolve()==Path(case['inputs']['dataset']['path']).resolve(), 'Frozen dataset symlink changed')
    R.ref(parent / 'inputs' / case['id'] / 'model.dem', case['original_dem_sha256'])


def validate(out, case, shots, level):
    value = R.load(out / 'pmu.json')
    counters(value,level,shots)
    warm = R.load(out / 'warmup.json')
    R.need(warm['requested_seconds'] == 2 and warm['actual_elapsed_ns'] >= 2000000000, 'Insufficient warmup')
    check = R.load(out / 'correctness.json')
    R.need(check['status'] == 'passed' and check['shots'] == shots, 'Correctness failed')
    count = 0
    with (out / 'native_shots.csv').open() as stream, Path(case['inputs']['reference']['path']).open() as oracle:
        expected = csv.DictReader(oracle)
        for actual in csv.DictReader(stream):
            want = next(expected)
            R.need(all(int(actual[k]) == int(want[k]) for k in ['shot_index', 'integer_weight', 'observable_mask']), f'Result differs at {count}')
            R.need(int(actual['input_events']) == int(want['detection_events']), 'Event count differs')
            R.need(int(actual['shot_index']) == int(actual['shot_execution_index']) == count, 'Wrong input order')
            R.need(actual['elapsed_ns'] == '' and actual['status'] == 'measured', 'Unexpected latency or status')
            count += 1
        if shots == 1000000: R.need(next(expected, None) is None, 'Trailing reference shots')
    R.need(count == shots, 'Incomplete results')
    execution = list(csv.DictReader((out / 'execution.csv').open()))
    R.need(len(execution) == 1 and execution[0]['execution'] == 'serial' and execution[0]['total_threads'] == '1', 'Wrong thread configuration')
    R.need(not (out / 'measurement.csv').exists() and not (out / 'native_blocks.csv').exists(), 'Unexpected latency measurement')
    R.write(out / 'validation.json', dict(complete=True, shots=shots, exact_differences=0, latency_collected=False,
        files={p.name:R.ref(p) for p in out.iterdir() if p.is_file()}))
    return value


def call(root, plan, build, case, method, out, host, shots):
    parent = Path(plan['reference_run'])
    config = plan['method_settings'][method]
    binary = build['variants'][config['binary']]['binary']
    R.ref(binary['path'], binary['sha256'])
    argv = [binary['path'], '--mode','pmu','--distance',str(case['d']),'--error-rate',str(case['p']),
        '--shots','1000000','--root-seed','2026101402','--stream-id',str(case['stream_id']),
        '--block-size','1024','--repeats','1','--main-cpu','0','--cpus','0',
        '--backend',config['backend'],'--execution','serial','--predecode','0',
        '--dataset',str(parent / 'inputs' / case['id'] / 'dataset.bin'), '--output-dir',str(out),
        '--warmup-seconds','2','--verify','exact','--witness-all','0','--model-source','frozen','--replay-kind','none']
    if shots != 1000000: argv += ['--limit',str(shots)]
    R.invoke(plan, host, argv, out, [0], method, dict(case=case['id'],method=method,shots=shots,mode='pmu'), pm_output=False)
    value = validate(out, case, shots, plan['level'])
    spec=LEVELS[plan['level']];prefix=spec['prefix'];metric=spec['metric']
    result = dict(case=case['id'],d=case['d'],p=case['p'],method=method,node=host['host'],shots=shots,
        pmu=R.ref(out / 'pmu.json'),validation=R.ref(out / 'validation.json'),binary=binary,
        **{prefix+'_loads':value[prefix+'_loads'],prefix+'_load_misses':value[prefix+'_load_misses'],metric:value[metric]})
    print(result['case'], method, shots, result[metric], flush=True)
    return result


def smoke(root, plan, build, host, destination):
    values=[]
    for case in [c for c in plan['cases'] if c['id'] in plan['smoke_cases']]:
        verify_input(case,plan)
        for method in METHODS:
            values.append(call(root, plan, build, case, method, destination / case['id'] / method, host, 1025))
    return values


def build_and_smoke(root):
    plan = load(root); host = machine(plan['build_node'])
    R.write(root / 'build-machine.json', host)
    for ref in plan['parent_native_sources'].values(): R.ref(ref['path'], ref['sha256'])
    parent_build = R.load(plan['parent_build']['path'])
    R.ref(plan['parent_build']['path'], plan['parent_build']['sha256'])
    for ref in parent_build['libraries'].values(): R.ref(ref['path'], ref['sha256'])
    out = root / 'build'; out.mkdir(exist_ok=False)
    cache = Path(plan['cache_dir'])
    cache.mkdir(parents=True, exist_ok=False)
    variants = {}
    for name, variant in plan['parent_variants'].items():
        args = variant['compile'].copy()+['-DPM_PMU_CACHE_LEVEL='+plan['level'][1:]]
        obj = cache / (name + '.o'); binary = out / name
        args[args.index('-c') + 1] = str(root / 'source/pmu_native/benchmark.cc')
        args[args.index('-o') + 1] = str(obj)
        link = variant['link'].copy()
        link[link.index('-o') - 1] = str(obj)
        link[link.index('-o') + 1] = str(binary)
        R.need(not any(v.startswith('-flto') for v in args+link), 'LTO enabled')
        for stage, argv in [('compile',args),('link',link)]:
            R.write(out / f'{name}-{stage}.json', argv)
            with (out / f'{name}-{stage}.log').open('x') as log:
                subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, check=True)
        variants[name] = dict(binary=R.ref(binary),core=variant['core'],compile=args,link=link)
        print('Built', name, flush=True)
    build = dict(complete=True,implementation_commit=plan['implementation_commit'],acquisition_commit=plan['acquisition_commit'],
        variants=variants, libraries=parent_build['libraries'], compiler=subprocess.check_output(['/usr/bin/c++','--version'], text=True))
    R.write(root / 'build/complete.json', build)
    values = smoke(root, plan, build, host, root / 'build-smoke')
    R.write(root / 'ready.json', dict(complete=True,passes=values,job_id=os.environ['SLURM_JOB_ID'],build=R.ref(root / 'build/complete.json')))


def run(root, index):
    plan = load(root); node = plan['lanes'][index]['node']; host = machine(node)
    ready = R.load(root / 'ready.json')
    R.need(ready['complete'] and (root / f'exit-{ready["job_id"]}.txt').read_text().strip() == '0', 'Build/smoke gate failed')
    R.ref(ready['build']['path'], ready['build']['sha256'])
    build = R.load(ready['build']['path'])
    out = root / 'lanes' / node; out.mkdir(parents=True,exist_ok=False)
    R.write(out / 'machine.json', host)
    state = dict(status='checking',node=node,job_id=os.environ['SLURM_JOB_ID'],started_utc=R.utc(),passes=[])
    try:
        state['smoke'] = smoke(root, plan, build, host, out / 'smoke')
        for case_id in plan['lanes'][index]['cases']:
            case = next(c for c in plan['cases'] if c['id'] == case_id)
            verify_input(case,plan)
            offset = (case['stream_id'] - 1) % 2
            for method in METHODS[offset:] + METHODS[:offset]:
                state.update(status='collecting',current=dict(case=case_id,method=method))
                R.write(out / 'status.json', state)
                state['passes'].append(call(root, plan, build, case, method, out / 'measurements' / case_id / method, host, plan['shots']))
                R.write(out / 'status.json', state)
        state.update(status='complete',complete=True,finished_utc=R.utc())
        R.write(out / 'complete.json', state)
    except BaseException as error:
        state.update(status='failed',error=str(error),traceback=traceback.format_exc()); raise
    finally: R.write(out / 'status.json', state)


def analyze(root):
    plan = load(root); machine(plan['analysis_node']); values=[]
    for node in [lane['node'] for lane in plan['lanes']]:
        done = R.load(root / 'lanes' / node / 'complete.json')
        R.need(done['complete'] and (root / f'exit-{done["job_id"]}.txt').read_text().strip() == '0', 'Incomplete lane')
        for entry in done['passes']:
            for key in ['pmu','validation']: R.ref(entry[key]['path'], entry[key]['sha256'])
            values.append(entry)
    expected = set(itertools.product([c['id'] for c in plan['cases']], METHODS))
    R.need(len(values) == 42 and {(v['case'],v['method']) for v in values} == expected, 'Incomplete 21x2 matrix')
    R.need(all(v['shots'] == 1000000 for v in values), 'Wrong formal shot count')
    out = root / 'analysis-r01'; out.mkdir(exist_ok=False)
    values.sort(key=lambda v:(v['d'],v['p'],METHODS.index(v['method'])))
    spec=LEVELS[plan['level']];prefix=spec['prefix'];metric=spec['metric']
    R.write(out / 'summary.json', dict(complete=True,plan=R.ref(root / 'plan.json'),metric=plan['protocol']['metric'],
        formula=plan['protocol']['formula'],latency_collected=False,formal_calls=42,total_decodes=42000000,unique_inputs=21000000,measurements=values))
    with (out / 'summary.csv').open('x') as stream:
        writer=csv.DictWriter(stream,lineterminator='\n',fieldnames=['case','d','p','method','node','shots',prefix+'_loads',prefix+'_load_misses',metric])
        writer.writeheader()
        for v in values: writer.writerow({k:v[k] for k in writer.fieldnames})
    lines=['# Upstream / GridSIMD '+plan['level']+' load miss 率','',
        '21组；每入口每组100万shots、1轮；单线程、LTO OFF。仅计解码循环的用户态读取；不测延迟。',
        plan['protocol']['formula']+'。加载、构图、预热、校验及CSV输出在计数窗口外。','',
        '| d | p | Upstream (%) | GridSIMD (%) |','| --- | --- | --- | --- |']
    for case in plan['cases']:
        pair={v['method']:v for v in values if v['case']==case['id']}
        lines.append(f'| {case["d"]} | {case["p"]:.4g} | {pair["Upstream"][metric]:.6f} | {pair["GridSIMD"][metric]:.6f} |')
    (out / 'REPORT.md').write_text('\n'.join(lines)+'\n')
    R.write(root / 'complete.json', dict(complete=True,job_id=os.environ['SLURM_JOB_ID'],summary=R.ref(out / 'summary.json')))
    print('Complete: 42 million-shot cache-only measurements', flush=True)


def submit(root, stage):
    plan = load(root); receipt=root / f'{stage}-submission.json'
    R.need(not receipt.exists(), 'Submission already exists')
    if stage == 'build': assignments=[(plan['build_node'],'build',None)]; dependency=[]
    elif stage == 'formal':
        build_submission=R.load(root / 'build-submission.json')
        R.need(build_submission['status']=='submitted' and len(build_submission['jobs'])==1, 'Build job missing')
        build_job=build_submission['jobs'][0]
        R.need(build_job['returncode']==0 and build_job['node']==plan['build_node'], 'Invalid build submission')
        assignments=[(lane['node'],'run',index) for index,lane in enumerate(plan['lanes'])]
        dependency=['--dependency=afterok:'+build_job['job_id']]
    else:
        formal=R.load(root / 'formal-submission.json')
        R.need(formal['status']=='submitted' and len(formal['jobs'])==len(plan['lanes']), 'Formal jobs missing')
        assignments=[(plan['analysis_node'],'analyze',None)]
        dependency=['--dependency=afterok:'+':'.join(j['job_id'] for j in formal['jobs'])]
    jobs=[]
    for node,action,index in assignments:
        argv=['sbatch','--parsable','--nodelist='+node,'--job-name=ae-pmu-'+plan['level'].lower()+'-'+stage,'--kill-on-invalid-dep=yes','--chdir='+str(root),
            '--time='+('04:00:00' if stage=='formal' else '01:00:00'),
            '--output='+str(root / f'{stage}-{node}-%j.log'),*dependency,str(root / 'source/pmu.sbatch'),
            str(root),plan['python'],'-u',str(root / 'source/pmu_collect.py'),action,'--output',str(root)]
        if index is not None: argv+=['--index',str(index)]
        proc=subprocess.run(argv,capture_output=True,text=True)
        entry=dict(node=node,argv=argv,returncode=proc.returncode,stdout=proc.stdout,stderr=proc.stderr)
        if proc.returncode==0: entry['job_id']=proc.stdout.strip().split(';')[0]
        jobs.append(entry)
        R.write(receipt,dict(status='submitted' if len(jobs)==len(assignments) and proc.returncode==0 else 'partial',jobs=jobs))
        R.need(proc.returncode==0,'Submission failed: '+proc.stderr)
        print(node,entry['job_id'],flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=['freeze','build','run','analyze','submit'])
    p.add_argument('--output',type=Path,required=True);p.add_argument('--index',type=int,default=0)
    p.add_argument('--stage',choices=['build','formal','analysis'])
    p.add_argument('--nodes',nargs='+',default=NODES)
    p.add_argument('--reference-run',type=Path,help='冻结矩阵的原始run，包含输入、构建及完整plan')
    p.add_argument('--level',choices=['L2','L3'])
    p.add_argument('--cache-dir',type=Path)
    args=p.parse_args(); root=args.output.resolve()
    if args.action=='freeze':
        if args.reference_run is None or args.level is None:
            p.error('freeze requires --reference-run and --level')
        freeze(root,args.nodes,args.reference_run,args.level,args.cache_dir);R.write(root / 'freeze.json',dict(plan_sha256=R.sha(root / 'plan.json'),created_utc=R.utc()))
    elif args.action=='build': build_and_smoke(root)
    elif args.action=='run': run(root,args.index)
    elif args.action=='analyze': analyze(root)
    else:
        if args.stage is None:
            p.error('submit requires --stage')
        submit(root,args.stage)
