#!/usr/bin/env python3
"""Run the local paper workflow: discover dependencies, build, collect serially, export.

Examples:
  python AE/run.py run --work ../ae-work
  python AE/run.py resume --work ../ae-work
  python AE/run.py plan --work ../ae-work --offline --reference-build /path/to/reference

Archived figure regeneration remains AE/replot.py. This entry never supplies M4 data.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

try:
    from .external.common import resolve_shot_counts, classify_shot_counts
except ImportError:
    from external.common import resolve_shot_counts, classify_shot_counts

HERE = Path(__file__).resolve().parent
UPSTREAM = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
PINNED_TOOLCHAIN = 'nightly-2026-03-10'
DISTANCES = [9, 13, 17, 21, 25, 29, 33]
ERROR_RATES = [0.0005, 0.001, 0.003]

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
        receipt = root / 'reference.json'
        if receipt.exists():
            for name in ('stim_source', 'stim_archive', 'googletest_source'):
                value = read(receipt).get(name)
                if value and Path(value).exists():
                    result.setdefault(name, str(Path(value).resolve()))
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
    if platform.system() != 'Linux':
        raise ValueError('AE/run.py automates Linux acquisition; use the M4 procedure in AE/REFERENCE.md on macOS')
    source = args.source.resolve()
    if not (source / 'AE/reproduce.py').is_file():
        raise ValueError('--source must identify a Beyond Blossom source checkout')
    work = args.work.resolve()
    if work == source or source in work.parents:
        raise ValueError('Keep --work outside the source checkout')
    hints = dependency_hints([args.reference_build, args.native_build, args.dependencies_from])
    paths = {}
    for name in ['stim_source', 'stim_archive', 'googletest_source', 'upstream_repository', 'crate_cache',
                 'reference_build', 'reference_record', 'native_build', 'external_build', 'official_source', 'reference_adapter']:
        value = getattr(args, name, None) or hints.get(name)
        paths[name] = str(Path(value).resolve()) if value else None
    toolchain = args.toolchain or PINNED_TOOLCHAIN
    hardware = cpu_features()
    backend = args.backend or hardware['backend']
    ranks = {'scalar': 0, 'avx2': 1, 'avx512vl': 2, 'avx512': 2}
    if args.action != 'build' and ranks[backend] > ranks[hardware['backend']]:
        raise ValueError('Requested SIMD backend is not supported by this measurement host')
    distances = [int(v) for v in args.distances.split(',')] if args.distances else (DISTANCES if args.profile == 'paper' else [9])
    rates = [float(v) for v in args.error_rates.split(',')] if args.error_rates else (ERROR_RATES if args.profile == 'paper' else [.003])
    if not distances or len(set(distances)) != len(distances) or any(d < 3 or d % 2 == 0 for d in distances):
        raise ValueError('--distances must select distinct odd distances >= 3')
    if not rates or len(set(rates)) != len(rates) or any(not 0 < p < .5 for p in rates):
        raise ValueError('--error-rates must select distinct rates between 0 and .5')
    if args.profile == 'paper' and not args.no_export and args.action != 'build' and (set(distances) != set(DISTANCES) or set(rates) != set(ERROR_RATES)):
        raise ValueError('Partial paper selections require --no-export; combine their bundles with export_results.py --shards')
    external_policy = resolve_shot_counts(args.profile, [(d, p) for d in distances for p in rates],
        args.external_shots if args.profile == 'paper' else 32,
        read(args.external_shot_counts) if args.external_shot_counts else None)
    return {'schema': 1, 'source': str(source), 'source_identity': identity(source), 'work': str(work),
            'orchestrator': {'path': str(Path(__file__).resolve()), 'sha256': digest(__file__)},
            'profile': args.profile, 'jobs': args.jobs, 'offline': args.offline, 'cpus': [] if args.action == 'build' else select_cpus(args.cpus),
            'distances': distances, 'error_rates': rates, 'export': not args.no_export,
            'builds_manifest': str(args.builds_manifest.resolve()) if args.builds_manifest else None,
            'placement': {'host': platform.node(), 'affinity': sorted(os.sched_getaffinity(0)),
                          'slurm': {k: os.environ[k] for k in ('SLURM_JOB_ID', 'SLURMD_NODENAME', 'SLURM_JOB_NODELIST', 'SLURM_CPUS_PER_TASK') if k in os.environ}},
            'backend': backend, 'hardware': hardware,
            'python': python_environment(args.python), 'toolchain': toolchain, 'dependencies': paths,
            'shots': 1000000 if args.profile == 'paper' else 1025,
            'repeats': 3 if args.profile == 'paper' else 1,
            'external_shots': args.external_shots if args.profile == 'paper' else 32,
            'external_shot_counts': external_policy,
            'pmu_levels': ([] if args.pmu == 'off' else ([2, 3] if args.action == 'build' or hardware['l2_events_supported_model'] else [3])),
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

    def verify_dependency(self, name, output):
        if name == 'vendor':
            spec = importlib.util.spec_from_file_location('bb_ae_fixed_vendor', self.ae / 'vendor.py')
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            receipt = read(output / 'dependencies.json')
            if receipt['manifest_sha256'] != digest(self.ae.parent / 'vendor/manifest.json'):
                raise ValueError('Vendored dependency manifest changed')
            manifest = module.load_manifest()
            for component, key in module.KEYS.items():
                if key in receipt:
                    module.verify_tree(receipt[key], manifest['components'][component])
        elif name == 'stim-library':
            receipt = read(output / '.dependency.json')
            if not receipt.get('source_files') or not receipt.get('artifacts'):
                raise ValueError('Stim dependency receipt lacks source/library identities')
            source = Path(receipt['source_root'])
            files = {str(source / name) for name in ('CMakeLists.txt', 'LICENSE') if (source / name).is_file()}
            files |= {str(p) for folder in ('src', 'file_lists') for p in (source / folder).rglob('*') if p.is_file()}
            if set(receipt['source_files']) != files:
                raise ValueError('Stim source file set changed')
            for path, sha in {**receipt['source_files'], **receipt['artifacts']}.items():
                if digest(path) != sha:
                    raise ValueError(f'Stim dependency changed: {path}')

    def stage(self, name, category, commands, marker='complete.json', optional_pmu=False, build_allowed=True):
        attempts = self.state['stages'].setdefault(name, [])
        if attempts and attempts[-1]['status'] in ('complete', 'unavailable'):
            last = attempts[-1]
            if last['status'] == 'unavailable':
                return None
            path = Path(last['output']) / marker
            if digest(path) != last['receipt_sha256'] or not read(path).get('complete'):
                raise ValueError(f'Completed stage receipt changed: {name}')
            if category == 'deps':
                self.verify_dependency(name, Path(last['output']))
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
        dependency_sources = {}
        if name == 'stim-library':
            source = Path(argv_list[0][argv_list[0].index('-S') + 1])
            files = [source / name for name in ('CMakeLists.txt', 'LICENSE') if (source / name).is_file()]
            files += sorted(p for folder in ('src', 'file_lists') for p in (source / folder).rglob('*') if p.is_file())
            dependency_sources = {str(p): digest(p) for p in files}
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
                save(receipt, {'complete': True, 'commands': argv_list, 'source_files': dependency_sources, 'source_root': str(source) if name == 'stim-library' else None,
                    'artifacts': {str(output / 'out/libstim.a'): digest(output / 'out/libstim.a')} if name == 'stim-library' else {}})
            if category == 'deps':
                self.verify_dependency(name, output)
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

    def external_build(self, path, kind, marker, variant=None, cache_level=3):
        path = Path(path)
        marker = Path(marker)
        record = read(marker if marker.is_absolute() else path / marker)
        if not record.get('complete'):
            raise ValueError(f'Existing {kind} build is incomplete: {path}')
        if kind == 'reference':
            if record.get('upstream_revision') != UPSTREAM or str(record.get('cache_level')) != str(cache_level):
                raise ValueError('Existing reference must be the fixed official build at the requested cache level')
        else:
            built = record.get('source', {})
            if built.get('commit') != self.config['source_identity']['commit'] or built.get('status') or built.get('dirty'):
                raise ValueError(f'Existing {kind} build is from another or dirty source; rebuild without rewriting its receipt')
        if kind == 'candidate':
            options = record.get('configuration', {})
            policy, statistics, level = variant or (0, False, 3)
            stats = str(options.get('BB_TAILRESET_SYNDROME_STATS')).upper()
            if (str(options.get('BB_TAILRESET_PREDECODE_MODE')) != str(POLICY_BUILD_MODES[policy])
                    or stats not in (('ON', 'TRUE', '1') if statistics else ('OFF', 'FALSE', '0'))
                    or str(options.get('BB_AE_CACHE_LEVEL')) != str(level)):
                raise ValueError('Existing candidate configuration does not match the requested variant')
        binaries = record.get('binaries', {}) if kind == 'external' else {'native': record['binary']}
        for binary in binaries.values():
            if digest(binary['path']) != binary['sha256']:
                raise ValueError(f'Existing {kind} executable changed')
        return path

    def shared_builds(self, filename):
        manifest = read(filename)
        if not manifest.get('complete') or manifest.get('source_identity') != self.config['source_identity']:
            raise ValueError('Shared builds do not match this clean source checkout')
        if manifest.get('machine') != platform.machine():
            raise ValueError('Shared binaries target another machine architecture')
        expected_receipts = {str(Path(v) / 'build.json') for v in manifest['native'].values()}
        expected_receipts.add(str(Path(manifest['external']) / 'build.json'))
        expected_receipts.update(manifest['reference_records'].values())
        if set(manifest['receipts']) != expected_receipts:
            raise ValueError('Shared manifest must bind every build receipt exactly once')
        for path, sha in manifest['receipts'].items():
            if digest(path) != sha:
                raise ValueError(f'Shared build receipt changed: {path}')
        native = {k: Path(v) for k, v in manifest['native'].items()}
        references = {int(k): Path(v) for k, v in manifest['references'].items()}
        self.reference_records = {int(k): v for k, v in manifest['reference_records'].items()}
        required = [(p, stats, 3) for p in range(3) for stats in (False, True)]
        required += [(0, False, level) for level in self.config['pmu_levels'] if level != 3]
        for p, stats, level in required:
            key = f'policy-{p}-' + ('statistics' if stats else f'l{level}')
            self.external_build(native[key], 'candidate', 'build.json', (p, stats, level))
        for level in set([3] + self.config['pmu_levels']):
            self.external_build(references[level], 'reference', self.reference_records[level], cache_level=level)
        external = self.external_build(manifest['external'], 'external', 'build.json')
        return native, references, external

    def record_builds(self, native, references, external, reference_records):
        receipts = [p / 'build.json' for p in native.values()] + [external / 'build.json']
        receipts += [Path(p) for p in reference_records.values()]
        save(self.root / 'builds.json', {'schema': 1, 'complete': True, 'machine': platform.machine(),
            'source_identity': self.config['source_identity'], 'native': {k: str(v) for k, v in native.items()},
            'references': {str(k): str(v) for k, v in references.items()},
            'reference_records': {str(k): str(v) for k, v in reference_records.items()},
            'external': str(external), 'receipts': {str(p): digest(p) for p in receipts}})

    def builds(self, allowed=True):
        c, d = self.config, dict(self.config['dependencies'])
        if c.get('builds_manifest'):
            return self.shared_builds(c['builds_manifest'])
        provided = {}
        for key, kind, marker in [('reference_build', 'reference', d['reference_record'] or 'reference.json'),
                                  ('native_build', 'candidate', 'build.json'), ('external_build', 'external', 'build.json')]:
            if d[key]:
                provided[key] = self.external_build(d[key], kind, marker)
        needed = ['stim_source'] + ([] if d['external_build'] else ['crate_cache'])
        if not d['reference_build'] or 2 in c['pmu_levels']:
            needed += ['googletest_source', 'reference_adapter']
            if not d.get('upstream_repository'):
                needed.append('official_source')
        if any(not d.get(k) for k in needed):
            vendored = self.stage('vendor', 'deps', lambda out: [[self.python,
                str(self.ae / 'vendor.py'), '--output', str(out)]], 'dependencies.json', build_allowed=allowed)
            values = read(vendored / 'dependencies.json')
            for key in ('stim_source', 'googletest_source', 'official_source', 'reference_adapter', 'crate_cache'):
                d[key] = d.get(key) or values[key]
        if not d['stim_archive']:
            out = self.stage('stim-library', 'deps', lambda p: [
                ['cmake', '-S', d['stim_source'], '-B', str(p), '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF', '-DSIMD_WIDTH=128'],
                ['cmake', '--build', str(p), '--target', 'libstim', '-j', str(c['jobs'])]], '.dependency.json', build_allowed=allowed)
            d['stim_archive'] = str(out / 'out/libstim.a')
        common = ['--stim-source', d['stim_source'], '--stim-archive', d['stim_archive'], '--jobs', str(c['jobs'])]
        references = {}
        for level in sorted(set([3] + c['pmu_levels'])):
            if level == 3 and d['reference_build']:
                references[level] = provided['reference_build']
            else:
                references[level] = self.stage(f'reference-l{level}', 'builds', lambda out, level=level: [[self.python,
                    str(self.ae / 'build_reference.py'), '--output', str(out),
                    *(['--upstream-repository', d['upstream_repository']] if d.get('upstream_repository') else ['--upstream-source', d['official_source']]),
                    '--reference-adapter', d['reference_adapter'], '--googletest-source', d['googletest_source'],
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
                    '--cache-level', str(level), *(['--statistics'] if statistics else []), *(['--offline'] if c['offline'] else []), *common]], 'build.json', build_allowed=allowed)
        if d['external_build']:
            external = provided['external_build']
        else:
            external = self.stage('external', 'builds', lambda out: [[self.python, str(self.ae / 'external/build.py'),
                '--output', str(out), '--reference-build', str(references[3]), '--toolchain', c['toolchain'], *common,
                *(['--reference-record', d['reference_record']] if d['reference_record'] else []),
                *(['--crate-cache', d['crate_cache']] if d['crate_cache'] else []), *(['--offline'] if c['offline'] else [])]],
                'build.json', build_allowed=allowed)
        self.reference_records = {level: (d['reference_record'] if level == 3 and d['reference_record'] else str(path / 'reference.json')) for level, path in references.items()}
        self.record_builds(native, references, external, self.reference_records)
        return native, references, external

    def collect(self, native, references, external):
        c = self.config
        placement = ['--cpus', ','.join(map(str, c['cpus'])), '--backend', c['backend']]
        def reference_args(level=3):
            root = references[level]
            record = getattr(self, 'reference_records', {}).get(level) or (c['dependencies']['reference_record'] if level == 3 else None)
            return ['--reference-binary', str(root / 'build/bin/experiment_benchmark'),
                    '--reference-record', record or str(root / 'reference.json')]
        def native_command(out, build, methods, measurement='batch', source_run=None, rate=None, profile=None, level=3, distance=None):
            return [self.python, str(self.ae / 'reproduce.py'), 'run', '--profile', profile or c['profile'],
                '--binary', str(build / 'bin/bb_benchmark'), '--build-record', str(build / 'build.json'),
                *reference_args(level), '--methods', methods, '--measurement', measurement, *placement,
                *(['--input-run', str(source_run), '--link-inputs'] if source_run else []),
                '--error-rates', str(rate) if rate else ','.join(map(str, c.get('error_rates', ERROR_RATES if c['profile']=='paper' else [.003]))),
                '--distances', str(distance) if distance else ','.join(map(str, c.get('distances', DISTANCES if c['profile']=='paper' else [9]))),
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
        for policy in (range(3) if .003 in c.get('error_rates', [.003]) else []):
            timings[policy] = self.stage(f'policy-{policy}-timing', 'runs', lambda out, policy=policy: [
                native_command(out, native[f'policy-{policy}-l3'], 'predecoded', source_run=batch, rate='.003')])
            counts[policy] = self.stage(f'policy-{policy}-counts', 'runs', lambda out, policy=policy: [
                native_command(out, native[f'policy-{policy}-statistics'], 'predecoded', 'statistics', batch, '.003')])
        pmu = {}
        pmu_target = (33 if c['profile']=='paper' else 9) in c.get('distances', DISTANCES) and .003 in c.get('error_rates', [.003])
        for level in (c['pmu_levels'] if pmu_target else []):
            probe = self.stage(f'pmu-l{level}-probe', 'capabilities', lambda out, level=level: [
                native_command(out, native[f'policy-0-l{level}'], 'reference,structured', 'pmu', profile='smoke', level=level, rate='.003', distance=9)], optional_pmu=True)
            if probe:
                pmu[level] = self.stage(f'pmu-l{level}', 'runs', lambda out, level=level: [
                    native_command(out, native[f'policy-0-l{level}'], 'reference,structured', 'pmu', batch,
                                   rate='.003', level=level, distance=33 if c['profile']=='paper' else 9)])
        bundle = {'native_run': str(batch), 'external_run': str(external_run),
                  'policy_timing': {str(p): str(v) for p, v in timings.items()},
                  'policy_counts': {str(p): str(v) for p, v in counts.items()},
                  **{f'l{level}_run': str(path) for level, path in pmu.items()}}
        export = None
        if c.get('export', True):
            groups = ['ablation', 'comparison', 'xeon-macros'] + (['predecoder'] if timings else [])
            export = self.stage('figures', 'exports', lambda out: [[self.python, str(self.ae / 'export_results.py'),
                '--profile', c['profile'], '--native-run', str(batch), '--external-run', str(external_run),
                *(['--policy-timing', *[f'{p}={timings[p]}' for p in range(3)],
                   '--policy-counts', *[f'{p}={counts[p]}' for p in range(3)]] if timings else []),
                *(['--l2-run', str(pmu[2])] if 2 in pmu else []), *(['--l3-run', str(pmu[3])] if 3 in pmu else []),
                '--groups', *groups, *(['--no-render'] if not c['render'] else []), '--output', str(out)]])
        save(self.root / 'complete.json', {'complete': True, 'scope': 'Fresh local Linux paper workflow',
            'profile': c['profile'], 'native_shots_per_case': c['shots'], 'external_shots_per_case': c['external_shots'],
            'external_shot_counts': c.get('external_shot_counts'), 'bundle': bundle, 'placement': c.get('placement'),
            'pmu_levels_collected': sorted(pmu),
            'pmu_levels_unavailable': sorted(set(c['pmu_levels']) - set(pmu)) if pmu_target else [],
            'pmu_applicable': pmu_target, 'excluded': c['excluded'], 'export': str(export) if export else None,
            'config_sha256': digest(self.root / 'config.json')})
        print(f'Complete: {export or self.root}', flush=True)



def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', nargs='?', choices=['run', 'plan', 'build', 'build-resume', 'collect', 'resume'], default='run')
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--source', type=Path, default=HERE.parent)
    p.add_argument('--profile', choices=['paper', 'smoke'], default='paper')
    p.add_argument('--external-shots', type=int, choices=[10000, 1000000],
                   help='Explicit uniform override; default uses the manuscript per-case sample counts')
    p.add_argument('--external-shot-counts', type=Path, help='Per-method/case overrides; unspecified points use manuscript counts or explicit --external-shots')
    p.add_argument('--python', default=sys.executable)
    p.add_argument('--cpus', help='Eight physical cores on one socket, including smoke; auto-discovered when omitted'); p.add_argument('--backend', choices=['scalar', 'avx2', 'avx512vl', 'avx512'])
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--distances', help='Comma-separated case distances')
    p.add_argument('--error-rates', help='Comma-separated case error rates')
    p.add_argument('--builds-manifest', type=Path, help='Reuse every shared build without compiling on this host')
    p.add_argument('--no-export', action='store_true', help='Save a case bundle for later multi-host aggregation')
    p.add_argument('--pmu', choices=['auto', 'off'], default='auto')
    p.add_argument('--offline', action='store_true'); p.add_argument('--no-render', action='store_true')
    p.add_argument('--toolchain', help='Installed Rust toolchain (default: fixed nightly-2026-03-10)')
    for name in ['stim-source', 'stim-archive', 'googletest-source', 'upstream-repository', 'crate-cache',
                 'reference-build', 'reference-record', 'native-build', 'external-build', 'dependencies-from', 'official-source', 'reference-adapter']:
        p.add_argument('--' + name, type=Path)
    return p


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parser().parse_args(argv)
    try:
        if args.jobs < 1:
            raise ValueError('--jobs must be positive')
        config_path = args.work.resolve() / 'config.json'
        if args.action in ('resume', 'collect', 'build-resume'):
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
        if not config.get('builds_manifest') and not config['dependencies']['external_build'] and not config['toolchain']:
            raise ValueError('Install Rust nightly once: rustup toolchain install nightly-2026-03-10 --profile minimal')
        config_path.parent.mkdir(parents=True, exist_ok=True)
        with (config_path.parent / '.workflow.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('This workflow is already running; wait before resuming it')
            if args.action not in ('resume', 'collect', 'build-resume'):
                if config_path.exists():
                    raise ValueError('Workflow already exists; use resume or collect with the same --work')
                save(config_path, config)
            workflow = Workflow(config)
            builds = workflow.builds(allowed=args.action != 'collect')
            if args.action not in ('build', 'build-resume'):
                if not config['cpus']:
                    raise ValueError('This was a build-only workflow; use a fresh run with --builds-manifest ' + str(workflow.root / 'builds.json'))
                workflow.collect(*builds)
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
