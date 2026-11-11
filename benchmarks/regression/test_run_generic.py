"""Collector control tests with synthetic timing rows; no native timing runs."""
import csv
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


RUNNER = pathlib.Path(__file__).with_name('run_generic.py')


class GenericCollectorTests(unittest.TestCase):
    def test_summary_weights_processes_equally_after_within_process_medians(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            with (root / 'raw.csv').open('w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=[
                    'workload', 'variant', 'matrix_repeat', 'native_repeat', 'ns_per_shot'])
                writer.writeheader()
                for variant, scale in [('baseline', 1), ('off', 2), ('on', 0.5)]:
                    for process, values in enumerate([[1, 100, 100], [2, 2, 2]]):
                        for batch, value in enumerate(values):
                            writer.writerow(dict(workload='synthetic', variant=variant, matrix_repeat=process,
                                                 native_repeat=batch, ns_per_shot=value * scale))
            subprocess.run([sys.executable, str(RUNNER), '--output', str(root), '--summarize-only'],
                           check=True, capture_output=True)
            row = json.loads((root / 'summary.json').read_text())[0]
            self.assertEqual(row['median_ns_per_shot'], {'baseline': 51, 'off': 102, 'on': 25.5})
            self.assertEqual(row['speedup_on_vs_source'], 2)

    def test_multiple_batches_are_retained_and_binaries_are_archived(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            mock = root / 'mock-native'
            mock.write_text('''#!/usr/bin/env python3
import pathlib, sys
if '--write-weights' in sys.argv:
    pathlib.Path(sys.argv[sys.argv.index('--write-weights') + 1]).write_bytes(bytes(8))
else:
    assert pathlib.Path(sys.argv[sys.argv.index('--expected-weights') + 1]).read_bytes() == bytes(8)
print('workload,variant,repeat,shots,elapsed_ns,ns_per_shot,weight_checksum,weight_sequence_hash,observable_checksum,complete_shots,skipped_shots')
for repeat in range(int(sys.argv[5])):
    print(f'{sys.argv[6]},{sys.argv[4]},{repeat},1,100,100,0,42,0,0,0')
''')
            mock.chmod(0o755)
            (root / 'graph').write_text('synthetic graph')
            (root / 'shots').write_text('synthetic shot')
            (root / 'manifest.json').write_text(json.dumps([dict(
                name='synthetic', kind='graph', graph=str(root / 'graph'), shots=str(root / 'shots'))]))
            output = root / 'run'
            subprocess.run([sys.executable, str(RUNNER), '--baseline-bin', str(mock), '--candidate-bin', str(mock),
                            '--corpora', str(root), '--output', str(output), '--candidate-identity', 'synthetic',
                            '--repeats', '2', '--batches-per-process', '3'], check=True, capture_output=True)
            with (output / 'raw.csv').open() as raw:
                rows = list(csv.DictReader(raw))
            self.assertEqual(len(rows), 18)
            self.assertEqual({row['matrix_repeat'] for row in rows}, {'0', '1'})
            self.assertEqual({row['native_repeat'] for row in rows}, {'0', '1', '2'})
            manifest = json.loads((output / 'manifest.json').read_text())
            self.assertEqual(manifest['status'], 'complete')
            self.assertEqual(manifest['batches_per_process'], 3)
            for command in manifest['commands']:
                self.assertEqual(pathlib.Path(command[0]).parent, output / 'artifacts')
            for variant in ['baseline', 'candidate']:
                self.assertEqual((output / 'artifacts' / variant).read_bytes(), mock.read_bytes())


if __name__ == '__main__':
    unittest.main()
