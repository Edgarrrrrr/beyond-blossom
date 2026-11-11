#!/usr/bin/env python3
"""Collect native decoding runs with explicit source, binary, input and CPU identities."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import struct
import shutil
import subprocess
import sys
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
METHODS = {
    'reference': dict(threads=1, backend='generic', execution='serial', predecode=0),
    'unstructured': dict(threads=1, backend='generic', execution='serial', predecode=0),
    'structured': dict(threads=1, execution='serial', predecode=0),
    'predecoded': dict(threads=1, execution='serial', predecode=1),
    'component-parallel': dict(threads=2, execution='shared', predecode=1, partitions=1,
                      component_threshold=64, shot_threshold=32, conflict_width=3),
    'pipelined': dict(threads=4, execution='pipeline', predecode=1, partitions=1,
                      component_threshold=64, shot_threshold=32, conflict_width=3),
    'parallel-predecode': dict(threads=8, execution='pipeline', predecode=1, partitions=2,
                    component_threshold=0, shot_threshold=32, conflict_width=0,
                    parallel_import=1, prefix_schedule='component_group_owned', conflict_geometry='three_planes'),
}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def write(path, data):
    with Path(path).open('x') as f:
        json.dump(data, f, indent=2, allow_nan=False)
        f.write('\n')


def source_identity():
    def git(*argv):
        p = subprocess.run(['git', '-C', str(HERE.parent), *argv], text=True, capture_output=True)
        return p.stdout.strip() if p.returncode == 0 else None
    return dict(commit=git('rev-parse', 'HEAD'), status=git('status', '--porcelain'))


def physical_cpus():
    if platform.system() == 'Darwin':
        return [-1] * (os.cpu_count() or 1)
    if not hasattr(os, 'sched_getaffinity'):
        raise ValueError('Native acquisition supports Linux affinity or Darwin QoS')
    selected, seen = [], set()
    for cpu in sorted(os.sched_getaffinity(0)):
        base = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
        key = ((base/'physical_package_id').read_text().strip(), (base/'core_id').read_text().strip())
        if selected and key[0] != next(iter(seen))[0]:
            continue
        if key not in seen:
            selected.append(cpu)
            seen.add(key)
    return selected


def invoke(command, directory):
    directory.mkdir(parents=True)
    write(directory/'command.json', command)
    # The native harness requires an empty artifact directory except process.log.
    artifact = directory/'native'
    artifact.mkdir()
    argv = [*command, '--output-dir', str(artifact)]
    with (artifact/'process.log').open('x') as log:
        result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
    write(directory/'exit.json', dict(returncode=result.returncode))
    if result.returncode:
        raise RuntimeError(f'Native command failed ({result.returncode}): {artifact}/process.log')
    return artifact


def verify_relative_files(root, files):
    root = Path(root).resolve()
    for name, expected in files.items():
        path = (root / name).resolve()
        if root not in path.parents or not path.is_file() or digest(path) != expected:
            raise ValueError(f'Changed or missing artifact: {name}')


def native_build_record(path, binary, identity, require_clean=True):
    record = json.loads(Path(path).read_text())
    built = record.get('source', {})
    if (not record.get('complete') or record.get('binary', {}).get('sha256') != digest(binary)
            or built.get('commit') != identity['commit'] or built.get('status') != identity['status']
            or (require_clean and (not built.get('commit') or built.get('status')))):
        raise ValueError('Candidate build receipt must match the executable and current clean source commit')
    if not record.get('source_files'):
        raise ValueError('Candidate build receipt lacks source file identities')
    verify_relative_files(HERE.parent, record['source_files'])
    if record.get('builder_sha256') != digest(HERE / 'build_native.py'):
        raise ValueError('Native builder changed after the candidate build')
    return record


def saved_inputs(run):
    root = Path(run).resolve()
    plan = json.loads((root / 'plan.json').read_text())
    receipt = json.loads((root / 'complete.json').read_text())
    if not receipt.get('complete') or receipt.get('plan_sha256') != digest(root / 'plan.json'):
        raise ValueError('Input reuse requires a completed run with an unchanged plan')
    cases = {}
    for item in receipt['inputs']:
        case = item['case']
        if case in cases:
            raise ValueError('Duplicate saved input case')
        verify_relative_files(root, item['files'])
        required = {str(Path('inputs') / case / name) for name in ('dataset.bin', 'model.dem', 'circuit.stim')}
        if not required <= item['files'].keys():
            raise ValueError(f'Incomplete saved input case: {case}')
        cases[case] = item
    return root, plan, cases


def collect(args):
    root = args.output.resolve()
    if root.exists():
        raise ValueError('Use a new --output directory; existing runs are never overwritten')
    if args.profile == 'paper' and (root == HERE.parent or HERE.parent in root.parents):
        raise ValueError('Paper output must be outside the source repository')
    binary = args.binary.resolve()
    reference = args.reference_binary.resolve() if args.reference_binary else binary
    if not binary.is_file() or not reference.is_file():
        raise ValueError('Both native executables must exist')
    if args.profile == 'paper' and not args.reference_binary:
        raise ValueError('Paper collection requires an independently built --reference-binary')
    if args.profile == 'paper' and digest(binary) == digest(reference):
        raise ValueError('Paper collection requires distinct reference and candidate executables')
    reference_record=None
    if args.reference_record:
        reference_record=json.loads(args.reference_record.read_text())
        if not reference_record.get('complete') or reference_record.get('upstream_revision')!='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd' or reference_record['binary']['sha256']!=digest(reference):
            raise ValueError('Reference build receipt does not identify the fixed upstream executable')
    if args.profile=='paper' and reference_record is None:
        raise ValueError('Paper collection requires --reference-record from build_reference.py')
    methods = args.methods.split(',') if args.methods else list(METHODS)
    if not methods or len(set(methods)) != len(methods) or set(methods)-METHODS.keys():
        raise ValueError('Unknown or duplicate method')
    if args.measurement == 'statistics' and (not args.reference_binary or any(METHODS[m]['execution']!='pipeline' for m in methods)):
        raise ValueError('Statistics requires --reference-binary and pipeline methods only')
    if args.measurement == 'pmu' and any(METHODS[m]['threads'] != 1 for m in methods):
        raise ValueError('PMU measures the calling thread only; choose serial methods')
    cpus = [int(v) for v in args.cpus.split(',')] if args.cpus else physical_cpus()
    needed = max(METHODS[m]['threads'] for m in methods)
    if len(cpus) < needed:
        raise ValueError(f'Need {needed} distinct physical CPUs on one socket')
    reused = saved_inputs(args.input_run) if args.input_run else None
    source_plan = reused[1] if reused else {}
    distances = [int(d) for d in args.distances.split(',')] if args.distances else source_plan.get('distances', [9] if args.profile == 'smoke' else [9,13,17,21,25,29,33])
    rates = [float(p) for p in args.error_rates.split(',')] if args.error_rates else source_plan.get('error_rates', [.003] if args.profile == 'smoke' else [.0005,.001,.003])
    shots = args.shots if args.shots is not None else source_plan.get('shots', 1025 if args.profile == 'smoke' else 1000000)
    repeats = args.repeats if args.repeats is not None else (1 if args.profile == 'smoke' else 3)
    seed = args.seed if args.seed is not None else source_plan.get('seed', 20261112)
    if reused and (shots != source_plan['shots'] or seed != source_plan['seed']):
        raise ValueError('Reused inputs require the original saved shot count and root seed')
    if reused and args.profile == 'paper':
        old_reference = source_plan.get('reference', {})
        if (not old_reference.get('independent') or
                (old_reference.get('build_record') or {}).get('upstream_revision') != 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'):
            raise ValueError('Paper replay inputs must originate from the fixed independent reference')
    if (shots < 1 or repeats < 1 or not distances or not rates or len(set(distances)) != len(distances)
            or len(set(rates)) != len(rates) or any(d<3 or d%2==0 for d in distances) or any(not 0<p<.5 for p in rates)):
        raise ValueError('Invalid workload')
    identity=source_identity()
    if args.profile=='paper' and (not identity['commit'] or identity['status']):
        raise ValueError('Commit the implementation and collector in a clean checkout before paper collection')
    build_record = native_build_record(args.build_record, binary, identity, args.profile == 'paper') if args.build_record else None
    if args.profile == 'paper' and build_record is None:
        raise ValueError('Paper collection requires --build-record from AE/build_native.py')
    root.mkdir(parents=True)
    build_records={}
    for label,executable in [('candidate',binary),('reference',reference)]:
        directory=executable.parent.parent
        records={}
        for name in ('CMakeCache.txt','compile_commands.json'):
            original=directory/name
            if original.is_file():
                destination=root/'build'/label/name
                destination.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(original,destination)
                records[str(destination.relative_to(root))]=digest(destination)
        build_records[label]=records
    plan = dict(schema=1, created_utc=datetime.now(timezone.utc).isoformat(), profile=args.profile,
                source=identity, build_records=build_records, platform=platform.platform(), machine=platform.machine(),host=platform.node(),placement=('Darwin USER_INITIATED QoS; physical affinity unverified' if platform.system()=='Darwin' else 'Linux physical CPU affinity'),
                binary=dict(path=str(binary),sha256=digest(binary),build_record=build_record),
                reference=dict(path=str(reference),sha256=digest(reference),independent=bool(args.reference_binary),build_record=reference_record),
                collector_sha256=digest(__file__), distances=distances,error_rates=rates,rounds='distance',
                shots=shots,repeats=repeats,seed=seed,cpus=cpus[:needed],backend=args.backend,
                reused_inputs=dict(run=str(reused[0]),plan_sha256=digest(reused[0]/'plan.json')) if reused else None,
                methods={m:METHODS[m] for m in methods},measurement=args.measurement,
                timing='Complete native decode, result staging and reset; excludes setup, warmup, validation and I/O',
                aggregation='sum(elapsed_ns)/sum(shots); also emit median of batch means')
    write(root/'plan.json', plan)
    calls, inputs = [], []
    try:
        for stream, (d,p) in enumerate(((d,p) for p in rates for d in distances), 1):
            case = f'd{d:02d}-p{p:g}'
            data_dir = root/'inputs'/case
            data_dir.mkdir(parents=True)
            dataset = data_dir/'dataset.bin'
            if reused:
                if case not in reused[2]:
                    raise ValueError(f'Requested case absent from saved inputs: {case}')
                for name in ('dataset.bin', 'model.dem', 'circuit.stim'):
                    shutil.copyfile(reused[0]/'inputs'/case/name, data_dir/name)
                with dataset.open('rb') as f:
                    raw = f.read(96)
                if len(raw) != 96:
                    raise ValueError('Truncated saved input header')
                header = struct.unpack('<12Q', raw)
                if (header[0] != 0x5354454e43494c31 or header[1] not in (2, 3) or header[2] != d
                        or struct.unpack('<d', struct.pack('<Q', header[3]))[0] != p or header[4] != shots):
                    raise ValueError('Saved input header and planned workload differ')
                stream = header[11]
                verify_relative_files(root, reused[2][case]['files'])
            common = ['--distance',str(d),'--error-rate',str(p),'--shots',str(shots),'--dataset',str(dataset),'--root-seed',str(seed),'--stream-id',str(stream)]
            if not reused:
                prepared=invoke([str(reference),*common,'--mode','prepare','--main-cpu',str(cpus[0])],root/'preparation'/case)
                for name in ('model.dem','circuit.stim'):
                    shutil.copyfile(prepared/name,data_dir/name)
            inputs.append(dict(case=case,files={str(f.relative_to(root)):digest(f) for f in data_dir.iterdir() if f.is_file()}))
            for repeat in range(repeats):
                order = methods[repeat % len(methods):] + methods[:repeat % len(methods)]
                for method in order:
                    options = dict(METHODS[method])
                    threads = options.pop('threads')
                    options.setdefault('backend',args.backend)
                    mode = args.measurement
                    if mode == 'statistics':mode='pipe-statistics'
                    if mode == 'batch' and options['execution']=='pipeline':mode='pipe-batch'
                    if mode == 'latency' and options['execution']=='pipeline':mode='pipe-latency'
                    command=[str(reference if method=='reference' else binary),*common,'--mode',mode,
                             '--cpus',','.join(map(str,cpus[:threads])),'--warmup-seconds','2',
                             '--verify','weight','--block-size','1024','--repeats','1']
                    for key,value in options.items():command += ['--'+key.replace('_','-'),str(value)]
                    output=invoke(command,root/'calls'/case/method/f'{repeat+1:02d}')
                    correct=json.loads((output/'correctness.json').read_text())
                    if correct['status']!='passed' or correct['weight_differences']!=0:
                        raise ValueError(f'Weight validation failed: {output}')
                    calls.append(dict(case=case,distance=d,error_rate=p,method=method,repeat=repeat+1,
                                      directory=str(output.relative_to(root)),
                                      files={str(f.relative_to(root)):digest(f) for f in output.iterdir() if f.is_file()}))
                    print(f'{case} {method} repeat {repeat+1}: passed',flush=True)
        if source_identity() != identity or digest(binary) != plan['binary']['sha256'] or digest(reference) != plan['reference']['sha256'] or digest(__file__) != plan['collector_sha256']:
            raise ValueError('Source, collector or executable changed during collection')
        write(root/'complete.json',dict(complete=True,plan_sha256=digest(root/'plan.json'),inputs=inputs,calls=calls))
    except BaseException as e:
        write(root/'failure.json',dict(error=str(e),completed_calls=len(calls)))
        raise
    analyze(root)


def analyze(root):
    root=Path(root).resolve()
    done=json.loads((root/'complete.json').read_text())
    if not done['complete'] or digest(root/'plan.json')!=done['plan_sha256']:
        raise ValueError('Incomplete or changed plan')
    plan=json.loads((root/'plan.json').read_text())
    expected={(d,p,m,r) for d in plan['distances'] for p in plan['error_rates'] for m in plan['methods'] for r in range(1,plan['repeats']+1)}
    actual={(c['distance'],c['error_rate'],c['method'],c['repeat']) for c in done['calls']}
    if actual!=expected or len(done['calls'])!=len(expected):raise ValueError('Incomplete/duplicate call matrix')
    for item in done['inputs']+done['calls']:
        for name,sha in item['files'].items():
            p=(root/name).resolve()
            if root not in p.parents or digest(p)!=sha:raise ValueError(f'Changed artifact: {name}')
    groups={}
    if plan['measurement']=='statistics':
        statistics_rows=[]
        for call in done['calls']:
            data=json.loads((root/call['directory']/'syndromes.json').read_text())
            if not data['complete'] or data['timed'] or data['shots']!=plan['shots']:raise ValueError('Invalid statistics replay')
            totals={key:sum(c[key] for c in data['components']) for key in ('normalized','residual','pairs','boundaries')}
            if totals['normalized']!=totals['residual']+2*totals['pairs']+totals['boundaries']:raise ValueError('Residual conservation failed')
            statistics_rows.append(dict(distance=call['distance'],error_rate=call['error_rate'],method=call['method'],repeat=call['repeat'],policy=data['mode'],shots=data['shots'],**totals,mean_residual=totals['residual']/data['shots'],matched_percent=100*(totals['normalized']-totals['residual'])/totals['normalized'] if totals['normalized'] else 0))
        target=root/'statistics.csv'
        if not target.exists():
            with target.open('x',newline='') as f:
                writer=csv.DictWriter(f,list(statistics_rows[0]));writer.writeheader();writer.writerows(statistics_rows)
        print(target)
        return
    if plan['measurement']=='pmu':
        print(f'Verified {len(done["calls"])} PMU calls; counters in each native/pmu.json')
        return
    for call in done['calls']:
        with (root/call['directory']/'measurement.csv').open() as f:rows=list(csv.DictReader(f))
        if len(rows)!=1:raise ValueError('Expected one native measurement row')
        row=rows[0]
        groups.setdefault((call['distance'],call['error_rate'],call['method']),[]).append((int(row['elapsed_ns']),int(row['shots'])))
    target=root/'summary.csv'
    if target.exists():
        print(f'Verified {len(done["calls"])} calls; existing summary retained: {target}')
        return
    with target.open('x',newline='') as f:
        writer=csv.writer(f);writer.writerow(['distance','error_rate','method','repeats','independent_shots','mean_us','median_batch_us'])
        for (d,p,m),values in sorted(groups.items()):
            writer.writerow([d,p,m,len(values),plan['shots'],sum(v[0] for v in values)/sum(v[1] for v in values)/1000,statistics.median(v[0]/v[1]/1000 for v in values)])
    print(target)


def external_command(args):
    command = [sys.executable, str(HERE / 'external/collect.py')]
    for name in ('ae_run', 'build', 'output', 'profile', 'cpus', 'limit', 'shot_counts',
                 'warmup_root', 'warmup_prefix', 'repeats', 'warmup_seconds', 'timeout'):
        value = getattr(args, name, None)
        if value is not None:
            command.extend(['--' + name.replace('_', '-'), str(value)])
    if args.methods:
        command.extend(['--methods', *args.methods])
    return command


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    run=sub.add_parser('run')
    run.add_argument('--profile',choices=['smoke','paper'],default='smoke')
    run.add_argument('--binary',type=Path,required=True)
    run.add_argument('--build-record',type=Path,help='Candidate build.json from AE/build_native.py; required for paper runs')
    run.add_argument('--reference-binary',type=Path)
    run.add_argument('--reference-record',type=Path)
    run.add_argument('--output',type=Path,required=True)
    run.add_argument('--input-run',type=Path,help='Reuse sealed saved inputs from a completed AE run without sampling')
    run.add_argument('--backend',choices=['scalar','avx2','avx512vl','avx512'],default='scalar')
    run.add_argument('--methods',help=','.join(METHODS))
    run.add_argument('--distances');run.add_argument('--error-rates');run.add_argument('--cpus')
    run.add_argument('--shots',type=int);run.add_argument('--repeats',type=int)
    run.add_argument('--seed',type=int,help='Default 20261112; input reuse preserves the original seed')
    run.add_argument('--measurement',choices=['batch','latency','pmu','statistics'],default='batch')
    check=sub.add_parser('analyze');check.add_argument('run',type=Path)
    external=sub.add_parser('external',help='Collect all external methods on a completed native run input matrix')
    external.add_argument('--ae-run',type=Path,required=True)
    external.add_argument('--build',type=Path,required=True)
    external.add_argument('--output',type=Path,required=True)
    external.add_argument('--profile',choices=['smoke','paper'],default='smoke')
    external.add_argument('--cpus')
    external.add_argument('--methods',nargs='+',choices=['parity','fusion','parity-uf','unionfind','ldpc-uf'])
    selection=external.add_mutually_exclusive_group()
    selection.add_argument('--limit',type=int)
    selection.add_argument('--shot-counts',type=Path)
    external.add_argument('--warmup-root',type=Path)
    external.add_argument('--warmup-prefix',type=int)
    external.add_argument('--repeats',type=int)
    external.add_argument('--warmup-seconds',type=float)
    external.add_argument('--timeout',type=float)
    args=parser.parse_args(argv)
    try:
        if args.command=='run':collect(args)
        elif args.command=='external':return subprocess.run(external_command(args)).returncode
        else:analyze(args.run)
    except (OSError,ValueError,RuntimeError,KeyError) as e:
        print(str(e),file=sys.stderr);return 1
    return 0

if __name__=='__main__':raise SystemExit(main())
