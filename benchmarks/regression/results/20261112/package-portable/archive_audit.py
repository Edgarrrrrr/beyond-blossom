#!/usr/bin/env python3
"""Persist only completed portable-package audit products and inspectable evidence."""
from pathlib import Path
import argparse
import hashlib
import json
import shlex
import shutil
import tarfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent
ART = ROOT / 'artifacts'
WORKSPACE = Path('/home/fujy/dev/beyond_blossom/validation/20261112/package-portable')
REPO = Path('/home/fujy/dev/beyond_blossom/repo/benchmarks/regression/results/20261112/package-portable')
COMMIT = 'd2506bde8ee9a2eba7ad97523fbc948a97142676'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)

def manifest(root):
    data = {str(p.relative_to(root)): {'sha256': sha(p), 'bytes': p.stat().st_size}
            for p in sorted(root.rglob('*')) if p.is_file() and p.name != 'manifest.json'}
    result = {'files': data, 'file_count': len(data), 'total_bytes': sum(x['bytes'] for x in data.values())}
    (root / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    if not args.execute:
        print('Prepared only; no archive written. Requires a successful artifacts/audit.json.')
        return
    audit = json.loads((ART / 'audit.json').read_text())
    assert audit['complete'] and audit['commit'] == COMMIT
    assert audit['no_lto']
    identity = json.loads((ART / 'source-files.json').read_text())
    assert identity['commit'] == COMMIT and identity['selected_source_status_at_snapshot'] == ''
    assert sha(ART / 'run_audit.executed.py') == audit['audit_script_sha256']
    sdist, wheel = Path(audit['sdist']['path']), Path(audit['wheel']['path'])
    assert sha(sdist) == audit['sdist']['sha256'] and sha(wheel) == audit['wheel']['sha256']
    with tarfile.open(sdist) as stream:
        members = {m.name.split('/', 1)[1]: m for m in stream if m.isfile() and '/' in m.name}
        for relative, expected in identity['files'].items():
            assert hashlib.sha256(stream.extractfile(members[relative]).read()).hexdigest() == expected, relative
    commands = json.loads((ART / 'compile_commands.json').read_text())
    core = [entry for entry in commands if '/src/beyond_blossom/' in entry['file']]
    assert core and all('-march=x86-64' in shlex.split(entry['command']) for entry in core)
    assert not any(any(x.startswith('-flto') for x in shlex.split(entry['command'])) for entry in commands)
    suites = ET.parse(ART / 'pytest.xml').getroot()
    pytest = {key: sum(int(s.get(key, 0)) for s in suites.iter('testsuite'))
              for key in ['tests', 'failures', 'errors', 'skipped']}
    assert pytest['tests'] == 250 and pytest['failures'] == pytest['errors'] == 0
    assert not WORKSPACE.exists() and not REPO.exists(), 'Never overwrite a prior package audit archive'
    for dirname in ['dist', 'wheels', 'logs']:
        for source in sorted((ART / dirname).rglob('*')):
            if source.is_file():
                copy(source, WORKSPACE / source.relative_to(ART))
    for filename in ['audit.json', 'source.tar', 'source-files.json', 'package-source-delta.json', 'old-r2-preservation.json', 'dependencies.json', 'pytest.xml',
                     'CMakeCache.txt', 'compile_commands.json', 'run_audit.executed.py']:
        copy(ART / filename, WORKSPACE / filename)
    for filename in ['controller-provenance.json', 'prepared-only.json', 'archive_audit.py']:
        copy(ROOT / filename, WORKSPACE / filename)
    for relative in ['outside/quickstart.py', 'outside/quickstart-result.json', 'outside/cpp-consumer/main.cc',
                     'outside/cpp-consumer/CMakeLists.txt', 'outside/predictions-b8.dets', 'outside/predictions-dets.dets']:
        copy(ART / relative, WORKSPACE / relative)
    summary = {'complete': True, 'commit': COMMIT, 'pytest': pytest, 'core_compile_commands': len(core),
               'all_core_commands_have_explicit_x86_64_baseline': True, 'no_lto': True,
               'sdist': {'name': sdist.name, 'sha256': sha(sdist), 'bytes': sdist.stat().st_size},
               'wheel': {'name': wheel.name, 'sha256': sha(wheel), 'bytes': wheel.stat().st_size},
               'source_manifest_file_count': len(identity['files']), 'all_manifest_files_survive_sdist_unchanged': True,
               'audit_script_sha256': sha(ART / 'run_audit.executed.py'),
               'workspace_archive': str(WORKSPACE),
               'prior_delivery_and_r2_artifacts_untouched': True}
    (WORKSPACE / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    readme = f'''# Portable-baseline package audit

The source distribution and wheel were freshly built from selected committed
package paths at `{COMMIT}`. All {len(identity['files'])} source manifest files
survive the source distribution byte-for-byte. The unrelated working-tree report
changes are explicitly recorded and excluded by `git archive`; this receipt does
not claim that the whole working tree was clean.

The wheel was built offline from the unpacked source distribution, then installed
into a new target directory and tested from outside the package source directory.
All 250 packaged Python tests passed (see `summary.json` for skipped-test count),
including Python/native APIs. CLI help/error/prediction checks passed. The same
unpacked source built the native CLI before SDK install, and an independent CMake
consumer found `BeyondBlossom::decoder`, compiled and ran successfully.

Every core compile command contains the explicit `-march=x86-64` baseline; LTO
is absent. The executed controller and its hash are retained. This new audit
incorporates the already resolved native-CLI-before-install and Stim-definition
consumer setup, so the old r2 audit and its setup-failure history remain intact.

The workspace archive contains the new `.tar.gz` source package, `.whl`, selected
source tar and complete small receipts/logs. This repository directory keeps only
receipts, source hashes, commands, test results and consumer inputs. It omits
intermediate objects, installed binaries and duplicate dependency trees.
Delivery filenames and pointers were not changed by this archival task.
'''
    (WORKSPACE / 'README.md').write_text(readme)
    excluded = {'source.tar', 'CMakeCache.txt'}
    for source in sorted(WORKSPACE.rglob('*')):
        if not source.is_file():
            continue
        relative = source.relative_to(WORKSPACE)
        if relative.parts[0] in {'dist', 'wheels'} or source.name in excluded:
            continue
        copy(source, REPO / relative)
    a, b = manifest(WORKSPACE), manifest(REPO)
    print(json.dumps({'summary': summary,
                      'workspace_files': a['file_count'], 'workspace_bytes': a['total_bytes'],
                      'repo_files': b['file_count'], 'repo_bytes': b['total_bytes']}, indent=2))

if __name__ == '__main__':
    main()
