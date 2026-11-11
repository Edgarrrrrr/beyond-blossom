#!/usr/bin/env python3
"""General-N adapters for frozen PM executables; no historical collector globals."""
from __future__ import annotations
import argparse
import csv
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

METHODS = ('Upstream', 'GridSIMD', 'Predecode', 'Component', 'TailReset', 'Spatial')
ENVIRONMENT = dict(OMP_NUM_THREADS='1', OMP_DYNAMIC='FALSE', OMP_PROC_BIND='FALSE',
    OMP_WAIT_POLICY='PASSIVE', GOMP_SPINCOUNT='0', OMP_THREAD_LIMIT='8', OMP_MAX_ACTIVE_LEVELS='1',
    OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1', NUMEXPR_NUM_THREADS='1',
    LD_LIBRARY_PATH='', LD_PRELOAD='', PM_PREDECODE_PMU='off', PM_IMPORT_PMU='off', PYTHONDONTWRITEBYTECODE='1')


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            result.update(block)
    return result.hexdigest()


def ref(path, expected=None):
    path = Path(path).resolve()
    result = dict(path=str(path), sha256=sha(path))
    need(expected is None or result['sha256'] == expected, 'Frozen file changed: ' + str(path))
    return result


def load(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f'.{os.getpid()}.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def utc():
    return datetime.now(timezone.utc).isoformat()


def environment(method):
    env = {k: v for k, v in os.environ.items() if not k.startswith('PM_') and k not in ('OMP_PLACES', 'GOMP_CPU_AFFINITY')}
    env.update(ENVIRONMENT)
    if method == 'Spatial':
        env.update(PM_FIXED_SEED_CONSTRUCTION='fused', PM_FIXED_SEED_INPUT='indexed',
                   PM_IMPORT_PREPARATION='captained_serial', PM_IMPORT_RESET_OWNER='captain')
    return env


def lane():
    groups = {}
    allocation = sorted(os.sched_getaffinity(0))
    for cpu in allocation:
        path = Path(f'/sys/devices/system/cpu/cpu{cpu}')
        socket = int((path / 'topology/physical_package_id').read_text())
        core = int((path / 'topology/core_id').read_text())
        nodes = [int(p.name[4:]) for p in path.glob('node[0-9]*')]
        if len(nodes) == 1 and nodes[0] in (0, 1):
            groups.setdefault((socket, nodes[0]), {}).setdefault(core, cpu)
    for (socket, numa), cores in sorted(groups.items()):
        if len(cores) >= 9:
            cpus = sorted(cores.values())[:9]
            os.sched_setaffinity(0, {cpus[8]})
            return dict(socket=socket, numa_node=numa, measurement_cpus=cpus[:8], controller_cpu=cpus[8],
                allocation_affinity=allocation, physical_cores={str(c): int(Path(f'/sys/devices/system/cpu/cpu{c}/topology/core_id').read_text()) for c in cpus})
    raise ValueError('Need nine distinct physical cores on one NUMA node')


def bound_exec(args):
    """Same strict MPOL_BIND checks as the frozen launcher; accepts any native CLI."""
    cpus = sorted(map(int, args.cpus.split(',')))
    need(os.uname().machine == 'x86_64' and args.numa in (0, 1), 'Unsupported NUMA ABI')
    need(cpus and len(set(cpus)) == len(cpus) and set(cpus).issubset(os.sched_getaffinity(0)), 'CPU lane outside allocation')
    for cpu in cpus:
        nodes = [int(p.name[4:]) for p in Path(f'/sys/devices/system/cpu/cpu{cpu}').glob('node[0-9]*')]
        need(nodes == [args.numa], 'CPU outside requested NUMA node')
    os.sched_setaffinity(0, set(cpus))
    import re
    header = Path('/usr/include/x86_64-linux-gnu/asm/unistd_64.h')
    calls = [int(re.search(r'^#define __NR_' + name + r'\s+(\d+)$', header.read_text(), re.M).group(1))
             for name in ('set_mempolicy', 'get_mempolicy')]
    need(calls == [238, 239], 'Unexpected syscall ABI')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    personality=None
    if args.disable_aslr:
        libc.personality.argtypes=[ctypes.c_ulong]
        libc.personality.restype=ctypes.c_int
        prior=libc.personality(ctypes.c_ulong(0xffffffff))
        if prior<0:raise OSError(ctypes.get_errno(),'Cannot read child personality')
        if libc.personality(ctypes.c_ulong(prior|0x0040000))<0:
            raise OSError(ctypes.get_errno(),'Cannot disable child ASLR')
        personality=libc.personality(ctypes.c_ulong(0xffffffff))
        need(personality>=0 and personality&0x0040000,'Child ADDR_NO_RANDOMIZE not set')
    mask = ctypes.c_ulong(1 << args.numa)
    if libc.syscall(ctypes.c_long(calls[0]), ctypes.c_int(2), ctypes.byref(mask), ctypes.c_ulong(64)):
        raise OSError(ctypes.get_errno(), 'set_mempolicy(MPOL_BIND) failed')
    mode, actual = ctypes.c_int(), ctypes.c_ulong()
    if libc.syscall(ctypes.c_long(calls[1]), ctypes.byref(mode), ctypes.byref(actual), ctypes.c_ulong(64), ctypes.c_void_p(), ctypes.c_ulong(0)):
        raise OSError(ctypes.get_errno(), 'get_mempolicy failed')
    need(mode.value == 2 and actual.value == mask.value, 'Strict NUMA policy differs')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    need(command and Path(command[0]).is_absolute(), 'Absolute native executable required')
    write(args.receipt, dict(status='strict_bind_verified_before_exec', numa_node=args.numa,
        child_cpus=cpus, host=os.uname().nodename, native=ref(command[0]), launcher=ref(__file__), utc=utc(),
        disable_aslr=args.disable_aslr,personality=personality))
    os.execv(command[0], command)


def invoke(plan, machine, command, directory, cpus, method, detail, pm_output=True):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    # Frozen PM executables require a fresh native-artifact directory. Keep
    # controller receipts beside it so the same output path works for PM,
    # external decoders and the separate predecode statistics executable.
    controller = directory.with_name(directory.name + '.controller')
    controller.mkdir(parents=True, exist_ok=False)
    policy = controller / 'numa-policy.json'
    if pm_output:
        command = [*command, '--main-cpu', str(cpus[0]), '--cpus', ','.join(map(str, cpus)), '--output-dir', str(directory)]
    argv = [sys.executable, str(Path(__file__).resolve()), 'exec-bound', '--numa', str(machine['numa_node']),
        '--cpus', ','.join(map(str, cpus)), '--receipt', str(policy)]
    if plan.get('protocol',{}).get('disable_aslr'):argv+=['--disable-aslr']
    argv+=['--',*map(str,command)]
    record = dict(status='running', argv=argv, detail=detail, started_utc=utc(),
        native_output=str(directory.resolve()), controller_output=str(controller.resolve()),
        environment={k: v for k, v in environment(method).items() if k in ENVIRONMENT or k.startswith('PM_')})
    write(controller / 'attempt.json', record)
    started = time.monotonic()
    try:
        # The controller is pinned separately; restore this child's explicit allocated lane.
        with (controller / 'process.log').open('x') as log:
            child=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,env=environment(method),
                preexec_fn=lambda: os.sched_setaffinity(0,set(cpus)))
            if plan.get('protocol',{}).get('disable_aslr'):
                expected=str(Path(command[0]).resolve())
                for attempt in range(100):
                    try:
                        maps=Path(f'/proc/{child.pid}/maps').read_text()
                        if expected in maps and '[heap]' in maps:
                            (controller/'native-maps.txt').write_text(maps);break
                    except FileNotFoundError:break
                    time.sleep(.01)
                ready_deadline=time.monotonic()+plan['timeout_seconds_per_call']
                while child.poll() is None and not (directory/'setup.json').exists():
                    need(time.monotonic()<ready_deadline,'Native setup timed out')
                    time.sleep(.02)
                if (directory/'setup.json').exists():
                    try:(controller/'native-maps-ready.txt').write_text(Path(f'/proc/{child.pid}/maps').read_text())
                    except FileNotFoundError:pass
            try:code=child.wait(timeout=plan['timeout_seconds_per_call'])
            except BaseException:
                child.kill();child.wait();raise
        record['returncode'] = code
        need(code == 0, 'Native call failed: ' + str(directory))
        binding = load(policy)
        need(binding['status'] == 'strict_bind_verified_before_exec' and binding['child_cpus'] == sorted(cpus)
            and binding['numa_node'] == machine['numa_node'], 'NUMA receipt differs')
        if plan.get('protocol',{}).get('disable_aslr'):
            need(binding['disable_aslr'] and binding['personality']&0x0040000,'Child ASLR control differs')
            need((controller/'native-maps.txt').is_file(),'Missing native address-map evidence')
        record['status'] = 'process_completed'
    except BaseException as error:
        record.update(status='failed', error=str(error), error_type=type(error).__name__)
        raise
    finally:
        record.update(finished_utc=utc(), outer_seconds=time.monotonic() - started)
        write(controller / 'attempt.json', record)
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    actions = parser.add_subparsers(dest='action', required=True)
    execute = actions.add_parser('exec-bound')
    execute.add_argument('--numa', type=int, required=True)
    execute.add_argument('--cpus', required=True)
    execute.add_argument('--receipt', type=Path, required=True)
    execute.add_argument('--disable-aslr',action='store_true')
    execute.add_argument('command', nargs=argparse.REMAINDER)
    bound_exec(parser.parse_args())
