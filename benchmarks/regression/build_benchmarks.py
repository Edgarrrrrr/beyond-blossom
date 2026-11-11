#!/usr/bin/env python3
"""Compile one benchmark against each completed CMake library, without LTO."""
import pathlib
import hashlib
import json
import shlex
import subprocess
import sys

baseline_dir, candidate_dir, source, out_dir = map(pathlib.Path, sys.argv[1:])
out_dir.mkdir(parents=True, exist_ok=True)
records = []
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

for variant, build, library in [('baseline', baseline_dir, 'pymatching'), ('candidate', candidate_dir, 'beyond_blossom')]:
    definitions = {}
    flags_path = build / f'CMakeFiles/lib{library}.dir/flags.make'
    for line in flags_path.read_text().splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            definitions[key] = shlex.split(value)
    flags = sum((definitions.get(key, []) for key in ['CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS']), [])
    assert not any('flto' in flag for flag in flags), 'LTO would invalidate this comparison'
    if variant == 'baseline':
        flags.append('-DBEYOND_BENCHMARK_BASELINE=1')
    compiler = next(line.split('=', 1)[1] for line in (build / 'CMakeCache.txt').read_text().splitlines()
                    if line.startswith('CMAKE_CXX_COMPILER:FILEPATH='))
    archive, stim = build / f'lib{library}.a', build / '_deps/stim-build/out/libstim.a'
    binary = out_dir / variant
    command = [compiler, *flags, '-fno-lto', '-pthread', str(source), str(archive),
               str(stim), '-o', str(binary)]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    records.append({'variant': variant, 'compiler': compiler, 'command': command, 'lto': False,
                    'flags_make': str(flags_path), 'flags_make_sha256': digest(flags_path),
                    'source': str(source), 'source_sha256': digest(source),
                    'library': str(archive), 'library_sha256': digest(archive),
                    'stim_library_sha256': digest(stim), 'binary': str(binary), 'binary_sha256': digest(binary)})
    (out_dir / 'build.json').write_text(json.dumps({'kind': 'pure_timing', 'builds': records}, indent=2) + '\n')
