#!/usr/bin/env python3
"""Post-validation receipt; does not pretend to be a pre-build receipt."""
from pathlib import Path
import hashlib
import json
import subprocess
from datetime import datetime, timezone

repo = Path('/home/fujy/dev/beyond_blossom/repo')
build = Path('/tmp/beyond-build')
source = Path('/tmp/beyond-asan-source-o1')
commit = 'edd29b6780713dc78f6a99d67c4c4d192e9c52b5'

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def run(*args):
    return subprocess.check_output(args, cwd=repo)

assert (build / 'structured-final-run/complete.json').exists(), 'Measurement still active'
paths = run('git', 'ls-tree', '-r', '--name-only', commit, 'src', 'CMakeLists.txt', 'cmake', 'licenses').decode().splitlines()
files = []
for relative in paths:
    path = source / relative
    actual = sha(path)
    expected = hashlib.sha256(run('git', 'show', f'{commit}:{relative}')).hexdigest()
    assert actual == expected, relative
    files.append({'path': relative, 'sha256': actual})

artifacts = []
for relative in ('asan-final-ctest.log', 'asan-final-build.log', 'final-release-ctest.log',
                 'asan-o1/CMakeCache.txt', 'asan-o1/compile_commands.json',
                 'asan-o1/beyond_blossom_tests', 'asan-o1/decoder_strategy_differential',
                 'asan-o1/beyond_blossom_profile_tests',
                 'candidate/beyond_blossom_tests', 'candidate/CMakeCache.txt',
                 'candidate/compile_commands.json'):
    path = build / relative
    artifacts.append({'path': str(path), 'sha256': sha(path), 'bytes': path.stat().st_size})
for log in ('asan-final-ctest.log', 'final-release-ctest.log'):
    assert '100% tests passed, 0 tests failed out of 356' in (build/log).read_text()

receipt = {
    'schema': 1,
    'kind': 'Post-validation observation of frozen source, executed artifacts, and completed logs',
    'observed_utc': datetime.now(timezone.utc).isoformat(),
    'implementation_commit': commit,
    'source_directory': str(source),
    'source_matches_commit': True,
    'source_scope': 'All tracked src, cmake, licenses files and CMakeLists.txt',
    'source_files': files,
    'release_tests': {'passed': 356, 'failed': 0, 'elapsed_seconds': 20.06},
    'asan_ubsan_tests': {'passed': 356, 'failed': 0, 'elapsed_seconds': 68.06,
        'optimization': '-O1',
        'environment': {'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
                        'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                        'OMP_WAIT_POLICY': 'PASSIVE', 'GOMP_SPINCOUNT': '0'},
        'leak_sanitizer': 'Disabled because ptrace is unsupported in the sandbox; no leak-check claim'},
    'artifacts': artifacts,
    'supersedes_for_final_validation': '/tmp/beyond-build/asan-o1-validation.json (354-test pre-gate build)',
}
output = build / 'asan-final-validation.json'
assert not output.exists()
output.write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'receipt': str(output), 'source_files': len(files), 'artifacts': len(artifacts)}))
