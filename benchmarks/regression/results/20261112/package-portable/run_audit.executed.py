#!/usr/bin/env python3
"""Audit a clean committed sdist -> wheel -> outside-install chain; inert without --execute."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import zipfile

DEFAULT_REPO = Path('/home/fujy/dev/beyond_blossom/repo')
STIM = Path('/home/fujy/dev/PyMatching/.local/cache/EXP-20261013-01/source-package-verify-r01/source-snapshot/stim')
GTEST = STIM.parent / 'googletest'
SELECTED = ('CMakeLists.txt', 'cmake', 'src', 'python', 'tests', 'data', 'licenses', 'setup.py', 'pyproject.toml',
            'MANIFEST.in', 'README.md', 'LICENSE', 'NOTICE', 'CONTRIBUTING.md', 'SECURITY.md', 'SOURCE.json', 'CITATION.cff')


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(data)
    return result.hexdigest()


def write(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write('\n')


def extract(archive, destination):
    destination.mkdir()
    root = destination.resolve()
    for item in archive:
        path = (root / item.name).resolve()
        if root not in path.parents:
            raise ValueError(f'Unsafe archive member: {item.name}')
        if item.isdir():
            path.mkdir(parents=True, exist_ok=True)
        elif item.isfile():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.extractfile(item).read())
            path.chmod(item.mode & 0o777)
        else:
            raise ValueError(f'Nonregular source archive member: {item.name}')


QUICKSTART = r'''
from pathlib import Path
import importlib.metadata as metadata
import json
import sys
import numpy as np
import stim
import beyond_blossom as bb
from beyond_blossom import _cpp_beyond_blossom as native

installed = Path(sys.argv[1]).resolve()
assert installed in Path(bb.__file__).resolve().parents, bb.__file__
assert installed in Path(native.__file__).resolve().parents, native.__file__
m = bb.Matching()
m.add_edge(0, 1, fault_ids=0, weight=1.0)
m.add_boundary_edge(0, fault_ids=1, weight=2.0)
assert m.decode([1, 1]).tolist() == [1, 0]
circuit = stim.Circuit.generated('surface_code:rotated_memory_x', distance=5, rounds=5,
                                after_clifford_depolarization=0.001)
model = circuit.detector_error_model(decompose_errors=True)
shots = np.ascontiguousarray(circuit.compile_detector_sampler(seed=17).sample(shots=100), dtype=np.uint8)
generic = bb.Matching.from_detector_error_model(model)
expected, expected_weights = generic.decode_batch(shots, return_weights=True)
decoder = bb.StructuredDecoder.from_detector_error_model(model, distance=5, rounds=5,
    strategy='predecoded', simd='scalar', include_search_graph=True)
predictions, weights = decoder.decode_batch(shots, return_weights=True)
assert predictions.shape == (100, 1) and weights.shape == (100,)
np.testing.assert_allclose(weights, expected_weights, rtol=0, atol=1e-9)
direct = native._StructuredDecoder(str(model), 5, 5, 'rotated', 'predecoded', 'scalar', [], False, True)
native_predictions, native_weights = direct.decode_batch(shots, False, False)
np.testing.assert_array_equal(native_predictions, predictions)
np.testing.assert_array_equal(native_weights, weights)
packed = np.packbits(shots, axis=1, bitorder='little')
packed_predictions, packed_weights = direct.decode_batch(packed, True, True)
np.testing.assert_array_equal(packed_predictions, np.packbits(predictions, axis=1, bitorder='little'))
np.testing.assert_array_equal(packed_weights, weights)
for row in range(8):
    events = np.flatnonzero(shots[row]).tolist()
    prediction, weight = direct.decode_sparse(events)
    np.testing.assert_array_equal(prediction, predictions[row])
    assert abs(weight - weights[row]) < 1e-9
    assert direct.decode_edges(events, False).shape[1] == 2
    assert direct.decode_edges(events, True).shape[1] == 2
record = dict(package=bb.__file__, extension=native.__file__, version=metadata.version('beyond-blossom'),
              python_version=sys.version,
              matching_result=[1, 0], structured_predictions=list(predictions.shape),
              native_sparse_cases=8, metadata=decoder.metadata,
              dependency_versions={name:metadata.version(name) for name in ['numpy','scipy','networkx','matplotlib','stim','rustworkx','pytest']})
Path('quickstart-result.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
'''

CPP = r'''
#include <beyond_blossom/structured/decoder.h>
#include <stim/dem/detector_error_model.h>
#include <array>
#include <iostream>
#include <vector>
int main() {
    stim::DetectorErrorModel model(R"(
        error(0.11) D0 D1 L0
        error(0.03) D0
        error(0.02) D1
        detector(0, 0, 0) D0
        detector(2, 0, 0) D1
    )");
    beyond_blossom::structured::Options options;
    options.distance = 3; options.rounds = 3; options.rotated = false;
    options.simd = beyond_blossom::GridSimd::scalar;
    options.include_search_graph = true;
    beyond_blossom::structured::Decoder decoder(model, options);
    const std::array<std::vector<uint64_t>, 4> shots{{{}, {0}, {1}, {0, 1}}};
    const auto results = decoder.decode_batch(shots);
    if (results.size() != 4 || results[0].obs_mask != 0 || results[0].weight != 0 || results[3].obs_mask != 1)
        return 1;
    const auto again = decoder.decode(shots[3]);
    if (again != results[3]) return 2;
    std::vector<int64_t> edges;
    decoder.decode_to_edges(shots[3], edges);
    if (edges.empty() || edges.size() % 2) return 3;
    std::cout << "installed CMake StructuredDecoder: passed\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true', help='Start the full build/test chain only after final-root authorization')
    parser.add_argument('--commit', help='Exact final 40-character HEAD SHA; required with --execute')
    parser.add_argument('--repo', type=Path, default=DEFAULT_REPO)
    parser.add_argument('--output', type=Path, default=Path('/tmp/beyond-package-portable-final/artifacts'))
    parser.add_argument('--python', type=Path, default=Path('/home/fujy/dev/PyMatching/.conda-env/bin/python'))
    parser.add_argument('--stim-source', type=Path, default=STIM)
    parser.add_argument('--googletest-source', type=Path, default=GTEST)
    parser.add_argument('--pybind-wheel', type=Path, default=Path('/tmp/pybind11-2.13.6-py3-none-any.whl'))
    parser.add_argument('--rustworkx-wheel', type=Path, default=Path('/tmp/rustworkx-0.18.1-cp310-abi3-manylinux2014_x86_64.manylinux_2_17_x86_64.whl'))
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if not args.execute:
        print(json.dumps(dict(status='prepared_only', selected_paths=SELECTED, output=str(args.output),
                              stages=['clean exact-HEAD archive', 'sdist', 'unpack sdist', 'offline wheel build from unpacked sdist',
                                      'outside target install', 'all packaged Python tests', 'CLI status/help/predict',
                                      'Python and direct native StructuredDecoder', 'installed CMake C++ StructuredDecoder'],
                              no_snapshot_or_build_started=True), indent=2))
        return 0
    if not args.commit or not re.fullmatch('[0-9a-f]{40}', args.commit) or args.jobs < 1:
        parser.error('--execute needs the exact final HEAD SHA and positive --jobs')
    repo, root = args.repo.resolve(), args.output.resolve()
    def git(*command):
        return subprocess.check_output(['git', '-C', str(repo), *command])
    if git('rev-parse', 'HEAD').decode().strip() != args.commit:
        raise ValueError('Requested commit is not the current final HEAD')
    repository_status = git('status', '--porcelain', '--untracked-files=all').decode()
    selected_source_status = git('status', '--porcelain', '--untracked-files=all', '--', *SELECTED).decode()
    if selected_source_status.strip():
        raise ValueError('Package source paths must match the requested committed snapshot')
    if root.exists() or root == repo or repo in root.parents:
        raise ValueError('Use a fresh artifact directory outside the source checkout')
    for path in [args.python, args.pybind_wheel, args.rustworkx_wheel,
                 args.stim_source / 'CMakeLists.txt', args.googletest_source / 'CMakeLists.txt']:
        if not path.is_file():
            raise ValueError(f'Missing offline dependency: {path}')
    available = set(git('ls-tree', '--name-only', args.commit).decode().splitlines())
    selected = [name for name in SELECTED if name in available]
    if set(SELECTED) - available:
        raise ValueError('Required package inputs missing from final commit: ' + ', '.join(set(SELECTED) - available))
    root.mkdir(parents=True)
    shutil.copyfile(__file__, root / 'run_audit.executed.py')
    for name in ['logs', 'outside', 'dist', 'wheels', 'mpl']:
        (root / name).mkdir()
    environment = os.environ.copy()
    for name in ('PYTHONPATH', 'PYTHONHOME', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CMAKE_ARGS'):
        environment.pop(name, None)
    environment.update(PYTHONNOUSERSITE='1', PIP_NO_INDEX='1', PIP_DISABLE_PIP_VERSION_CHECK='1',
                       PIP_NO_CACHE_DIR='1', MPLCONFIGDIR=str(root / 'mpl'), CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs),
                       CMAKE_GENERATOR='Unix Makefiles')
    environment['PATH'] = str(args.python.parent) + os.pathsep + environment.get('PATH', '')
    commands = []
    def run(name, command, *, cwd=None, env=None, expected=0):
        record = dict(name=name, argv=[str(v) for v in command], cwd=str(cwd or root / 'outside'), expected_returncode=expected)
        commands.append(record)
        write(root / 'logs' / (name + '.command.json'), record)
        with (root / 'logs' / (name + '.log')).open('x') as stream:
            result = subprocess.run(record['argv'], cwd=record['cwd'], env=env or environment,
                                    stdout=stream, stderr=subprocess.STDOUT)
        record['returncode'] = result.returncode
        if (expected is None and result.returncode == 0) or (expected is not None and result.returncode != expected):
            raise RuntimeError(f'{name} exited {result.returncode}; inspect {root}/logs/{name}.log')
        return (root / 'logs' / (name + '.log')).read_text()
    try:
        fixture_suffixes = {'.dem', '.stim', '.b8', '.dets', '.txt'}
        data_files = git('ls-tree', '-r', '--name-only', args.commit, '--', 'data').decode().splitlines()
        archive_paths = [name for name in selected if name != 'data'] + [name for name in data_files if Path(name).suffix in fixture_suffixes]
        omitted_data_utilities = [name for name in data_files if Path(name).suffix not in fixture_suffixes]
        archive = git('archive', '--format=tar', args.commit, '--', *archive_paths)
        (root / 'source.tar').write_bytes(archive)
        with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
            extract(stream, root / 'source')
        source_files = {str(p.relative_to(root / 'source')): digest(p) for p in sorted((root / 'source').rglob('*')) if p.is_file()}
        source_identity = dict(commit=args.commit, archive_sha256=hashlib.sha256(archive).hexdigest(), selected_paths=selected,
                               omitted_nonpackage_data_utilities=omitted_data_utilities,
                               files=source_files, clean_at_snapshot=True,
                               snapshot_scope='Selected package paths from exact git archive commit; unrelated report changes excluded',
                               repository_status_at_snapshot=repository_status,
                               selected_source_status_at_snapshot=selected_source_status,
                               created_utc=datetime.now(timezone.utc).isoformat())
        write(root / 'source-files.json', source_identity)
        dependency_files = {name: {str(p.relative_to(directory)): digest(p) for p in sorted((directory / 'src').rglob('*')) if p.is_file()}
                            for name, directory in [('stim', args.stim_source), ('googletest', args.googletest_source)]}
        write(root / 'dependencies.json', dict(source_expected_revisions=json.loads((root / 'source/SOURCE.json').read_text())['dependency_revisions'],
              files=dependency_files, pybind_wheel_sha256=digest(args.pybind_wheel), rustworkx_wheel_sha256=digest(args.rustworkx_wheel)))
        run('local-dependencies', [args.python, '-m', 'pip', 'install', '--no-index', '--no-deps', '--target', root / 'deps',
                                  args.pybind_wheel, args.rustworkx_wheel])
        build_environment = dict(environment, PYTHONPATH=str(root / 'deps'))
        flags = ['-DFETCHCONTENT_SOURCE_DIR_STIM=' + str(args.stim_source.resolve()),
                 '-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=' + str(args.googletest_source.resolve()),
                 '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
                 '-DBB_REQUIRE_OPENMP=ON', '-DBB_SANITIZERS=OFF', '-DBB_TEST_COVERAGE=OFF']
        build_environment['CMAKE_ARGS'] = shlex.join(flags)
        run('sdist', [args.python, 'setup.py', 'sdist', '--dist-dir', root / 'dist'], cwd=root / 'source', env=build_environment)
        sdists = list((root / 'dist').glob('*.tar.gz'))
        if len(sdists) != 1:
            raise ValueError('Expected one source distribution')
        with tarfile.open(sdists[0]) as stream:
            extract(stream, root / 'unpacked')
        unpacked = list((root / 'unpacked').iterdir())
        if len(unpacked) != 1 or not unpacked[0].is_dir():
            raise ValueError('Unexpected source distribution layout')
        package_source = unpacked[0]
        for relative, expected in source_files.items():
            # Every selected tracked build/test/data input must survive MANIFEST.in.
            if not (package_source / relative).is_file() or digest(package_source / relative) != expected:
                raise ValueError(f'Sdist omitted or changed tracked package input: {relative}')
        if not (package_source / 'data/three_errors.dets').is_file():
            raise ValueError('Regression fixture three_errors.dets missing from sdist')
        if (package_source / 'AE').exists() or (package_source / 'benchmarks').exists():
            raise ValueError('Research tooling leaked into the runtime source distribution')
        run('wheel', [args.python, '-m', 'pip', 'wheel', '--no-index', '--no-deps', '--no-build-isolation',
                      '--wheel-dir', root / 'wheels', '.'], cwd=package_source, env=build_environment)
        wheels = list((root / 'wheels').glob('beyond_blossom-*.whl'))
        if len(wheels) != 1:
            raise ValueError('Expected one built wheel')
        with zipfile.ZipFile(wheels[0]) as stream:
            names = stream.namelist()
            if not any(name.startswith('beyond_blossom/_cpp_beyond_blossom') and name.endswith('.so') for name in names):
                raise ValueError('Wheel lacks the compiled extension')
            wheel_licenses = {}
            for relative in ('LICENSE', 'NOTICE', 'licenses/Stim.txt', 'licenses/pybind11.txt'):
                matching = [name for name in names if '.dist-info/' in name and name.endswith('/' + Path(relative).name)]
                expected = (package_source / relative).read_bytes()
                if len(matching) != 1 or stream.read(matching[0]) != expected:
                    raise ValueError('Wheel license missing or changed: ' + relative)
                wheel_licenses[matching[0]] = hashlib.sha256(expected).hexdigest()
        run('install', [args.python, '-m', 'pip', 'install', '--no-index', '--no-deps', '--target', root / 'installed', wheels[0]])
        test_environment = dict(environment, PYTHONPATH=os.pathsep.join([str(root / 'installed'), str(root / 'deps')]))
        (root / 'outside/quickstart.py').write_text(QUICKSTART)
        run('quickstart', [args.python, root / 'outside/quickstart.py', root / 'installed'], env=test_environment)
        run('pytest', [args.python, '-m', 'pytest', package_source / 'tests', '-q', '--basetemp', root / 'pytest-temp',
                       '--junitxml', root / 'pytest.xml'], env=test_environment)
        entry = root / 'installed/bin/beyond-blossom'
        help_output = run('cli-help', [entry, '--help'], env=test_environment)
        if 'unstructured|structured|predecoded' not in help_output or 'Traceback' in help_output:
            raise ValueError('Console help does not describe the canonical API')
        error_output = run('cli-unknown', [entry, 'unknown-command'], env=test_environment, expected=2)
        if 'Unrecognized command' not in error_output or 'Traceback' in error_output:
            raise ValueError('Console command errors expose a traceback or omit the message')
        error_output = run('cli-missing-input', [entry, 'predict', '--dem', root / 'outside/missing.dem'], env=test_environment, expected=None)
        if 'Failed to open' not in error_output or 'Traceback' in error_output:
            raise ValueError('Native CLI failure is not propagated correctly')
        for format_ in ['dets', 'b8']:
            prediction = root / 'outside' / ('predictions-' + format_ + '.dets')
            run('cli-predict-' + format_, [entry, 'predict', '--dem', package_source / 'data/three_errors.dem',
                 '--in', package_source / ('data/three_errors.' + format_), '--in_format', format_,
                 '--out', prediction, '--out_format', 'dets'], env=test_environment)
            if prediction.read_text() != 'shot\nshot L0\nshot L2\nshot L1\n':
                raise ValueError('Console prediction differs from the packaged fixture')
        caches = [p for p in (package_source / 'build').rglob('CMakeCache.txt') if 'BB_BUILD_PYTHON:BOOL=ON' in p.read_text()]
        if len(caches) != 1:
            raise ValueError('Expected one extension CMake build')
        native_build = caches[0].parent
        compile_commands = json.loads((native_build / 'compile_commands.json').read_text())
        if any(any(flag.startswith('-flto') for flag in shlex.split(row['command'])) for row in compile_commands):
            raise ValueError('Package build unexpectedly used LTO')
        shutil.copyfile(caches[0], root / 'CMakeCache.txt')
        shutil.copyfile(native_build / 'compile_commands.json', root / 'compile_commands.json')
        run('sdk-cli-build', ['cmake', '--build', native_build, '--target', 'beyond_blossom',
                              '--parallel', str(args.jobs)], env=build_environment)
        run('sdk-install', ['cmake', '--install', native_build, '--prefix', root / 'sdk'], env=build_environment)
        for name, original in [('LICENSE', package_source / 'LICENSE'), ('NOTICE', package_source / 'NOTICE'),
                               ('Stim.txt', args.stim_source / 'LICENSE')]:
            installed_license = root / 'sdk/share/licenses/BeyondBlossom' / name
            if not installed_license.is_file() or installed_license.read_bytes() != original.read_bytes():
                raise ValueError('Native SDK license missing or changed: ' + name)
        sdk_help = run('sdk-cli-help', [root / 'sdk/bin/beyond_blossom', '--help'])
        if 'unstructured|structured|predecoded' not in sdk_help:
            raise ValueError('Installed native CLI help omits canonical strategy API')
        consumer = root / 'outside/cpp-consumer'
        consumer.mkdir()
        (consumer / 'main.cc').write_text(CPP)
        (consumer / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.18)\nproject(PackageConsumer LANGUAGES CXX)\nfind_package(BeyondBlossom CONFIG REQUIRED)\nadd_executable(consumer main.cc)\ntarget_link_libraries(consumer PRIVATE BeyondBlossom::decoder)\n')
        run('sdk-configure', ['cmake', '-S', consumer, '-B', consumer / 'build', '-DCMAKE_PREFIX_PATH=' + str(root / 'sdk'),
                             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF'])
        run('sdk-build', ['cmake', '--build', consumer / 'build', '--parallel', str(args.jobs)])
        run('sdk-consumer', [consumer / 'build/consumer'])
        for relative, expected in source_files.items():
            if digest(package_source / relative) != expected:
                raise ValueError('Packaged source changed during the audit: ' + relative)
        record = dict(complete=True, commit=args.commit, clean_snapshot=True, selected_paths=selected,
                      snapshot_scope=source_identity['snapshot_scope'],
                      repository_status_at_snapshot=repository_status,
                      selected_source_status_at_snapshot=selected_source_status, no_lto=True,
                      source_archive_sha256=digest(root / 'source.tar'), source_manifest_sha256=digest(root / 'source-files.json'),
                      sdist=dict(path=str(sdists[0]), sha256=digest(sdists[0])), wheel=dict(path=str(wheels[0]), sha256=digest(wheels[0])),
                      wheel_license_files=wheel_licenses,
                      native_license_files={str(p.relative_to(root)):digest(p) for p in (root / 'sdk/share/licenses/BeyondBlossom').iterdir() if p.is_file()},
                      installed_extension={str(p.relative_to(root)):digest(p) for p in (root / 'installed/beyond_blossom').glob('*.so')},
                      dependency_manifest_sha256=digest(root / 'dependencies.json'),
                      compile_commands_sha256=digest(root / 'compile_commands.json'), commands=commands,
                      quickstart=json.loads((root / 'outside/quickstart-result.json').read_text()),
                      junit_sha256=digest(root / 'pytest.xml'), audit_script_sha256=digest(__file__),
                      completed_utc=datetime.now(timezone.utc).isoformat())
        write(root / 'audit.json', record)
        print(root / 'audit.json')
        return 0
    except BaseException as error:
        write(root / 'failure.json', dict(complete=False, error=str(error), commands=commands, commit=args.commit))
        raise


if __name__ == '__main__':
    raise SystemExit(main())
