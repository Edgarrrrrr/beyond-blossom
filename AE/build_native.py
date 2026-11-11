#!/usr/bin/env python3
"""Build the AE native candidate and seal its source, dependencies and executable."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys

from reproduce import HERE, digest, source_identity, write


def file_identity(root, paths):
    return {str(path.relative_to(root)): digest(path) for path in sorted(paths) if path.is_file()}


def source_files():
    repo = HERE.parent
    paths = [repo / 'CMakeLists.txt', HERE / 'build_native.py', HERE / 'tailreset_statistics.h']
    for directory in (repo / 'src', repo / 'cmake', HERE / 'native'):
        paths.extend(directory.rglob('*'))
    return file_identity(repo, paths)


def dependency_files(root):
    paths = [root / 'CMakeLists.txt', root / 'LICENSE']
    for directory in ('src', 'file_lists'):
        paths.extend((root / directory).rglob('*'))
    return file_identity(root, paths)


def read_cache(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and ':' in line and '=' in line:
            name, value = line.split('=', 1)
            result[name.split(':', 1)[0]] = value
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', choices=['smoke', 'paper'], default='paper')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cache-level', choices=['2', '3'], default='3')
    parser.add_argument('--predecode-mode', choices=['0', '1', '2'], default='0')
    parser.add_argument('--statistics', action='store_true')
    parser.add_argument('--stim-source', type=Path)
    parser.add_argument('--stim-archive', type=Path)
    parser.add_argument('--cxx-flags', default='-O3')
    parser.add_argument('--cmake-arg', action='append', default=[])
    args = parser.parse_args(argv)
    root = args.output.resolve()
    if root.exists() or root == HERE.parent or HERE.parent in root.parents:
        parser.error('Use a new build directory outside the source repository')
    if args.jobs < 1 or (args.stim_archive and not args.stim_source):
        parser.error('Positive --jobs required; --stim-archive requires --stim-source')
    identity = source_identity()
    if args.profile == 'paper' and (not identity['commit'] or identity['status']):
        parser.error('Paper builds require a clean committed source checkout')
    sources = source_files()
    root.mkdir(parents=True)
    configure = ['cmake', '-S', str(HERE / 'native'), '-B', str(root),
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
                 '-DCMAKE_CXX_FLAGS=' + args.cxx_flags,
                 '-DBB_AE_CACHE_LEVEL=' + args.cache_level,
                 '-DBB_TAILRESET_PREDECODE_MODE=' + args.predecode_mode,
                 '-DBB_TAILRESET_SYNDROME_STATS=' + ('ON' if args.statistics else 'OFF')]
    if args.stim_source:
        configure.append('-DFETCHCONTENT_SOURCE_DIR_STIM=' + str(args.stim_source.resolve()))
    if args.stim_archive:
        configure.append('-DBB_AE_STIM_ARCHIVE=' + str(args.stim_archive.resolve()))
    configure.extend(args.cmake_arg)
    build = ['cmake', '--build', str(root), '--target', 'bb_benchmark', '-j', str(args.jobs)]
    try:
        def execute(label, command):
            write(root / (label + '-command.json'), command)
            with (root / (label + '.log')).open('x') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

        execute('configure', configure)
        cache = read_cache(root / 'CMakeCache.txt')
        if Path(cache['BB_SOURCE_DIR']).resolve() != HERE.parent:
            raise ValueError('Configured candidate source differs from the sealed checkout')
        stim = Path(cache['stim_SOURCE_DIR']).resolve()
        configured_archive = cache.get('BB_AE_STIM_ARCHIVE', '')
        stim_archive = Path(configured_archive).resolve() if configured_archive else None
        dependency_sources = dependency_files(stim)
        if not dependency_sources or 'CMakeLists.txt' not in dependency_sources:
            raise ValueError('Configured Stim source could not be sealed')
        archive_before = digest(stim_archive) if stim_archive else None
        execute('build', build)
        if identity != source_identity() or sources != source_files():
            raise ValueError('Candidate source changed during the build')
        if dependency_sources != dependency_files(stim):
            raise ValueError('Stim source changed during the build')
        if stim_archive and digest(stim_archive) != archive_before:
            raise ValueError('Prebuilt Stim archive changed during the build')
        if stim_archive is None:
            archives = list(root.rglob('libstim.a'))
            if len(archives) != 1:
                raise ValueError('Expected exactly one built Stim archive')
            stim_archive = archives[0]
        binary = root / 'bin/bb_benchmark'
        compiler = cache['CMAKE_CXX_COMPILER']
        compiler_version = subprocess.check_output([compiler, '--version'], text=True).strip()
        receipt = dict(schema=1, complete=True, profile=args.profile, source=identity,
            source_files=sources, source_root=str(HERE.parent), builder_sha256=digest(__file__),
            platform=platform.platform(), machine=platform.machine(),
            compiler=dict(path=compiler, version=compiler_version),
            commands=dict(configure=configure, build=build),
            configuration={k: v for k, v in cache.items() if k.startswith(('BB_', 'CMAKE_CXX_', 'OpenMP_', 'CMAKE_INTERPROCEDURAL_'))},
            dependency=dict(name='Stim', source=str(stim), source_files=dependency_sources,
                            archive=str(stim_archive), archive_sha256=digest(stim_archive)),
            build_files={name: digest(root / name) for name in ['CMakeCache.txt', 'compile_commands.json']},
            binary=dict(path=str(binary), sha256=digest(binary)))
        write(root / 'build.json', receipt)
        print(root / 'build.json')
        return 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        write(root / 'failure.json', dict(complete=False, error=str(error)))
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
