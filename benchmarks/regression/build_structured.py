#!/usr/bin/env python3
"""Link source and migrated structured timing adapters to existing release libraries."""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_flags(build, library):
    path = build / f'CMakeFiles/lib{library}.dir/flags.make'
    definitions = {}
    for line in path.read_text().splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            definitions[key] = shlex.split(value)
    flags = sum((definitions.get(key, []) for key in ['CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS']), [])
    if any('flto' in flag for flag in flags):
        raise RuntimeError('Structured migration comparison requires LTO disabled')
    for flag in definitions.get('CXX_DEFINES', []):
        key, _, value = flag.removeprefix('-D').partition('=')
        instrumented = any(token in key for token in ['PROFILE_COUNTERS', 'PROFILE_TIMERS', 'PROFILE_TIMELINE',
                                                       'ENABLE_PROFILE', 'TAILRESET_SYNDROME_STATS', 'PREDECODE_DIAGNOSTICS'])
        if instrumented and value != '0':
            raise RuntimeError(f'Pure timing requires an uninstrumented library: {flag}')
    compiler = None
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if line.startswith('CMAKE_CXX_COMPILER:FILEPATH='):
            compiler = line.split('=', 1)[1]
    if compiler is None:
        raise RuntimeError('CMake C++ compiler identity is missing')
    return compiler, flags, path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-snapshot', type=pathlib.Path, required=True)
parser.add_argument('--candidate-source', type=pathlib.Path, default=pathlib.Path.cwd())
parser.add_argument('--source-build', type=pathlib.Path, required=True)
parser.add_argument('--candidate-build', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
records = []
for name, root, build, library, prefix, relative_adapter in [
    ('source', args.source_snapshot, args.source_build, 'pymatching', 'PM', 'AE/pmu_native/benchmark.cc'),
    ('candidate', args.candidate_source, args.candidate_build, 'beyond_blossom', 'BB', 'AE/native/benchmark.cc'),
]:
    root, build = root.resolve(), build.resolve()
    source = root / relative_adapter
    if not source.is_file():
        raise RuntimeError(f'Missing original adapter: {source}')
    compiler, flags, flags_path = make_flags(build, library)
    extra = [f'-D{prefix}_EXPERIMENT_SHARED=1', f'-D{prefix}_EXPERIMENT_PIPELINE=1',
             f'-D{prefix}_EXPERIMENT_OPENMP=0', f'-D{prefix}_AE_TAILRESET=1',
             f'-D{prefix}_UNIFIED_CONTROL=0', f'-D{prefix}_PREDECODE_DIAGNOSTICS=0',
             f'-D{prefix}_PMU_CACHE_LEVEL=2']
    archive = build / f'lib{library}.a'
    stim = build / '_deps/stim-build/out/libstim.a'
    binary = (args.output / name).resolve()
    command = [compiler, *flags, *extra, '-fno-lto', '-pthread', str(source), str(archive), str(stim), '-o', str(binary)]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    adapter_headers = sorted(source.parent.glob('*.h'))
    tailreset_statistics = source.parent.parent / 'tailreset_statistics.h'
    if tailreset_statistics.is_file():
        adapter_headers.append(tailreset_statistics)
    records.append({'variant': name, 'compiler': compiler, 'command': command, 'lto': False,
                    'flags_make': str(flags_path), 'flags_make_sha256': digest(flags_path),
                    'adapter': str(source), 'adapter_sha256': digest(source),
                    'library': str(archive), 'library_sha256': digest(archive),
                    'stim_library_sha256': digest(stim), 'binary': str(binary), 'binary_sha256': digest(binary),
                    'adapter_headers': {str(path.relative_to(root)): digest(path)
                                        for path in adapter_headers}})
    (args.output / 'build.json').write_text(json.dumps({'kind': 'pure_timing', 'builds': records}, indent=2) + '\n')
