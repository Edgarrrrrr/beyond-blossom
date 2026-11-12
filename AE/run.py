#!/usr/bin/env python3
"""Run the local paper workflow: discover dependencies, build, collect serially, export.

Examples:
  python AE/run.py run --work ../ae-work --external-shots 10000
  python AE/run.py resume --work ../ae-work
  python AE/run.py plan --work ../ae-work --offline --reference-build /path/to/reference

Archived figure regeneration remains AE/replot.py. This entry never supplies M4 data.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
UPSTREAM = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
PINS = {
    'stim': ('https://github.com/quantumlib/Stim.git', '1320ad7eac7de34d2e9c70daa44fbc6d84174450'),
    'googletest': ('https://github.com/google/googletest.git', '58d77fa8070e8cec2dc1ed015d66b454c8d78850'),
    'upstream': ('https://github.com/oscarhiggott/PyMatching.git', UPSTREAM),
}
METHODS = 'reference,structured,predecoded,component-parallel,pipelined,parallel-predecode'
POLICY_BUILD_MODES = {0: 0, 1: 1, 2: 3}  # Table slot 2 is true simple half-edge, not historical coordinate mode 2.


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def save(path, data):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True, allow_nan=False) + '\n')
    temporary.replace(path)


def git(source, *args):
    result = subprocess.run(['git', '-C', str(source), *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def identity(source):
    return {'commit': git(source, 'rev-parse', 'HEAD'), 'status': git(source, 'status', '--porcelain')}


def physical_cpus(allowed=None, topology=Path('/sys/devices/system/cpu')):
    if platform.system() != 'Linux':
        raise ValueError('This local workflow targets Linux; M4 uses its separate acquisition procedure')
    allowed = sorted(os.sched_getaffinity(0) if allowed is None else allowed)
    sockets = {}
    for cpu in allowed:
        base = topology / f'cpu{cpu}' / 'topology'
        socket = int((base / 'physical_package_id').read_text())
        core = int((base / 'core_id').read_text())
        sockets.setdefault(socket, {}).setdefault(core, cpu)
    if not sockets:
        raise ValueError('No available physical CPUs')
    socket = min(sockets, key=lambda s: (-len(sockets[s]), s))
    return sorted(sockets[socket].values())


def select_cpus(value):
    available = set(os.sched_getaffinity(0))
    selected = [int(v) for v in value.split(',')] if value else physical_cpus()
    if len(selected) < 8 or len(set(selected)) != len(selected) or not set(selected) <= available:
        raise ValueError('The six paper methods require eight available distinct physical cores on one socket')
    keys = []
    for cpu in selected:
        base = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
        keys.append(((base / 'physical_package_id').read_text().strip(), (base / 'core_id').read_text().strip()))
    if len(set(keys)) != len(keys) or len({x[0] for x in keys}) != 1:
        raise ValueError('--cpus must select distinct physical cores on one socket')
    return selected[:8]


def cpu_features():
    text = Path('/proc/cpuinfo').read_text() if Path('/proc/cpuinfo').exists() else ''
    first = text.split('\n\n')[0]
    fields = {line.split(':', 1)[0].strip(): line.split(':', 1)[1].strip()
              for line in first.splitlines() if ':' in line}
    flags = set(fields.get('flags', '').split())
    backend = 'avx512vl' if {'avx512f', 'avx512vl', 'avx2'} <= flags else 'avx2' if 'avx2' in flags else 'scalar'
    # The L2 raw event encodings in this artifact are specific to this Intel family/model.
    cascade_lake = fields.get('vendor_id') == 'GenuineIntel' and fields.get('cpu family') == '6' and fields.get('model') == '85'
    return {'backend': backend, 'model': fields.get('model name'), 'l2_events_supported_model': cascade_lake}


def dependency_hints(builds):
    result = {}
    aliases = {'FETCHCONTENT_SOURCE_DIR_STIM': 'stim_source', 'stim_SOURCE_DIR': 'stim_source',
               'BB_AE_STIM_ARCHIVE': 'stim_archive', 'CACHED_STIM_ARCHIVE': 'stim_archive',
               'FETCHCONTENT_SOURCE_DIR_GOOGLETEST': 'googletest_source', 'googletest_SOURCE_DIR': 'googletest_source'}
    for root in builds:
        if not root:
            continue
        root = Path(root)
        values = {}
        for file in [root / 'CMakeCache.txt', root / 'build/CMakeCache.txt']:
            if file.exists():
                for line in file.read_text().splitlines():
                    if not line.startswith(('#', '//')) and ':' in line and '=' in line:
                        name, value = line.split('=', 1)
                        values[name.split(':', 1)[0]] = value
        file = root / 'configure-command.json'
        if file.exists():
            for value in read(file):
                if value.startswith('-D') and '=' in value:
                    key, value = value[2:].split('=', 1)
                    values[key] = value
        for key, name in aliases.items():
            if values.get(key) and Path(values[key]).exists():
                result.setdefault(name, str(Path(values[key]).resolve()))
    return result


def python_environment(python):
    code = ('import json, numpy, matplotlib, stim, beyond_blossom; '
            'print(json.dumps({m.__name__:m.__file__ for m in '
            '(numpy,matplotlib,stim,beyond_blossom)}))')
    with tempfile.TemporaryDirectory(prefix='bb-ae-python-') as cache:
        result = subprocess.run([python, '-c', code], capture_output=True, text=True,
                                env={**os.environ, 'MPLCONFIGDIR': cache})
    return {'executable': python, 'available': result.returncode == 0,
            'modules': json.loads(result.stdout) if result.returncode == 0 else {},
            'error': result.stderr.strip() if result.returncode else None}


def configuration(args):
    source = args.source.resolve()
    if not (source / 'AE/reproduce.py').is_file():
        raise ValueError('--source must identify a Beyond Blossom source checkout')
    work = args.work.resolve()
    if work == source or source in work.parents:
        raise ValueError('Keep --work outside the source checkout')
    hints = dependency_hints([args.reference_build, args.native_build, args.dependencies_from])
    paths = {}
    for name in ['stim_source', 'stim_archive', 'googletest_source', 'upstream_repository', 'crate_cache',
                 'reference_build', 'reference_record', 'native_build', 'external_build']:
        value = getattr(args, name, None) or hints.get(name)
        paths[name] = str(Path(value).resolve()) if value else None
    if not paths['upstream_repository'] and git(source, 'cat-file', '-t', UPSTREAM) == 'commit':
        paths['upstream_repository'] = str(source)
    if not paths['crate_cache']:
        cache = Path(os.environ.get('CARGO_HOME', Path.home() / '.cargo')) / 'registry/cache'
        candidates = sorted(cache.glob('*')) if cache.exists() else []
        if candidates:
            paths['crate_cache'] = str(candidates[0])
    toolchain = args.toolchain
    if not toolchain and shutil.which('rustup'):
        installed = subprocess.run(['rustup', 'toolchain', 'list'], capture_output=True, text=True)
        names = [line.split()[0] for line in installed.stdout.splitlines() if line.strip()]
        toolchain = next((n for n in names if n.startswith('nightly-2026-03-10')), None)
        toolchain = toolchain or next((n for n in names if n.startswith('nightly-')), None)
    hardware = cpu_features()
    external_policy = None
    if args.external_shot_counts:
        overrides = read(args.external_shot_counts)
        if not isinstance(overrides, dict):
            raise ValueError('--external-shot-counts must contain a JSON object')
        external_policy = {method: args.external_shots if args.profile == 'paper' else 32
                           for method in ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf')}
        external_policy.update(overrides)
    return {'schema': 1, 'source': str(source), 'source_identity': identity(source), 'work': str(work),
            'orchestrator': {'path': str(Path(__file__).resolve()), 'sha256': digest(__file__)},
            'profile': args.profile, 'jobs': args.jobs, 'offline': args.offline, 'cpus': select_cpus(args.cpus),
            'backend': args.backend or hardware['backend'], 'hardware': hardware,
            'python': python_environment(args.python), 'toolchain': toolchain, 'dependencies': paths,
            'shots': 1000000 if args.profile == 'paper' else 1025,
            'repeats': 3 if args.profile == 'paper' else 1,
            'external_shots': args.external_shots if args.profile == 'paper' else 32,
            'external_shot_counts': external_policy,
            'pmu_levels': ([] if args.pmu == 'off' else ([2, 3] if hardware['l2_events_supported_model'] else [3])),
            'render': not args.no_render, 'methods': METHODS, 'policy_execution': 'serial',
            'policy_build_modes': {str(k): v for k, v in POLICY_BUILD_MODES.items()},
            'excluded': ['Apple M4 hardware acquisition'], 'archived_data_used_as_measurements': False}


class Workflow:
    def __init__(self, config):
        self.config = config
        self.root = Path(config['work'])
        self.ae = Path(config['source']) / 'AE'
        self.python = config['python']['executable']
        self.state_path = self.root / 'state.json'
        self.state = read(self.state_path) if self.state_path.exists() else {'stages': {}}
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / 'logs').mkdir(exist_ok=True)

    def stage(self, name, category, commands, marker='complete.json', optional_pmu=False, build_allowed=True):
        attempts = self.state['stages'].setdefault(name, [])
        if attempts and attempts[-1]['status'] in ('complete', 'unavailable'):
            last = attempts[-1]
            if last['status'] == 'unavailable':
                return None
            path = Path(last['output']) / marker
            if digest(path) != last['receipt_sha256'] or not read(path).get('complete'):
                raise ValueError(f'Completed stage receipt changed: {name}')
            print(f'{name}: reuse completed stage', flush=True)
            return Path(last['output'])
        if not build_allowed and category in ('builds', 'deps'):
            raise ValueError(f'{name} has not been built; run build or resume first')
        suffix = '' if not attempts else f'-attempt-{len(attempts)+1}'
        output = self.root / category / (name + suffix)
        output.parent.mkdir(parents=True, exist_ok=True)
        if output.exists():
            raise ValueError(f'Untracked stage directory exists: {output}')
        argv_list = commands(output)
        log_path = self.root / 'logs' / f'{name}{suffix}.log'
        record = {'output': str(output), 'commands': argv_list, 'status': 'running',
                  'started_utc': datetime.now(timezone.utc).isoformat(), 'log': str(log_path)}
        attempts.append(record)
        save(self.state_path, self.state)
        print(f'{name}: {log_path}', flush=True)
        try:
            with log_path.open('x') as log:
                for command in argv_list:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True,
                                   env={**os.environ, 'MPLCONFIGDIR': str(self.root / '.matplotlib')})
            receipt = output / marker
            if marker == '.dependency.json':
                save(receipt, {'complete': True, 'commands': argv_list})
            if not read(receipt).get('complete'):
                raise ValueError(f'Stage did not produce a complete receipt: {name}')
            record.update(status='complete', receipt_sha256=digest(receipt))
        except BaseException as error:
            record.update(status='failed', error=str(error))
            details = log_path.read_text(errors='replace')
            if optional_pmu and isinstance(error, subprocess.CalledProcessError):
                for log in output.glob('calls/**/process.log'):
                    details += log.read_text(errors='replace')
                if any(text in details for text in ('perf_event_open cache-read counter', 'PMU group unavailable', 'Invalid cache-read counters')):
                    record.update(status='unavailable', reason='Cache counter capability probe failed; see retained logs')
                    print(f'{name}: PMU unavailable; retained capability failure', flush=True)
                else:
                    raise
            else:
                raise
        finally:
            record['finished_utc'] = datetime.now(timezone.utc).isoformat()
            save(self.state_path, self.state)
        return output if record['status'] == 'complete' else None

    def external_build(self, path, kind, marker):
        path = Path(path)
        marker = Path(marker)
        record = read(marker if marker.is_absolute() else path / marker)
        if not record.get('complete'):
            raise ValueError(f'Existing {kind} build is incomplete: {path}')
        if kind == 'reference':
            if record.get('upstream_revision') != UPSTREAM or str(record.get('cache_level')) != '3':
                raise ValueError('Existing reference must be the fixed official L3 build')
        else:
            built = record.get('source', {})
            if built.get('commit') != self.config['source_identity']['commit'] or built.get('status') or built.get('dirty'):
                raise ValueError(f'Existing {kind} build is from another or dirty source; rebuild without rewriting its receipt')
        if kind == 'candidate':
            options = record.get('configuration', {})
            if (str(options.get('BB_TAILRESET_PREDECODE_MODE')) != '0'
                    or options.get('BB_TAILRESET_SYNDROME_STATS') not in ('OFF', 'FALSE', '0')
                    or str(options.get('BB_AE_CACHE_LEVEL')) != '3'):
                raise ValueError('Existing candidate must be policy 0, statistics off, cache level 3')
        binaries = record.get('binaries', {}) if kind == 'external' else {'native': record['binary']}
        for binary in binaries.values():
            if digest(binary['path']) != binary['sha256']:
                raise ValueError(f'Existing {kind} executable changed')
        return path

    def clone(self, name, allowed):
        if self.config['offline']:
            raise ValueError(f'Missing {name} source in offline mode; supply its local path')
        url, revision = PINS[name]
        return self.stage(name, 'deps', lambda out: [['git', 'clone', '--no-checkout', url, str(out)],
            ['git', '-C', str(out), 'checkout', '--detach', revision]], '.dependency.json', build_allowed=allowed)

    def builds(self, allowed=True):
        c, d = self.config, dict(self.config['dependencies'])
        provided = {}
        for key, kind, marker in [('reference_build', 'reference', d['reference_record'] or 'reference.json'),
                                  ('native_build', 'candidate', 'build.json'), ('external_build', 'external', 'build.json')]:
            if d[key]:
                provided[key] = self.external_build(d[key], kind, marker)
        needed = [('stim', 'stim_source')]
        if not d['reference_build'] or 2 in c['pmu_levels']:
            needed += [('googletest', 'googletest_source'), ('upstream', 'upstream_repository')]
        for name, key in needed:
            if not d[key]:
                d[key] = str(self.clone(name, allowed))
        if not d['stim_archive']:
            out = self.stage('stim-library', 'deps', lambda p: [
                ['cmake', '-S', d['stim_source'], '-B', str(p), '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF'],
                ['cmake', '--build', str(p), '--target', 'libstim', '-j', str(c['jobs'])]], '.dependency.json', build_allowed=allowed)
            d['stim_archive'] = str(out / 'out/libstim.a')
        common = ['--stim-source', d['stim_source'], '--stim-archive', d['stim_archive'], '--jobs', str(c['jobs'])]
        references = {}
        for level in sorted(set([3] + c['pmu_levels'])):
            if level == 3 and d['reference_build']:
                references[level] = provided['reference_build']
            else:
                references[level] = self.stage(f'reference-l{level}', 'builds', lambda out, level=level: [[self.python,
                    str(self.ae / 'build_reference.py'), '--output', str(out), '--upstream-repository', d['upstream_repository'],
                    '--history-checkout', c['source'], '--history-ref', 'source-2411603', '--googletest-source', d['googletest_source'],
                    '--cache-level', str(level), *common]], 'reference.json', build_allowed=allowed)
        native = {}
        variants = [(0, False, 3), (1, False, 3), (2, False, 3), (0, True, 3), (1, True, 3), (2, True, 3)]
        if 2 in c['pmu_levels']:
            variants.append((0, False, 2))
        for policy, statistics, level in variants:
            key = f'policy-{policy}-' + ('statistics' if statistics else f'l{level}')
            if (policy, statistics, level) == (0, False, 3) and d['native_build']:
                native[key] = provided['native_build']
            else:
                native[key] = self.stage(key, 'builds', lambda out, policy=policy, statistics=statistics, level=level: [[self.python,
                    str(self.ae / 'build_native.py'), '--output', str(out), '--profile', c['profile'], '--predecode-mode', str(POLICY_BUILD_MODES[policy]),
                    '--cache-level', str(level), *(['--statistics'] if statistics else []), *common]], 'build.json', build_allowed=allowed)
        if d['external_build']:
            external = provided['external_build']
        else:
            external = self.stage('external', 'builds', lambda out: [[self.python, str(self.ae / 'external/build.py'),
                '--output', str(out), '--reference-build', str(references[3]), '--toolchain', c['toolchain'], *common,
                *(['--reference-record', d['reference_record']] if d['reference_record'] else []),
                *(['--crate-cache', d['crate_cache']] if d['crate_cache'] else (['--offline'] if c['offline'] else []))]],
                'build.json', build_allowed=allowed)
        return native, references, external

    def collect(self, native, references, external):
        c = self.config
        placement = ['--cpus', ','.join(map(str, c['cpus'])), '--backend', c['backend']]
        def reference_args(level=3):
            root = references[level]
            record = c['dependencies']['reference_record'] if level == 3 else None
            return ['--reference-binary', str(root / 'build/bin/experiment_benchmark'),
                    '--reference-record', record or str(root / 'reference.json')]
        def native_command(out, build, methods, measurement='batch', source_run=None, rate=None, profile=None, level=3, distance=None):
            return [self.python, str(self.ae / 'reproduce.py'), 'run', '--profile', profile or c['profile'],
                '--binary', str(build / 'bin/bb_benchmark'), '--build-record', str(build / 'build.json'),
                *reference_args(level), '--methods', methods, '--measurement', measurement, *placement,
                *(['--input-run', str(source_run), '--link-inputs'] if source_run else []), *(['--error-rates', str(rate)] if rate else []),
                *(['--distances', str(distance)] if distance else []),
                *(['--repeats', '1'] if measurement in ('statistics', 'pmu') else []), '--output', str(out)]
        batch = self.stage('native-batch', 'runs', lambda out: [native_command(out, native['policy-0-l3'], METHODS)])
        external_selection = ['--limit', str(c['external_shots'])]
        if c.get('external_shot_counts'):
            policy_file = self.root / 'external-shot-counts.json'
            if policy_file.exists() and read(policy_file) != c['external_shot_counts']:
                raise ValueError('External shot policy changed after configuration')
            if not policy_file.exists():
                save(policy_file, c['external_shot_counts'])
            external_selection = ['--shot-counts', str(policy_file)]
        external_run = self.stage('external', 'runs', lambda out: [[self.python, str(self.ae / 'reproduce.py'), 'external',
            '--profile', c['profile'], '--ae-run', str(batch), '--build', str(external), '--output', str(out),
            '--cpus', ','.join(map(str, c['cpus'][:4])), *external_selection]])
        timings, counts = {}, {}
        for policy in range(3):
            timings[policy] = self.stage(f'policy-{policy}-timing', 'runs', lambda out, policy=policy: [
                native_command(out, native[f'policy-{policy}-l3'], 'predecoded', source_run=batch, rate='.003')])
            counts[policy] = self.stage(f'policy-{policy}-counts', 'runs', lambda out, policy=policy: [
                native_command(out, native[f'policy-{policy}-statistics'], 'predecoded', 'statistics', batch, '.003')])
        pmu = {}
        for level in c['pmu_levels']:
            probe = self.stage(f'pmu-l{level}-probe', 'capabilities', lambda out, level=level: [
                native_command(out, native[f'policy-0-l{level}'], 'reference,structured', 'pmu', profile='smoke', level=level)], optional_pmu=True)
            if probe:
                pmu[level] = self.stage(f'pmu-l{level}', 'runs', lambda out, level=level: [
                    native_command(out, native[f'policy-0-l{level}'], 'reference,structured', 'pmu', batch,
                                   rate='.003', level=level, distance=33 if c['profile']=='paper' else 9)])
        groups = ['ablation', 'comparison', 'predecoder', 'xeon-macros']
        export = self.stage('figures', 'exports', lambda out: [[self.python, str(self.ae / 'export_results.py'),
            '--profile', c['profile'], '--native-run', str(batch), '--external-run', str(external_run),
            '--policy-timing', *[f'{p}={timings[p]}' for p in range(3)],
            '--policy-counts', *[f'{p}={counts[p]}' for p in range(3)],
            *(['--l2-run', str(pmu[2])] if 2 in pmu else []), *(['--l3-run', str(pmu[3])] if 3 in pmu else []),
            '--groups', *groups, *(['--no-render'] if not c['render'] else []), '--output', str(out)]])
        save(self.root / 'complete.json', {'complete': True, 'scope': 'Fresh local Linux paper workflow',
            'profile': c['profile'], 'native_shots_per_case': c['shots'], 'external_shots_per_case': c['external_shots'],
            'external_shot_counts': c.get('external_shot_counts'),
            'pmu_levels_collected': sorted(pmu), 'pmu_levels_unavailable': sorted(set(c['pmu_levels']) - set(pmu)),
            'excluded': c['excluded'], 'export': str(export), 'config_sha256': digest(self.root / 'config.json')})
        print(f'Complete: {export}', flush=True)


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', nargs='?', choices=['run', 'plan', 'build', 'collect', 'resume'], default='run')
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--source', type=Path, default=HERE.parent)
    p.add_argument('--profile', choices=['paper', 'smoke'], default='paper')
    p.add_argument('--external-shots', type=int, choices=[10000, 1000000], default=10000)
    p.add_argument('--external-shot-counts', type=Path, help='Optional per-method/case overrides; unspecified external methods retain --external-shots')
    p.add_argument('--python', default=sys.executable)
    p.add_argument('--cpus'); p.add_argument('--backend', choices=['scalar', 'avx2', 'avx512vl', 'avx512'])
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--pmu', choices=['auto', 'off'], default='auto')
    p.add_argument('--offline', action='store_true'); p.add_argument('--no-render', action='store_true')
    p.add_argument('--toolchain', help='Installed Rust nightly; automatically detected when omitted')
    for name in ['stim-source', 'stim-archive', 'googletest-source', 'upstream-repository', 'crate-cache',
                 'reference-build', 'reference-record', 'native-build', 'external-build', 'dependencies-from']:
        p.add_argument('--' + name, type=Path)
    return p


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parser().parse_args(argv)
    try:
        if args.jobs < 1:
            raise ValueError('--jobs must be positive')
        config_path = args.work.resolve() / 'config.json'
        if args.action in ('resume', 'collect'):
            if any(a.startswith('--') and a.split('=')[0] != '--work' for a in argv):
                raise ValueError('resume/collect use the saved configuration; supply only --work')
            config = read(config_path)
        else:
            config = configuration(args)
        if args.action == 'plan':
            print(json.dumps(config, indent=2))
            return 0
        current = identity(config['source'])
        if current != config['source_identity']:
            raise ValueError('Source changed since this workflow was configured; use a new work directory')
        if config['profile'] == 'paper' and (not current['commit'] or current['status']):
            raise ValueError('Paper acquisition requires a clean committed --source checkout')
        if config.get('orchestrator', {}).get('sha256') != digest(__file__):
            raise ValueError('Workflow entry changed; resume with the original fixed source entry')
        environment = python_environment(config['python']['executable'])
        if not environment['available']:
            raise ValueError('Install beyond_blossom, stim, numpy and matplotlib in --python before starting:\n' + environment['error'])
        if not config['dependencies']['external_build'] and not config['toolchain']:
            raise ValueError('Install Rust nightly once: rustup toolchain install nightly-2026-03-10 --profile minimal')
        config_path.parent.mkdir(parents=True, exist_ok=True)
        with (config_path.parent / '.workflow.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('This workflow is already running; wait before resuming it')
            if args.action not in ('resume', 'collect'):
                if config_path.exists():
                    raise ValueError('Workflow already exists; use resume or collect with the same --work')
                save(config_path, config)
            workflow = Workflow(config)
            builds = workflow.builds(allowed=args.action != 'collect')
            if args.action != 'build':
                workflow.collect(*builds)
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
