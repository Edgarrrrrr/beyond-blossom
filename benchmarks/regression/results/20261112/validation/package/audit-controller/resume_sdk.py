#!/usr/bin/env python3
"""Finish the SDK audit after the extension-only wheel build omitted the CLI target."""
from pathlib import Path
import importlib.util
import hashlib
import json
import os
import shlex
import subprocess
import zipfile
from datetime import datetime, timezone

ROOT = Path('/tmp/beyond-package-final/artifacts-r2')
spec = importlib.util.spec_from_file_location('audit', ROOT / 'run_audit.executed.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
source = ROOT / 'unpacked/beyond_blossom-0.1.0'
build = source / 'build/python/temp.linux-x86_64-cpython-311/beyond_blossom._cpp_beyond_blossom'
failed = json.loads((ROOT / 'resolved-sdk-setup-failure.json').read_text())
identity = json.loads((ROOT / 'source-files.json').read_text())
commands = failed['commands'][:]
environment = os.environ.copy()
for name in ('PYTHONPATH', 'PYTHONHOME', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CMAKE_ARGS'):
    environment.pop(name, None)
environment.update(PYTHONNOUSERSITE='1', PIP_NO_INDEX='1')
environment['PATH'] = '/home/fujy/dev/PyMatching/.conda-env/bin:' + environment.get('PATH', '')

def run(name, argv):
    name += '-r2'
    record = dict(name=name, argv=[str(v) for v in argv], cwd=str(ROOT / 'outside'), expected_returncode=0)
    audit.write(ROOT / 'logs' / (name + '.command.json'), record)
    with (ROOT / 'logs' / (name + '.log')).open('x') as stream:
        completed = subprocess.run(record['argv'], cwd=record['cwd'], env=environment, stdout=stream, stderr=subprocess.STDOUT)
    record['returncode'] = completed.returncode
    commands.append(record)
    if completed.returncode:
        raise RuntimeError(name + ' failed; inspect log')

for relative, expected in identity['files'].items():
    assert audit.digest(source / relative) == expected, relative
assert failed['commit'] == identity['commit'] == 'edd29b6780713dc78f6a99d67c4c4d192e9c52b5'
cli_record = dict(name='sdk-cli-build', argv=['cmake', '--build', str(build), '--target', 'beyond_blossom', '--parallel', '2'],
                  cwd='/home/fujy/dev/beyond_blossom', expected_returncode=0, returncode=0)
assert (build / 'beyond_blossom').is_file()
assert '[100%] Built target beyond_blossom' in (ROOT / 'logs/sdk-cli-build.log').read_text()
assert json.loads((ROOT / 'logs/sdk-cli-build.command.json').read_text()) == cli_record
commands.append(cli_record)
run('sdk-install-retry', ['cmake', '--install', build, '--prefix', ROOT / 'sdk'])
for name, original in [('LICENSE', source / 'LICENSE'), ('NOTICE', source / 'NOTICE'), ('Stim.txt', audit.STIM / 'LICENSE')]:
    assert (ROOT / 'sdk/share/licenses/BeyondBlossom' / name).read_bytes() == original.read_bytes(), name
run('sdk-cli-help', [ROOT / 'sdk/bin/beyond_blossom', '--help'])
assert 'unstructured|structured|predecoded' in (ROOT / 'logs/sdk-cli-help-r2.log').read_text()
consumer = ROOT / 'outside/cpp-consumer'
consumer.mkdir(exist_ok=True)
(consumer / 'main.cc').write_text(audit.CPP.replace('#include <beyond_blossom/structured/decoder.h>', '#include <beyond_blossom/structured/decoder.h>\n#include <stim/dem/detector_error_model.h>'))
(consumer / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.18)\nproject(PackageConsumer LANGUAGES CXX)\nfind_package(BeyondBlossom CONFIG REQUIRED)\nadd_executable(consumer main.cc)\ntarget_link_libraries(consumer PRIVATE BeyondBlossom::decoder)\n')
run('sdk-configure', ['cmake', '-S', consumer, '-B', consumer / 'build', '-DCMAKE_PREFIX_PATH=' + str(ROOT / 'sdk'),
                      '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF'])
run('sdk-build', ['cmake', '--build', consumer / 'build', '--parallel', '2'])
run('sdk-consumer', [consumer / 'build/consumer'])
for relative, expected in identity['files'].items():
    assert audit.digest(source / relative) == expected, relative
wheel, = (ROOT / 'wheels').glob('*.whl')
sdist, = (ROOT / 'dist').glob('*.tar.gz')
with zipfile.ZipFile(wheel) as archive:
    licenses = {}
    for relative in ('LICENSE', 'NOTICE', 'licenses/Stim.txt', 'licenses/pybind11.txt'):
        name, = [name for name in archive.namelist() if '.dist-info/' in name and name.endswith('/' + Path(relative).name)]
        assert archive.read(name) == (source / relative).read_bytes()
        licenses[name] = hashlib.sha256(archive.read(name)).hexdigest()
compile_commands = json.loads((ROOT / 'compile_commands.json').read_text())
assert not any(any(flag.startswith('-flto') for flag in shlex.split(row['command'])) for row in compile_commands)
record = dict(complete=True, commit=identity['commit'], clean_snapshot=True, selected_paths=identity['selected_paths'],
    source_archive_sha256=audit.digest(ROOT / 'source.tar'), source_manifest_sha256=audit.digest(ROOT / 'source-files.json'),
    sdist=dict(path=str(sdist), sha256=audit.digest(sdist)), wheel=dict(path=str(wheel), sha256=audit.digest(wheel)),
    wheel_license_files=licenses,
    native_license_files={str(p.relative_to(ROOT)):audit.digest(p) for p in (ROOT / 'sdk/share/licenses/BeyondBlossom').iterdir() if p.is_file()},
    installed_extension={str(p.relative_to(ROOT)):audit.digest(p) for p in (ROOT / 'installed/beyond_blossom').glob('*.so')},
    dependency_manifest_sha256=audit.digest(ROOT / 'dependencies.json'), compile_commands_sha256=audit.digest(ROOT / 'compile_commands.json'),
    commands=commands, quickstart=json.loads((ROOT / 'outside/quickstart-result.json').read_text()),
    junit_sha256=audit.digest(ROOT / 'pytest.xml'), audit_script_sha256=audit.digest(ROOT / 'run_audit.executed.py'),
    continuation_script_sha256=audit.digest(__file__), no_lto=True,
    recovered_setup_failure=dict(record='resolved-sdk-setup-failure.json', sha256=audit.digest(ROOT / 'resolved-sdk-setup-failure.json'),
        reason='Wheel builds extension target only; SDK install also needs the native CLI. Built that target using the same unchanged verified objects and retried SDK install.'),
    consumer_header_setup_resolution=dict(initial_build_log='logs/sdk-build.log', reason='Audit consumer constructed Stim DetectorErrorModel but omitted its required definition header; included stim/dem/detector_error_model.h, with no product source changes.'),
    completed_utc=datetime.now(timezone.utc).isoformat())
audit.write(ROOT / 'audit.json', record)
print(ROOT / 'audit.json')
