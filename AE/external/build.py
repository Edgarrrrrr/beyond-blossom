#!/usr/bin/env python3
"""Build locked native external decoders and the fixed independent integer oracle."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tomllib

from common import HERE, REFERENCE_REVISION, sha256, source_identity, verify_files, verify_sources, write_json

_vendor_spec = importlib.util.spec_from_file_location('beyond_blossom_ae_vendor', HERE.parent / 'vendor.py')
_vendor = importlib.util.module_from_spec(_vendor_spec)
_vendor_spec.loader.exec_module(_vendor)
materialize = _vendor.materialize


def checked_version(command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def portable_unionfind_source(destination):
    """Make the size conversion accepted by Clang explicit outside the timer."""
    source = (HERE / 'native/uf.cpp').read_text()
    old = '    UnionFindCPP::Decoder<BoundaryLattice> decoder(m,endpoints);'
    if source.count(old) != 1:
        raise ValueError('Historical UF constructor changed; review the portability adaptation')
    source = source.replace('#include <chrono>', '#include <chrono>\n#include <limits>')
    source = source.replace(old, '''    if (m > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        edges.size() > static_cast<size_t>(std::numeric_limits<int>::max()) - m)
        throw std::runtime_error("Graph exceeds the native 32-bit index range");
    UnionFindCPP::Decoder<BoundaryLattice> decoder(static_cast<uint32_t>(m),endpoints);''')
    destination.write_text(source)
    return destination


def vendor_crates(cache, destination):
    """Create an isolated Cargo directory source from checksum-verified archives."""
    lock = tomllib.loads((HERE / 'native/blossom/Cargo.lock').read_text())
    archives = {p.name: p for p in Path(cache).rglob('*.crate')}
    missing = [f"{p['name']}-{p['version']}.crate" for p in lock['package']
               if 'source' in p and f"{p['name']}-{p['version']}.crate" not in archives]
    if missing:
        raise ValueError('Offline crate cache is incomplete: ' + ', '.join(missing))
    destination.mkdir()
    for package in lock['package']:
        if 'source' not in package:
            continue
        if package['source'] != 'registry+https://github.com/rust-lang/crates.io-index':
            raise ValueError('Only the locked crates.io registry is accepted')
        name = f"{package['name']}-{package['version']}"
        archive = archives[name + '.crate']
        if sha256(archive) != package['checksum']:
            raise ValueError(f'Crate checksum mismatch: {name}')
        with tarfile.open(archive) as stream:
            for item in stream:
                target = (destination / item.name).resolve()
                if destination.resolve() / name not in target.parents:
                    if item.isdir() and target == destination.resolve() / name:
                        target.mkdir(exist_ok=True)
                        continue
                    raise ValueError(f'Unsafe crate member: {item.name}')
                if item.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif item.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(stream.extractfile(item).read())
                else:
                    raise ValueError(f'Non-regular crate member: {item.name}')
        directory = destination / name
        files = {str(p.relative_to(directory)): sha256(p) for p in directory.rglob('*') if p.is_file()}
        (directory / '.cargo-checksum.json').write_text(json.dumps(dict(files=files, package=package['checksum'])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--only', nargs='+', choices=['unionfind', 'ldpc-uf', 'blossom', 'oracle'],
                        default=['unionfind', 'ldpc-uf', 'blossom', 'oracle'])
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--cargo', default='cargo')
    parser.add_argument('--toolchain', required=False, help='Explicit installed Rust toolchain, e.g. nightly-2026-03-10')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--offline', action='store_true', help='Disable Cargo networking; locked bundled crates are used by default')
    parser.add_argument('--crate-cache', type=Path, help='Directory containing every .crate in Cargo.lock; enables offline vendoring')
    parser.add_argument('--reference-build', type=Path, help='Output of AE/build_reference.py; required for oracle')
    parser.add_argument('--reference-record', type=Path, help='Optional supplemented reference receipt; otherwise reference-build/reference.json')
    parser.add_argument('--stim-source', type=Path)
    parser.add_argument('--stim-archive', type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if 'blossom' in args.only and not args.toolchain:
        parser.error('--toolchain is required for blossom; the historical floating nightly is not a version lock')
    if 'oracle' in args.only and not args.reference_build:
        parser.error('oracle requires --reference-build from AE/build_reference.py')
    if 'oracle' in args.only:
        record = json.loads((args.reference_record or args.reference_build / 'reference.json').read_text())
        if args.stim_source is None and record.get('stim_source'):
            args.stim_source = Path(record['stim_source'])
        if args.stim_archive is None and record.get('stim_archive'):
            args.stim_archive = Path(record['stim_archive'])
        if not args.stim_source or not args.stim_archive:
            parser.error('An older reference receipt requires explicit --stim-source and --stim-archive; new bundled builds record both')
    root = args.output.resolve()
    if root.exists():
        parser.error('Use a fresh output directory')
    if HERE.parent.parent in root.parents:
        parser.error('Keep build products outside the published source repository')
    lock = verify_sources()
    start_identity = source_identity()
    builder_start = sha256(__file__)
    common_start = sha256(HERE / 'common.py')
    sparse_header_start = sha256(HERE / 'native/sparse_samples.h')
    oracle_start = sha256(HERE / 'native/oracle.cpp') if 'oracle' in args.only else None
    root.mkdir(parents=True)
    vendored = None
    if 'blossom' in args.only and args.crate_cache is None:
        vendored = materialize(root / 'dependencies', ['crates'])
        args.crate_cache = Path(vendored['crate_cache'])
    versions = dict(cxx=checked_version([args.cxx, '--version']))
    commands, binaries, generated_sources = {}, {}, {}
    environment = os.environ.copy()
    # Record inherited compiler settings; default builds preserve the historical flags.
    inherited = {key: environment[key] for key in ('CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'CARGO_HOME', 'RUSTUP_HOME') if key in environment}
    # Keep Cargo's cache/config separate from the user's global Cargo directory.
    # RUSTUP_HOME and the explicitly selected installed toolchain stay unchanged.
    cargo_home = root / 'cargo-home'
    cargo_home.mkdir()
    environment['CARGO_HOME'] = str(cargo_home)

    def run(name, command):
        commands[name] = command
        write_json(root / (name + '-command.json'), command)
        with (root / (name + '.log')).open('x') as stream:
            subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=environment, cwd=root, check=True)

    uf, ldpc = HERE / 'sources/unionfind', HERE / 'sources/ldpc-union-find'
    common = [args.cxx, '-std=c++20', '-O3', '-DNDEBUG', '-I' + str(HERE / 'native')]
    if 'unionfind' in args.only:
        adapted = portable_unionfind_source(root / 'uf-portable.cpp')
        generated_sources[adapted.name] = sha256(adapted)
        includes = [uf / 'patched_include', uf / 'upstream/UnionFindPy/cpp/include',
                    uf / 'upstream/UnionFindPy/cpp/externals/robin-map/include', uf / 'deps', uf]
        run('unionfind', common + ['-include', 'cassert'] + ['-I' + str(p) for p in includes] +
            [str(adapted), str(uf / 'upstream/UnionFindPy/cpp/src/utility.cpp'), '-o', str(root / 'unionfind')])
        binaries['unionfind'] = root / 'unionfind'
    if 'ldpc-uf' in args.only:
        includes = [ldpc / 'diagnostic_include', ldpc, ldpc / 'upstream/src_cpp', ldpc / 'upstream/include/robin_map', uf / 'deps']
        run('ldpc-uf', common + ['-DBENCH_LDPC'] + ['-I' + str(p) for p in includes] +
            [str(HERE / 'native/uf.cpp'), '-o', str(root / 'ldpc-uf')])
        binaries['ldpc-uf'] = root / 'ldpc-uf'
    if 'blossom' in args.only:
        cargo = [args.cargo, '+' + args.toolchain]
        versions['cargo'] = checked_version(cargo + ['--version'])
        rustc = str(Path(args.cargo).resolve().parent / 'rustc') if '/' in args.cargo else 'rustc'
        versions['rustc'] = checked_version([rustc, '+' + args.toolchain, '--version', '--verbose'])
        sysroot = Path(checked_version([rustc, '+' + args.toolchain, '--print', 'sysroot']))
        manifest = sysroot / 'lib/rustlib/multirust-channel-manifest.toml'
        if manifest.is_file():
            versions['rust_release_manifest'] = dict(date=tomllib.loads(manifest.read_text())['date'], sha256=sha256(manifest))
        flags = []
        if args.crate_cache:
            vendor_crates(args.crate_cache, root / 'vendor')
            flags += ['--offline', '--config', 'source.crates-io.replace-with="external-locked"',
                      '--config', 'source.external-locked.directory=' + json.dumps(str(root / 'vendor'))]
        elif args.offline:
            flags += ['--offline']
        run('blossom', cargo + ['build', '--locked', '--release', '--jobs', str(args.jobs), '--manifest-path',
            str(HERE / 'native/blossom/Cargo.toml'), '--target-dir', str(root / 'rust')] + flags)
        shutil.copyfile(root / 'rust/release/decoder-bench-blossom', root / 'blossom')
        (root / 'blossom').chmod(0o755)
        binaries['blossom'] = root / 'blossom'
    reference = None
    if 'oracle' in args.only:
        refroot = args.reference_build.resolve()
        reference_path = args.reference_record or refroot / 'reference.json'
        reference = json.loads(reference_path.read_text())
        if not reference.get('complete') or reference.get('upstream_revision') != REFERENCE_REVISION:
            raise ValueError('Independent reference revision does not match the fixed baseline')
        verify_files(refroot / 'upstream', reference['source_files'])
        if sha256(refroot / 'upstream/LICENSE') != reference['source_license_sha256']:
            raise ValueError('Independent reference license identity changed')
        library = Path(reference['matching_library']['path'])
        if not library.is_absolute():
            library = refroot / library
        if sha256(library) != reference['matching_library']['sha256']:
            raise ValueError('Independent reference library identity changed')
        if sha256(args.stim_archive) != reference['stim_archive_sha256']:
            raise ValueError('Stim archive differs from the fixed reference build')
        verify_files(args.stim_source.resolve(), reference['stim_source_files'])
        includes = [refroot / 'upstream/src', args.stim_source.resolve() / 'src', uf / 'deps']
        run('oracle', common + ['-pthread'] + ['-I' + str(p) for p in includes] + [str(HERE / 'native/oracle.cpp'),
            str(library), str(args.stim_archive.resolve()), '-o', str(root / 'oracle')])
        binaries['oracle'] = root / 'oracle'
    verify_sources()
    finish_identity = source_identity()
    if sha256(__file__) != builder_start or sha256(HERE / 'common.py') != common_start or sha256(HERE / 'native/sparse_samples.h') != sparse_header_start or (oracle_start is not None and sha256(HERE / 'native/oracle.cpp') != oracle_start):
        raise ValueError('Build controller or oracle source changed during compilation')
    consistent_identity = dict(finish_identity)
    if finish_identity != start_identity:
        consistent_identity.update(dirty=True, status='Repository identity changed during compilation; see source_start/source_finish')
    receipt = dict(schema=1, complete=True, source=consistent_identity, source_start=start_identity, source_finish=finish_identity, platform=platform.platform(),
                   machine=platform.machine(), compilers=versions, inherited_environment=inherited,
                   build_environment=dict(cwd=str(root), cargo_home=str(cargo_home), rustup_home=environment.get('RUSTUP_HOME')),
                   commands=commands, generated_sources=generated_sources, sources_lock_sha256=sha256(HERE / 'sources.lock.json'),
                   builder_sha256=sha256(__file__), common_sha256=sha256(HERE / 'common.py'),
                   sparse_samples_header_sha256=sparse_header_start,
                   oracle_source_sha256=sha256(HERE / 'native/oracle.cpp') if 'oracle' in binaries else None,
                   cargo_lock_sha256=sha256(HERE / 'native/blossom/Cargo.lock'), rust_toolchain=args.toolchain,
                   reference=reference, binaries={name: dict(path=str(path), sha256=sha256(path)) for name, path in binaries.items()},
                   historical_source_files_verified=len(lock['files']))
    if vendored:
        receipt['vendored_dependencies'] = vendored
    write_json(root / 'build.json', receipt)
    print(root / 'build.json')


if __name__ == '__main__':
    main()
