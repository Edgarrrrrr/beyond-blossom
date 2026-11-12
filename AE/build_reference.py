#!/usr/bin/env python3
"""Build the fixed independent reference from bundled, verified source (offline)."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile

from vendor import digest, extract, load_manifest

HERE = Path(__file__).resolve().parent
REVISION = 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'


def check_source(root, component):
    for relative, expected in component['files'].items():
        path = Path(root) / relative
        if not path.is_file() or path.is_symlink() or digest(path) != expected:
            raise ValueError('Fixed dependency source mismatch: ' + str(path))


def unpack_git(archive, output):
    output.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream:
            target = (output / member.name).resolve()
            if output.resolve() not in target.parents:
                raise ValueError('Unsafe reference source path')
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(stream.extractfile(member).read())
            else:
                raise ValueError('Non-regular reference source member')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    history = p.add_mutually_exclusive_group()
    history.add_argument('--history-archive', type=Path)
    history.add_argument('--history-checkout', type=Path)
    history.add_argument('--reference-adapter', type=Path, help='Unmodified original adapter directory; verified against vendor manifest')
    p.add_argument('--history-ref', help='Historical tag/ref for an explicit history checkout')
    upstream = p.add_mutually_exclusive_group()
    upstream.add_argument('--upstream-repository', type=Path, help='Optional Git repository containing the fixed official revision')
    upstream.add_argument('--upstream-source', type=Path, help='Optional extracted fixed official source; all bundled files are checked')
    p.add_argument('--stim-source', type=Path)
    p.add_argument('--stim-archive', type=Path, help='Optional already-built library from the fixed Stim source')
    p.add_argument('--googletest-source', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--offline', action='store_true', help='Use only local and bundled sources (also the default)')
    p.add_argument('--cache-level', choices=['2', '3'], default='3')
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--cxx-flags', default='-O3')
    p.add_argument('--cmake-arg', action='append', default=[])
    args = p.parse_args()
    root = args.output.resolve()
    if root.exists() or root == HERE.parent or HERE.parent in root.parents:
        p.error('Use a fresh output directory outside the published repository')
    if args.jobs < 1:
        p.error('--jobs must be positive')
    if args.history_ref and not args.history_checkout:
        p.error('--history-ref requires --history-checkout')
    manifest = load_manifest()
    components = manifest['components']
    root.mkdir(parents=True)

    def execute(name, command):
        (root / (name + '-command.json')).write_text(json.dumps(command, indent=2) + '\n')
        with (root / (name + '.log')).open('x') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    source = root / 'upstream'
    if args.upstream_repository:
        archive = subprocess.check_output(['git', '-C', str(args.upstream_repository), 'archive', REVISION])
        if hashlib.sha256(archive).hexdigest() != components['official']['origin']['git_archive_sha256']:
            raise ValueError('Official Git archive does not match the fixed reference')
        unpack_git(archive, source)
    elif args.upstream_source:
        check_source(args.upstream_source, components['official'])
        # Copy only the sealed source files; a local .git directory is unnecessary.
        source.mkdir()
        for relative in components['official']['files']:
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.upstream_source / relative, target)
    else:
        extract('official', source)
    check_source(source, components['official'])

    native = root / 'tools/AE/pmu_native'
    if args.history_archive or args.history_checkout:
        option = '--archive' if args.history_archive else '--checkout'
        historical = args.history_archive or args.history_checkout
        subprocess.run([sys.executable, str(HERE / 'recover_history.py'), option, str(historical),
                        '--output', str(root / 'tools'),
                        *(['--ref', args.history_ref] if args.history_ref else [])], check=True)
        # Older full-history recovery stores LICENSE at its root, not here.
        adapter_files = {k: v for k, v in components['reference-adapter']['files'].items() if k != 'LICENSE'}
        check_source(native, dict(files=adapter_files))
    elif args.reference_adapter:
        check_source(args.reference_adapter, components['reference-adapter'])
        shutil.copytree(args.reference_adapter, native)
    else:
        extract('reference-adapter', native)
    from portable_native import adapt
    adapt(native)

    args.stim_source = (args.stim_source.resolve() if args.stim_source else extract('stim', root / 'dependencies/stim'))
    args.googletest_source = (args.googletest_source.resolve() if args.googletest_source
                             else extract('googletest', root / 'dependencies/googletest'))
    check_source(args.stim_source, components['stim'])
    check_source(args.googletest_source, components['googletest'])
    stim_source_files = {str(f.relative_to(args.stim_source)): digest(f)
                         for f in sorted((args.stim_source / 'src').rglob('*')) if f.is_file()}
    if args.stim_archive:
        args.stim_archive = args.stim_archive.resolve()
        if not args.stim_archive.is_file():
            p.error('Missing explicit Stim archive')
    else:
        execute('stim-configure', ['cmake', '-S', str(args.stim_source), '-B', str(root / 'stim-build'),
                '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
                '-DSIMD_WIDTH=128', '-DCMAKE_CXX_FLAGS=' + args.cxx_flags, *args.cmake_arg])
        execute('stim-build', ['cmake', '--build', str(root / 'stim-build'), '--target', 'libstim', '-j', str(args.jobs)])
        archives = list((root / 'stim-build').rglob('libstim.a'))
        if len(archives) != 1:
            raise ValueError('Expected one freshly built Stim archive')
        args.stim_archive = archives[0]
    stim_archive_before = digest(args.stim_archive)
    configure = ['cmake', '-S', str(native), '-B', str(root / 'build'),
        '-DPYMATCHING_SOURCE_DIR=' + str(source), '-DFETCHCONTENT_SOURCE_DIR_STIM=' + str(args.stim_source),
        '-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=' + str(args.googletest_source),
        '-DCACHED_STIM_ARCHIVE=' + str(args.stim_archive), '-DSTENCIL_GENERIC_ONLY=ON',
        '-DEXPERIMENT_ENABLE_SHARED=OFF', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
        '-DCMAKE_CXX_FLAGS=' + args.cxx_flags + ' -DPM_PMU_CACHE_LEVEL=' + args.cache_level, *args.cmake_arg]
    execute('configure', configure)
    execute('build', ['cmake', '--build', str(root / 'build'), '--target', 'experiment_benchmark', '-j', str(args.jobs)])
    for name, path in [('official', source), ('stim', args.stim_source), ('googletest', args.googletest_source)]:
        check_source(path, components[name])
    if digest(args.stim_archive) != stim_archive_before:
        raise ValueError('Stim archive changed during reference compilation')
    binary = root / 'build/bin/experiment_benchmark'
    receipt = dict(schema=1, complete=True, upstream_revision=REVISION,
        source_archive_sha256=components['official']['origin']['git_archive_sha256'],
        source_archive_identity='canonical fixed-revision Git archive; bundled snapshot verified per file',
        binary=dict(path=str(binary), sha256=digest(binary)),
        matching_library=dict(path=str(root / 'build/pymatching/libpymatching.a'),
                              sha256=digest(root / 'build/pymatching/libpymatching.a')),
        source_files={str(f.relative_to(source)): digest(f) for f in sorted((source / 'src').rglob('*')) if f.is_file()},
        source_license_sha256=digest(source / 'LICENSE'),
        stim_source=str(args.stim_source), stim_archive=str(args.stim_archive),
        stim_archive_sha256=stim_archive_before, stim_source_files=stim_source_files,
        googletest_source=str(args.googletest_source),
        historical_manifest_sha256=digest(HERE / 'historical_sources.json'),
        adapter_files={str(f.relative_to(native)): digest(f) for f in native.iterdir() if f.is_file()},
        vendor_manifest_sha256=digest(HERE.parent / 'vendor/manifest.json'),
        vendor_archives={name: components[name]['sha256'] for name in ['official', 'reference-adapter', 'stim', 'googletest']},
        builder_sha256=digest(__file__), vendor_helper_sha256=digest(HERE / 'vendor.py'),
        portability_sha256=digest(HERE / 'portable_native.py'),
        platform=platform.platform(), lto=False, cache_level=args.cache_level)
    (root / 'reference.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(binary)
    print(root / 'reference.json')


if __name__ == '__main__':
    main()
