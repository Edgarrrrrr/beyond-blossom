"""Validate sealed matrix selection without launching native decoders."""
import json
from pathlib import Path
import tempfile
import unittest

from collect import input_matrix, shot_count
from common import sha256


class MatrixSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.case = 'd09-p0.003'
        directory = self.root / 'inputs' / self.case
        directory.mkdir(parents=True)
        for name in ('dataset.bin', 'model.dem', 'circuit.stim'):
            (directory / name).write_text('sealed fixture')
        (self.root / 'plan.json').write_text(json.dumps(dict(distances=[9], error_rates=[.003], shots=64)))
        self.receipt = dict(complete=True, plan_sha256=sha256(self.root / 'plan.json'),
            inputs=[dict(case=self.case, files={str(p.relative_to(self.root)): sha256(p) for p in directory.iterdir()})])
        self.write_receipt()

    def write_receipt(self):
        (self.root / 'complete.json').write_text(json.dumps(self.receipt))

    def test_complete_matrix_and_changed_hash(self):
        _, _, cases = input_matrix(self.root)
        self.assertEqual(cases, [(self.case, 9, .003)])
        (self.root / 'inputs' / self.case / 'dataset.bin').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'identity'):
            input_matrix(self.root)

    def test_duplicate_case_is_rejected(self):
        self.receipt['inputs'].append(self.receipt['inputs'][0])
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, 'duplicated'):
            input_matrix(self.root)

    def test_explicit_case_method_and_fallback_counts(self):
        policy = dict(parity=1000000, **{self.case: dict(parity=10000)})
        self.assertEqual(shot_count(policy, self.case, 'parity', 64), 10000)
        self.assertEqual(shot_count(policy, 'd13-p0.003', 'parity', 64), 1000000)
        self.assertEqual(shot_count(policy, self.case, 'fusion', 64), 64)
        with self.assertRaisesRegex(ValueError, 'Invalid'):
            shot_count(dict(parity=True), self.case, 'parity', 64)


if __name__ == '__main__':
    unittest.main()
