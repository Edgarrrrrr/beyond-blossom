#!/usr/bin/env python3
"""Compile one benchmark against each completed CMake library, without LTO."""
import pathlib
import shlex
import subprocess
import sys

baseline_dir, candidate_dir, source, out_dir = map(pathlib.Path, sys.argv[1:])
out_dir.mkdir(parents=True, exist_ok=True)
for variant, build, library in [('baseline', baseline_dir, 'pymatching'), ('candidate', candidate_dir, 'beyond_blossom')]:
    definitions = {}
    for line in (build / f'CMakeFiles/lib{library}.dir/flags.make').read_text().splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            definitions[key] = shlex.split(value)
    flags = sum((definitions.get(key, []) for key in ['CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS']), [])
    assert not any('flto' in flag for flag in flags), 'LTO would invalidate this comparison'
    if variant == 'baseline':
        flags.append('-DBEYOND_BENCHMARK_BASELINE=1')
    command = ['c++', *flags, '-fno-lto', '-pthread', str(source), str(build / f'lib{library}.a'),
               str(build / '_deps/stim-build/out/libstim.a'), '-o', str(out_dir / variant)]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True)
