#!/usr/bin/env python3
"""Build once, then collect complete case bundles serially within Slurm workers.

  python AE/slurm.py run --work /shared/ae-paper --nodes node1,node2 --partition batch
  python AE/slurm.py prepare --work /shared/ae-smoke --profile smoke --nodes node1
  python AE/slurm.py resume --work /shared/ae-paper

The default is a fresh paper acquisition, not archived replotting. Shared source,
Python environment, work directory and binaries must be visible on every node.
All six methods (including smoke) need eight physical cores on one socket.
No cluster name or private cache is assumed; resource flags are site-specific.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import fcntl
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

try:
    from . import run as local
except ImportError:
    import run as local


def now():
    return datetime.now(timezone.utc).isoformat()


def case_name(distance, rate):
    return f'd{distance}-p{rate:g}'


def assign_cases(cases, count):
    """Deterministic cost proxy for scheduling only; never selects measured results."""
    bins = [[] for _ in range(min(count, len(cases)))]
    load = [0.] * len(bins)
    for distance, rate in sorted(cases, key=lambda c: (-c[0] ** 3 * c[1], -c[0], -c[1])):
        index = min(range(len(bins)), key=lambda i: (load[i], i))
        bins[index].append({'distance': distance, 'error_rate': rate, 'name': case_name(distance, rate)})
        load[index] += distance ** 3 * rate
    return bins


def forwarded(args):
    values = ['--source', str(args.source.resolve()), '--profile', args.profile, '--python', args.python,
              '--jobs', str(args.jobs), '--pmu', args.pmu]
    if args.external_shots is not None:
        values += ['--external-shots', str(args.external_shots)]
    for option in ('toolchain', 'dependencies_from', 'stim_source', 'stim_archive', 'googletest_source',
                   'upstream_repository', 'official_source', 'reference_adapter', 'crate_cache',
                   'reference_build', 'reference_record', 'native_build', 'external_build', 'external_shot_counts'):
        value = getattr(args, option, None)
        if value is not None:
            values += ['--' + option.replace('_', '-'), str(value.resolve() if isinstance(value, Path) else value)]
    if args.offline:
        values += ['--offline']
    return values


def prepare(args):
    work = args.work.resolve()
    source = args.source.resolve()
    if work == source or source in work.parents:
        raise ValueError('Keep the shared --work directory outside the source checkout')
    if (work / 'plan.json').exists():
        raise ValueError('This Slurm plan already exists; use submit or resume with only --work')
    if args.shards < 1 or args.cpus_per_task < 8 or args.jobs < 1:
        raise ValueError('Use positive shards/jobs and --cpus-per-task >= 8 (eight same-socket physical cores are required)')
    nodes = args.nodes.split(',') if args.nodes else []
    if any(not node or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._' for c in node) for node in nodes) or len(nodes) != len(set(nodes)):
        raise ValueError('--nodes accepts distinct comma-separated host names (expand bracket ranges first)')
    distances = [int(x) for x in args.distances.split(',')] if args.distances else (local.DISTANCES if args.profile == 'paper' else [9])
    rates = [float(x) for x in args.error_rates.split(',')] if args.error_rates else (local.ERROR_RATES if args.profile == 'paper' else [.003])
    if not distances or len(set(distances)) != len(distances) or any(d < 3 or d % 2 == 0 for d in distances):
        raise ValueError('Distances must be distinct odd integers >= 3')
    if not rates or len(set(rates)) != len(rates) or any(not 0 < p < .5 for p in rates):
        raise ValueError('Error rates must be distinct values between 0 and .5')
    if args.profile == 'paper' and (set(distances) != set(local.DISTANCES) or set(rates) != set(local.ERROR_RATES)):
        raise ValueError('The automatic paper export needs all 21 cases; use smoke for a small Slurm trial or run.py --no-export for custom case bundles')
    current = local.identity(source)
    if args.profile == 'paper' and (not current['commit'] or current['status']):
        raise ValueError('Paper acquisition requires a clean committed --source checkout')
    cases = [(d, p) for d in distances for p in rates]
    bins = assign_cases(cases, len(nodes) if nodes else args.shards)
    work.mkdir(parents=True, exist_ok=True)
    (work / 'logs').mkdir(exist_ok=True)
    (work / 'jobs').mkdir(exist_ok=True)
    manifest = args.builds_manifest.resolve() if args.builds_manifest else work / 'shared-build/builds.json'
    build_command = [args.python, str(source / 'AE/run.py'), 'build', '--work', str(work / 'shared-build'), *forwarded(args)]
    plan = {'schema': 1, 'created_utc': now(), 'source': str(source), 'source_identity': current,
            'controller_sha256': local.digest(__file__), 'local_entry_sha256': local.digest(source / 'AE/run.py'),
            'work': str(work), 'python': args.python, 'profile': args.profile,
            'backend': args.backend or local.cpu_features()['backend'], 'cpus': args.cpus,
            'pmu': args.pmu, 'render': not args.no_render,
            'native_shots_per_case': 1000000 if args.profile == 'paper' else 1025,
            'repeats': 3 if args.profile == 'paper' else 1,
            'external_requested_shots': args.external_shots,
            'external_shots': args.external_shots if args.profile == 'paper' else 32,
            'external_shot_counts': local.resolve_shot_counts(args.profile, cases,
                args.external_shots if args.profile == 'paper' else 32,
                local.read(args.external_shot_counts) if args.external_shot_counts else None),
            'builds_manifest': str(manifest), 'build_command': None if args.builds_manifest else build_command,
            'resources': {'partition': args.partition, 'cpus_per_task': args.cpus_per_task,
                          'memory': args.mem, 'time': args.time, 'exclusive': not args.shared_node},
            'assignment': 'descending d^3*p scheduling proxy; all methods and repeats of one case stay together',
            'shards': [{'index': i, 'node': nodes[i] if nodes else None, 'cases': cases} for i, cases in enumerate(bins)],
            'excluded': ['Apple M4 acquisition'], 'archived_data_used_as_measurements': False}
    if plan['external_shot_counts'] is not None:
        local.save(work / 'external-shot-counts.json', plan['external_shot_counts'])
    local.save(work / 'plan.json', plan)
    for shard in plan['shards']:
        command = [args.python, str(source / 'AE/slurm.py'), 'worker', '--work', str(work), '--shard', str(shard['index'])]
        (work / f"jobs/worker-{shard['index']}.sh").write_text('#!/bin/bash\nset -eu\nexec ' + shlex.join(command) + '\n')
    command = [args.python, str(source / 'AE/slurm.py'), 'export', '--work', str(work)]
    (work / 'jobs/export.sh').write_text('#!/bin/bash\nset -eu\nexec ' + shlex.join(command) + '\n')
    print(f'Prepared {len(cases)} cases in {len(bins)} workers: {work / "plan.json"}', flush=True)
    return plan


def load_plan(work):
    plan = local.read(Path(work).resolve() / 'plan.json')
    if plan['source_identity'] != local.identity(plan['source']):
        raise ValueError('Source changed since preparation; use a fresh fixed checkout and work directory')
    if local.digest(__file__) != plan['controller_sha256'] or local.digest(Path(plan['source']) / 'AE/run.py') != plan['local_entry_sha256']:
        raise ValueError('Workflow entry changed since preparation')
    submissions = Path(plan['work']) / 'submissions.json'
    if submissions.exists() and local.read(submissions).get('plan_sha256'):
        verify_submission(plan)
    return plan


def verify_submission(plan):
    root = Path(plan['work'])
    record = local.read(root / 'submissions.json')
    if record.get('plan_sha256') != local.digest(root / 'plan.json'):
        raise ValueError('Submitted Slurm plan changed')
    if record.get('builds_manifest_sha256') != local.digest(plan['builds_manifest']):
        raise ValueError('Submitted shared build manifest changed')
    return record


def sbatch_command(plan, shard=None, dependencies=None):
    root = Path(plan['work'])
    resources = plan['resources']
    name = f"worker-{shard['index']}" if shard else 'export'
    command = ['sbatch', '--parsable', '--nodes=1', '--ntasks=1', '--hint=nomultithread',
               f"--cpus-per-task={resources['cpus_per_task'] if shard else 1}",
               f"--job-name=bb-ae-{name}", f'--output={root}/logs/{name}-%j.log']
    if resources['partition']:
        command.append('--partition=' + resources['partition'])
    if resources['memory'] is not None:
        command.append('--mem=' + (resources['memory'] if shard else '4G'))
    if resources['time']:
        command.append('--time=' + resources['time'])
    if shard and resources['exclusive']:
        command.append('--exclusive')
    requested_node = shard['node'] if shard else plan['shards'][0]['node']
    if requested_node:
        command.append('--nodelist=' + requested_node)
    if dependencies:
        command.append('--dependency=afterok:' + ':'.join(dependencies))
    command.append(str(root / f'jobs/{name}.sh'))
    return command


def ensure_builds(plan):
    manifest = Path(plan['builds_manifest'])
    if not manifest.exists():
        if not plan['build_command']:
            raise ValueError(f'Missing supplied shared build manifest: {manifest}')
        command = list(plan['build_command'])
        work = Path(plan['work']) / 'shared-build'
        if (work / 'config.json').exists():
            # A failed build retries only incomplete build stages, never starts collection.
            command[2] = 'build-resume'
            command = command[:3] + ['--work', str(work)]
        with (Path(plan['work']) / 'logs/shared-build.log').open('a') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    record = local.read(manifest)
    if not record.get('complete') or record['source_identity'] != plan['source_identity']:
        raise ValueError('Shared build source identity does not match this Slurm plan')
    return local.digest(manifest)


def submit(plan, resume=False):
    root = Path(plan['work'])
    submissions_path = root / 'submissions.json'
    previous = local.read(submissions_path) if submissions_path.exists() else {'workers': {}, 'exports': []}
    if previous.get('plan_sha256'):
        verify_submission(plan)
    if previous['workers'] and not resume:
        raise ValueError('Jobs were already submitted; use resume after all jobs are terminal')
    if resume:
        def active(ids):
            if not ids:
                return {}
            command = ['squeue', '--noheader', '--jobs=' + ','.join(ids), '--format=%i|%T|%r']
            result = subprocess.run(command, capture_output=True, text=True)
            # Slurm can return 1 for IDs that already aged out of its live queue.
            if result.returncode and not (not result.stdout.strip() and 'Invalid job id' in result.stderr):
                raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
            rows = [line.strip().split('|', 2) for line in result.stdout.splitlines() if line.strip()]
            if any(len(row) != 3 or row[0] not in ids for row in rows):
                raise ValueError('Unexpected Slurm queue response')
            return {row[0]: row[1:] for row in rows}
        worker_ids = [r['job_id'] for attempts in previous['workers'].values() for r in attempts]
        workers = active(worker_ids)
        if workers:
            raise ValueError('Prior workers are still active; resume after they finish: ' + ', '.join(workers))
        exports = active([r['job_id'] for r in previous['exports'] if not r.get('cancelled_utc')])
        missing_worker = any(not (root / f"workers/{s['index']}/complete.json").exists() for s in plan['shards'])
        for job, (state, reason) in exports.items():
            if state != 'PENDING' or not missing_worker or reason not in ('Dependency', 'DependencyNeverSatisfied'):
                raise ValueError('Prior export is still active; wait before resuming: ' + job)
            # Only our recorded dependency export is cancelled, after all worker jobs
            # have left the queue and at least one worker lacks a completed bundle.
            subprocess.run(['scancel', job], check=True)
            record = next(r for r in previous['exports'] if r['job_id'] == job)
            record.update(cancelled_utc=now(), cancellation_reason='Terminal worker without complete bundle; stale afterok export',
                          observed_state=state, observed_reason=reason)
            local.save(submissions_path, previous)
    manifest_sha = ensure_builds(plan)
    previous.update(builds_manifest_sha256=manifest_sha, plan_sha256=local.digest(root / 'plan.json'))
    local.save(submissions_path, previous)
    pending = []
    def launch(command):
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        job = result.stdout.strip().split(';')[0]
        if not job.isdigit():
            raise ValueError('sbatch did not return a parsable job ID: ' + result.stdout)
        return {'job_id': job, 'command': command, 'submitted_utc': now()}
    for shard in plan['shards']:
        done = root / f"workers/{shard['index']}/complete.json"
        if done.exists() and local.read(done).get('complete'):
            continue
        allocation = dict(shard)
        prior_placement = root / f"workers/{shard['index']}/placement.json"
        if resume and not allocation['node'] and prior_placement.exists():
            allocation['node'] = local.read(prior_placement)['node']
        record = launch(sbatch_command(plan, allocation))
        previous['workers'].setdefault(str(shard['index']), []).append(record)
        pending.append(record['job_id'])
        local.save(submissions_path, previous)
    if not (root / 'complete.json').exists():
        record = launch(sbatch_command(plan, dependencies=pending))
        previous['exports'].append(record)
        local.save(submissions_path, previous)
    print('Submitted worker jobs: ' + ', '.join(pending) + '; export runs after successful workers.', flush=True)


def worker(plan, index):
    if not os.environ.get('SLURM_JOB_ID'):
        raise ValueError('The worker entry must run inside a Slurm allocation')
    root = Path(plan['work'])
    verify_submission(plan)
    shard = plan['shards'][index]
    hostname = os.environ.get('SLURMD_NODENAME', platform.node())
    if shard['node'] and hostname.split('.')[0] != shard['node'].split('.')[0]:
        raise ValueError('This worker was allocated to a different node than its case assignment')
    cpus = local.select_cpus(plan['cpus'])
    hardware = local.cpu_features()
    rank = {'scalar': 0, 'avx2': 1, 'avx512vl': 2, 'avx512': 2}
    if rank[plan['backend']] > rank[hardware['backend']]:
        raise ValueError('This worker does not support the shared planned SIMD backend')
    worker_root = root / f'workers/{index}'
    worker_root.mkdir(parents=True, exist_ok=True)
    record = {'complete': False, 'started_utc': now(), 'host': platform.node(), 'node': hostname,
              'job_id': os.environ['SLURM_JOB_ID'], 'cpus': cpus, 'affinity': sorted(os.sched_getaffinity(0)),
              'hardware': hardware, 'plan_sha256': local.digest(root / 'plan.json'),
              'builds_manifest_sha256': local.digest(plan['builds_manifest']), 'cases': []}
    with (worker_root / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        local.save(worker_root / 'placement.json', record)
        try:
            for case in shard['cases']:
                case_root = root / 'cases' / case['name']
                existing = case_root / 'config.json'
                if existing.exists():
                    config = local.read(existing)
                    if config['placement']['host'] != platform.node() or config['cpus'] != cpus:
                        raise ValueError('Resuming a case requires its original host and CPU placement')
                    command = [plan['python'], str(Path(plan['source']) / 'AE/run.py'), 'resume', '--work', str(case_root)]
                else:
                    command = [plan['python'], str(Path(plan['source']) / 'AE/run.py'), 'run', '--work', str(case_root),
                        '--source', plan['source'], '--python', plan['python'], '--profile', plan['profile'],
                        '--builds-manifest', plan['builds_manifest'], '--distances', str(case['distance']),
                        '--error-rates', str(case['error_rate']), '--cpus', ','.join(map(str, cpus)),
                        '--backend', plan['backend'], '--pmu', plan['pmu'], '--no-export']
                    if plan['external_requested_shots'] is not None:
                        command += ['--external-shots', str(plan['external_requested_shots'])]
                    if plan['external_shot_counts'] is not None:
                        command += ['--external-shot-counts', str(root / 'external-shot-counts.json')]
                print(f"{case['name']}: {shlex.join(command)}", flush=True)
                subprocess.run(command, check=True)
                completion = local.read(case_root / 'complete.json')
                if not completion.get('complete'):
                    raise ValueError('Case did not finish: ' + case['name'])
                record['cases'].append({'name': case['name'], 'complete': str(case_root / 'complete.json'),
                                        'sha256': local.digest(case_root / 'complete.json')})
            record.update(complete=True, finished_utc=now())
            local.save(worker_root / 'complete.json', record)
        except BaseException as error:
            record.update(error=str(error), finished_utc=now())
            local.save(worker_root / 'failure.json', record)
            raise


def export(plan):
    root = Path(plan['work'])
    frozen = verify_submission(plan)
    bundles, placements = [], []
    for shard in plan['shards']:
        done = local.read(root / f"workers/{shard['index']}/complete.json")
        expected = {c['name']: c for c in shard['cases']}
        if (not done.get('complete') or len(done['cases']) != len(expected)
                or {c['name'] for c in done['cases']} != set(expected)):
            raise ValueError('A worker has not completed its assigned cases')
        if done['plan_sha256'] != frozen['plan_sha256'] or done['builds_manifest_sha256'] != frozen['builds_manifest_sha256']:
            raise ValueError('Worker receipts use another plan or shared build')
        if shard['node'] and done['node'].split('.')[0] != shard['node'].split('.')[0]:
            raise ValueError('Worker completed on another assigned node')
        for case in done['cases']:
            case_root = root / 'cases' / case['name']
            if Path(case['complete']).resolve() != case_root / 'complete.json':
                raise ValueError('Worker case completion path differs from assignment')
            if local.digest(case['complete']) != case['sha256']:
                raise ValueError('Case completion changed after the worker finished')
            completion = local.read(case['complete'])
            config = local.read(case_root / 'config.json')
            planned = expected[case['name']]
            if (not completion.get('complete') or completion['config_sha256'] != local.digest(case_root / 'config.json')
                    or config['distances'] != [planned['distance']] or config['error_rates'] != [planned['error_rate']]
                    or config['backend'] != plan['backend'] or config['source_identity'] != plan['source_identity']
                    or config['placement']['host'] != done['host'] or config['cpus'] != done['cpus']):
                raise ValueError('Case configuration differs from its assigned case/host/CPU/backend')
            bundles.append(completion['bundle'])
        placements.append(done)
    manifest = {'schema': 1, 'shards': bundles, 'placements': placements, 'plan_sha256': frozen['plan_sha256']}
    shards_path = root / 'shards.json'
    if shards_path.exists() and local.read(shards_path) != manifest:
        raise ValueError('Completed shard manifest changed')
    if not shards_path.exists():
        local.save(shards_path, manifest)
    shards_sha = local.digest(shards_path)
    def verify_export(output):
        receipt = local.read(output / 'complete.json')
        binding = receipt.get('sources', {}).get('shards_manifest', {}).get('manifest', {})
        if not receipt.get('complete') or binding.get('sha256') != shards_sha:
            raise ValueError('Existing export belongs to another shard manifest')
        for relative, sha in receipt['files'].items():
            if local.digest(output / relative) != sha:
                raise ValueError('Export artifact changed: ' + relative)
        return local.digest(output / 'complete.json')
    completed = root / 'complete.json'
    if completed.exists():
        previous = local.read(completed)
        if previous.get('plan_sha256') != frozen['plan_sha256'] or previous.get('shards_sha256') != shards_sha:
            raise ValueError('Existing workflow completion belongs to another plan or shards')
        output = Path(previous['export'])
        if verify_export(output) != previous.get('export_receipt_sha256'):
            raise ValueError('Completed export receipt changed')
        print(completed, flush=True)
        return
    output = root / 'exports'
    attempt = 1
    while output.exists() and not (output / 'complete.json').exists():
        attempt += 1
        output = root / f'exports-attempt-{attempt}'
    if not output.exists():
        command = [plan['python'], str(Path(plan['source']) / 'AE/export_results.py'), '--shards', str(shards_path),
                   '--profile', plan['profile'], '--groups', 'ablation', 'comparison', 'predecoder', 'xeon-macros',
                   '--output', str(output), *([] if plan['render'] else ['--no-render'])]
        subprocess.run(command, check=True)
    receipt_sha = verify_export(output)
    local.save(completed, {'complete': True, 'finished_utc': now(), 'profile': plan['profile'],
        'cases': len(bundles), 'plan_sha256': frozen['plan_sha256'], 'shards_sha256': shards_sha,
        'export': str(output), 'export_receipt_sha256': receipt_sha, 'excluded': plan['excluded']})
    print(completed, flush=True)


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', nargs='?', default='run', choices=['run', 'prepare', 'submit', 'resume', 'worker', 'export'])
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parent.parent)
    p.add_argument('--nodes', help='Optional comma-separated nodes; otherwise Slurm chooses nodes')
    p.add_argument('--shards', type=int, default=6, help='Workers when --nodes is absent (default: 6)')
    p.add_argument('--partition', help='Cluster partition; default uses scheduler default')
    p.add_argument('--cpus-per-task', type=int, default=8, help='Allocate enough CPUs for 8 same-socket physical cores; full-node count is site-specific')
    p.add_argument('--mem', default='0', help='Slurm memory request (default: 0, all node memory)')
    p.add_argument('--time', help='Slurm wall time; default follows partition policy')
    p.add_argument('--shared-node', action='store_true', help='Opt out of exclusive worker nodes')
    p.add_argument('--shard', type=int, help=argparse.SUPPRESS)
    # Keep build and measurement options identical to the local workflow.
    for action in local.parser()._actions:
        if action.dest in ('help', 'action', 'work', 'source', 'no_export'):
            continue
        p._add_action(action)
    return p


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parser().parse_args(argv)
    try:
        if args.action in ('run', 'prepare'):
            plan = prepare(args)
            if args.action == 'prepare':
                return 0
        else:
            allowed = {'--work', '--shard'} if args.action == 'worker' else {'--work'}
            if any(a.startswith('--') and a.split('=')[0] not in allowed for a in argv):
                raise ValueError('Use the saved plan with only --work (and --shard for a worker)')
            plan = load_plan(args.work)
        if args.action == 'worker':
            if args.shard is None or not 0 <= args.shard < len(plan['shards']):
                raise ValueError('Invalid worker index')
            worker(plan, args.shard)
        elif args.action == 'export':
            export(plan)
        else:
            with (Path(plan['work']) / '.submit.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                submit(plan, resume=args.action == 'resume')
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
