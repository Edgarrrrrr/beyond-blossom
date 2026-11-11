#!/usr/bin/env python3
"""Preserve completed local validation runs without rewriting original receipts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile
from datetime import datetime, timezone

BUILD = Path('/tmp/beyond-build')
DEST = Path('/home/fujy/dev/beyond_blossom/validation/20261112')

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def archive(name, entries):
    out = DEST / (name + '.tar.gz')
    if out.exists():
        raise RuntimeError(f'Refusing to overwrite {out}')
    rows = []
    with tarfile.open(out, 'w:gz', compresslevel=1) as tar:
        for source in entries:
            source = Path(source)
            if not source.exists():
                raise FileNotFoundError(source)
            paths = sorted(source.rglob('*')) if source.is_dir() else [source]
            for p in paths:
                if not p.is_file():
                    continue
                relative = str(p.relative_to('/tmp'))
                before = p.stat()
                sha = digest(p)
                tar.add(p, arcname=relative, recursive=False)
                after = p.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise RuntimeError(f'Source changed: {p}')
                rows.append({'original': str(p), 'member': relative, 'bytes': before.st_size, 'sha256': sha})
    receipt = {'archive': out.name, 'sha256': digest(out), 'bytes': out.stat().st_size,
               'created_utc': datetime.now(timezone.utc).isoformat(), 'files': rows,
               'receipt_policy': 'Original files are preserved byte for byte, including historical absolute paths. Extract archive under a chosen directory; member names map original /tmp locations.'}
    target = DEST / (name + '.manifest.json')
    target.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'archive': str(out), 'files': len(rows), 'bytes': out.stat().st_size, 'sha256': receipt['sha256']}), flush=True)
    return receipt

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--execute', action='store_true')
    args = p.parse_args()
    if not args.execute:
        p.error('Explicit --execute required after all measurement processes finish')
    if not (BUILD / 'structured-final-run/complete.json').exists():
        raise RuntimeError('Structured collection has not completed')
    DEST.mkdir(parents=True, exist_ok=True)
    archive('performance', [BUILD/x for x in [
        'generic-final-fast', 'generic-final-dense', 'generic-final-bin',
        'generic-final-summary.json', 'generic-final-summary.csv',
        'generic-final-protocol.json', 'generic-final-commands.sh', 'final-hardware.json',
        'generic-final-build.log', 'generic-final-fast.log', 'generic-final-dense.log',
        'structured-final-run', 'structured-final-bin', 'structured-final-build.log', 'structured-final-run.log',
        'generic-pilot-source09/source-manifest.json']])
    archive('native-validation', [BUILD/x for x in [
        'final-release-ctest.log', 'asan-final-ctest.log', 'asan-final-build.log',
        'asan-o1/CMakeCache.txt', 'asan-o1/compile_commands.json', 'asan-o1/beyond_blossom_tests',
        'asan-o1/decoder_strategy_differential', 'asan-o1/beyond_blossom_profile_tests',
        'asan-o1/Testing', 'candidate/CMakeCache.txt', 'candidate/compile_commands.json',
        'candidate/beyond_blossom_tests', 'candidate/Testing', 'candidate/libbeyond_blossom.a',
        'baseline/CMakeCache.txt', 'baseline/compile_commands.json', 'baseline/libpymatching.a',
        'asan-final-validation.json']] + [Path('/tmp/beyond-asan-source-o1')])
    pilot = sorted(BUILD.glob('generic-pilot-run*'))
    for source in sorted(BUILD.glob('generic-pilot-source*')):
        manifest = source / 'source-manifest.json'
        if manifest.exists():
            pilot.append(manifest)
    archive('exploratory-history', pilot)

if __name__ == '__main__':
    main()
