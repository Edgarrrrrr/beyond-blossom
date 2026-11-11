"""Exercise provenance and saved-input orchestration without building or decoding."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import build_native
import reproduce


class OrchestrationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def saved_run(self):
        source = self.root / 'saved'
        case = 'd09-p0.003'
        directory = source / 'inputs' / case
        directory.mkdir(parents=True)
        pbits = struct.unpack('<Q', struct.pack('<d', .003))[0]
        (directory / 'dataset.bin').write_bytes(struct.pack('<12Q', 0x5354454e43494c31, 2, 9, pbits, 2, 720, 0, 0, 0, 0, 0, 17))
        (directory / 'model.dem').write_text('detector(0,0,0) D0\n')
        (directory / 'circuit.stim').write_text('R 0\n')
        reproduce.write(source / 'plan.json', dict(distances=[9], error_rates=[.003], shots=2, seed=555))
        files = {str(p.relative_to(source)): reproduce.digest(p) for p in directory.iterdir()}
        reproduce.write(source / 'complete.json', dict(complete=True,
            plan_sha256=reproduce.digest(source / 'plan.json'), inputs=[dict(case=case, files=files)]))
        return source

    def test_input_replay_preserves_seed_stream_and_never_prepares(self):
        source = self.saved_run()
        binary = self.root / 'candidate'
        binary.write_text('test executable identity; never launched')
        output = self.root / 'replay'
        calls = []
        def invoke(command, directory):
            calls.append(command)
            native = directory / 'native'
            native.mkdir(parents=True)
            reproduce.write(native / 'correctness.json', dict(status='passed', weight_differences=0))
            (native / 'measurement.csv').write_text('elapsed_ns,shots\n10000,2\n')
            return native
        with mock.patch.object(reproduce, 'invoke', side_effect=invoke), mock.patch.object(
                reproduce, 'source_identity', return_value=dict(commit='test', status='')):
            code = reproduce.main(['run', '--binary', str(binary), '--input-run', str(source),
                '--output', str(output), '--methods', 'structured', '--cpus', '0'])
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 1)
        self.assertNotIn('prepare', calls[0])
        self.assertEqual(calls[0][calls[0].index('--stream-id') + 1], '17')
        self.assertEqual(calls[0][calls[0].index('--root-seed') + 1], '555')
        self.assertEqual(reproduce.digest(source / 'inputs/d09-p0.003/dataset.bin'),
                         reproduce.digest(output / 'inputs/d09-p0.003/dataset.bin'))

    def test_saved_input_corruption_and_duplicates_are_rejected(self):
        source = self.saved_run()
        receipt = json.loads((source / 'complete.json').read_text())
        receipt['inputs'].append(receipt['inputs'][0])
        (source / 'complete.json').write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            reproduce.saved_inputs(source)
        receipt['inputs'].pop()
        (source / 'complete.json').write_text(json.dumps(receipt))
        (source / 'inputs/d09-p0.003/model.dem').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'Changed'):
            reproduce.saved_inputs(source)

    def test_external_command_preserves_explicit_zero_and_method_list(self):
        args = argparse.Namespace(ae_run=Path('/a'), build=Path('/b'), output=Path('/c'),
                                  profile='smoke', warmup_seconds=0, methods=['parity', 'fusion'])
        command = reproduce.external_command(args)
        self.assertEqual(command[command.index('--warmup-seconds') + 1], '0')
        self.assertEqual(command[-3:], ['--methods', 'parity', 'fusion'])
        self.assertNotIn('--limit', command)

    def test_receipt_rejects_wrong_binary_or_source_commit(self):
        fake_ae = self.root / 'repo/AE'
        fake_ae.mkdir(parents=True)
        (fake_ae / 'build_native.py').write_text('builder')
        source = fake_ae.parent / 'CMakeLists.txt'
        source.write_text('source')
        binary = self.root / 'binary'
        binary.write_text('binary')
        identity = dict(commit='current', status='')
        receipt = dict(complete=True, source=identity, binary=dict(sha256=reproduce.digest(binary)),
                       source_files={'CMakeLists.txt': reproduce.digest(source)},
                       builder_sha256=reproduce.digest(fake_ae / 'build_native.py'))
        record = self.root / 'build.json'
        record.write_text(json.dumps(receipt))
        with mock.patch.object(reproduce, 'HERE', fake_ae):
            reproduce.native_build_record(record, binary, identity)
            with self.assertRaisesRegex(ValueError, 'receipt'):
                reproduce.native_build_record(record, binary, dict(commit='different', status=''))
            binary.write_text('different binary')
            with self.assertRaisesRegex(ValueError, 'receipt'):
                reproduce.native_build_record(record, binary, identity)

    def test_build_receipt_is_emitted_only_after_stable_mock_build(self):
        stim = self.root / 'stim'
        (stim / 'src').mkdir(parents=True)
        (stim / 'CMakeLists.txt').write_text('fake dependency')
        (stim / 'src/stim.h').write_text('fake header')
        output = self.root / 'build'
        def run(command, **kwargs):
            if '-S' in command:
                (output / 'CMakeCache.txt').write_text(
                    f'BB_SOURCE_DIR:PATH={reproduce.HERE.parent}\nstim_SOURCE_DIR:STATIC={stim}\nCMAKE_CXX_COMPILER:FILEPATH=fake-cxx\n')
                (output / 'compile_commands.json').write_text('[]')
            else:
                (output / 'bin').mkdir()
                (output / 'bin/bb_benchmark').write_text('fake executable')
                (output / 'libstim.a').write_text('fake archive')
        with (mock.patch.object(build_native, 'source_identity', return_value=dict(commit='test', status='')),
             mock.patch.object(build_native, 'source_files', return_value={'CMakeLists.txt': 'test-hash'}),
             mock.patch.object(build_native.subprocess, 'run', side_effect=run),
             mock.patch.object(build_native.subprocess, 'check_output', return_value='test compiler')):
            self.assertEqual(build_native.main(['--output', str(output), '--stim-source', str(stim)]), 0)
        receipt = json.loads((output / 'build.json').read_text())
        self.assertEqual(receipt['binary']['sha256'], reproduce.digest(output / 'bin/bb_benchmark'))
        self.assertEqual(receipt['dependency']['archive_sha256'], reproduce.digest(output / 'libstim.a'))
        output = self.root / 'changed-build'
        with (mock.patch.object(build_native, 'source_identity', side_effect=[
                  dict(commit='test', status=''), dict(commit='changed', status='')]),
              mock.patch.object(build_native, 'source_files', return_value={'CMakeLists.txt': 'test-hash'}),
              mock.patch.object(build_native.subprocess, 'run', side_effect=run)):
            self.assertEqual(build_native.main(['--output', str(output), '--stim-source', str(stim)]), 1)
        self.assertFalse((output / 'build.json').exists())
        self.assertFalse(json.loads((output / 'failure.json').read_text())['complete'])


if __name__ == '__main__':
    unittest.main()
