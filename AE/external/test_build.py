"""Build-controller checks without invoking a compiler or Cargo."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('external_build_controller', HERE / 'build.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class BuildEnvironmentTest(unittest.TestCase):
    def test_vendor_import_preserves_module_search_order(self):
        original = list(sys.path)
        isolated = importlib.util.spec_from_file_location('external_build_import_check', HERE / 'build.py')
        module = importlib.util.module_from_spec(isolated)
        isolated.loader.exec_module(module)
        self.assertEqual(sys.path, original)

    def test_cargo_build_uses_its_own_home_and_working_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'build'
            calls = []

            def execute(command, **kwargs):
                calls.append((command, kwargs))
                executable = output / 'rust/release/decoder-bench-blossom'
                executable.parent.mkdir(parents=True)
                executable.write_bytes(b'mock executable, never run')

            def version(command):
                return str(Path(temporary) / 'sysroot') if '--print' in command else 'mock compiler version'

            argv = ['build.py', '--output', str(output), '--only', 'blossom',
                    '--toolchain', 'nightly-test', '--crate-cache', str(Path(temporary) / 'cache')]
            identity = {'commit': 'test', 'dirty': False, 'status': ''}
            with (mock.patch.object(sys, 'argv', argv),
                  mock.patch.dict(os.environ, {'CARGO_HOME': '/ignored/global-cargo',
                      'RUSTUP_HOME': '/installed/rustup', 'RUSTFLAGS': '-C target-cpu=x86-64'}),
                  mock.patch.object(builder, 'verify_sources', return_value={'files': {}}),
                  mock.patch.object(builder, 'source_identity', return_value=identity),
                  mock.patch.object(builder.platform, 'platform', return_value='test platform'),
                  mock.patch.object(builder.platform, 'machine', return_value='x86_64'),
                  mock.patch.object(builder, 'checked_version', side_effect=version),
                  mock.patch.object(builder, 'vendor_crates'),
                  mock.patch.object(builder.subprocess, 'run', side_effect=execute),
                  contextlib.redirect_stdout(io.StringIO())):
                builder.main()
            self.assertEqual(len(calls), 1)
            command, kwargs = calls[0]
            self.assertIn('--offline', command)
            self.assertIn('--locked', command)
            self.assertEqual(kwargs['cwd'], output)
            self.assertEqual(kwargs['env']['CARGO_HOME'], str(output / 'cargo-home'))
            self.assertEqual(kwargs['env']['RUSTUP_HOME'], '/installed/rustup')
            self.assertEqual(kwargs['env']['RUSTFLAGS'], '-C target-cpu=x86-64')
            receipt = json.loads((output / 'build.json').read_text())
            self.assertTrue(receipt['complete'])
            self.assertEqual(receipt['build_environment']['cargo_home'], str(output / 'cargo-home'))
            self.assertEqual(receipt['build_environment']['cwd'], str(output))
            self.assertEqual(receipt['inherited_environment']['CARGO_HOME'], '/ignored/global-cargo')


if __name__ == '__main__':
    unittest.main()
