#!/usr/bin/env python3
"""Verify and unpack the repository's fixed build dependencies without networking."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
VENDOR = HERE.parent / 'vendor'
KEYS = {'stim': 'stim_source', 'googletest': 'googletest_source',
        'official': 'official_source', 'reference-adapter': 'reference_adapter',
        'crates': 'crate_cache', 'migration-source': 'migration_source'}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def load_manifest():
    manifest = json.loads((VENDOR / 'manifest.json').read_text())
    if manifest.get('schema') != 1 or set(manifest.get('components', {})) != set(KEYS):
        raise ValueError('Unsupported or incomplete vendor manifest')
    return manifest


def verify_tree(root, component):
    root = Path(root)
    actual = set()
    for path in root.rglob('*'):
        if path.is_symlink():
            raise ValueError(f'Symlink in vendored source: {path}')
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            actual.add(relative)
            if component['files'].get(relative) != digest(path):
                raise ValueError(f'Vendored source changed: {path}')
    if actual != set(component['files']):
        raise ValueError(f'Vendored source file set differs: {root}')


def extract(name, destination):
    component = load_manifest()['components'][name]
    archive = VENDOR / component['archive']
    if not archive.is_file() or digest(archive) != component['sha256']:
        raise ValueError(f'Missing or changed vendored archive: {archive}')
    destination = Path(destination).absolute()
    if destination.is_symlink():
        raise ValueError('Vendored output must not be a symlink')
    if destination.exists():
        verify_tree(destination, component)
        return destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=destination.name + '.extract-', dir=destination.parent))
    try:
        seen = set()
        with tarfile.open(archive, 'r:gz') as stream:
            for member in stream:
                path = PurePosixPath(member.name)
                if (path.is_absolute() or '..' in path.parts or len(path.parts) < 2
                        or path.parts[0] != name or not member.isfile()):
                    raise ValueError(f'Unsafe vendored archive member: {member.name}')
                relative = PurePosixPath(*path.parts[1:]).as_posix()
                if relative in seen or relative not in component['files']:
                    raise ValueError(f'Unexpected vendored archive member: {relative}')
                seen.add(relative)
                target = staging / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o644)
        verify_tree(staging, component)
        staging.rename(destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination.resolve()


def materialize(output, names=None):
    output = Path(output).resolve()
    names = list(KEYS) if names is None else list(names)
    if not names or len(set(names)) != len(names) or any(name not in KEYS for name in names):
        raise ValueError('Select distinct known dependency names')
    manifest = load_manifest()
    result = {'schema': 1, 'complete': True, 'manifest_sha256': digest(VENDOR / 'manifest.json'), 'identities': {}}
    for name in names:
        result[KEYS[name]] = str(extract(name, output / name))
        component = manifest['components'][name]
        result['identities'][name] = {'archive': component['archive'], 'sha256': component['sha256'],
                                      'origin': {k: v for k, v in component['origin'].items()
                                                 if k not in ('git_blobs', 'packages')}}
    receipt = output / 'dependencies.json'
    if receipt.exists():
        previous = json.loads(receipt.read_text())
        if previous.get('manifest_sha256') != result['manifest_sha256']:
            raise ValueError('Existing dependency receipt uses another manifest')
        result['identities'] = {**previous.get('identities', {}), **result['identities']}
        result = {**previous, **result}
    receipt.write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--only', nargs='+', choices=list(KEYS))
    args = parser.parse_args()
    materialize(args.output, args.only)
    print(args.output.resolve() / 'dependencies.json')


if __name__ == '__main__':
    main()
