"""Validate sealed matrix selection without launching native decoders."""
import json
from pathlib import Path
import tempfile
import unittest

from collect import input_matrix, shot_count
from common import sha256, paper_shots, resolve_shot_counts, classify_shot_counts, PAPER_COUNT_POLICY


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


    def test_paper_defaults_match_manuscript_sample_assignments(self):
        distances = [9, 13, 17, 21, 25, 29, 33]
        rates = [.0005, .001, .003]
        methods = ['parity', 'fusion', 'unionfind', 'ldpc-uf']
        rows = [dict(distance=d, error_rate=p, method=m, shots=paper_shots(d, p, m))
                for d in distances for p in rates for m in methods]
        self.assertEqual(sum(r['shots'] == 1000000 for r in rows), 54)
        self.assertEqual(sum(r['shots'] == 10000 for r in rows), 30)
        self.assertEqual(classify_shot_counts(rows)['policy'], PAPER_COUNT_POLICY)
        for r in rows: r['shots'] = 10000
        self.assertEqual(classify_shot_counts(rows)['mismatched_points'], 54)
        self.assertEqual(classify_shot_counts(rows)['policy'], 'custom-counts')
        self.assertEqual(paper_shots(33, .003, 'unionfind'), 1000000)
        self.assertEqual(paper_shots(9, .0005, 'parity-uf'), 10000)
        self.assertEqual(paper_shots(21, .001, 'parity'), 1000000)
        self.assertEqual(paper_shots(25, .001, 'parity'), 10000)

    def test_policy_projects_full_matrix_into_single_case_worker(self):
        settings = [(d, p) for d in [9, 13, 17, 21, 25, 29, 33] for p in [.0005, .001, .003]]
        full = resolve_shot_counts('paper', settings)
        one = resolve_shot_counts('paper', [(9, .003)], overrides=full)
        self.assertEqual(set(one), {'d09-p0.003'})
        self.assertEqual(one['d09-p0.003'], full['d09-p0.003'])
        uniform = resolve_shot_counts('paper', [(9, .003)], uniform=10000)
        self.assertEqual(set(uniform['d09-p0.003'].values()), {10000})
        smoke = resolve_shot_counts('smoke', [(9, .003)], smoke_default=32)
        self.assertEqual(set(smoke['d09-p0.003'].values()), {32})
        with self.assertRaisesRegex(ValueError, 'Unknown'):
            resolve_shot_counts('paper', [(9, .003)], overrides={'d99-p0.003': {'parity': 10000}})


if __name__ == '__main__':
    unittest.main()
