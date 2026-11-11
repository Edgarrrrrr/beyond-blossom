import copy
import csv
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
import tailreset_collect as C
import tailreset_data as D


class ValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root/'call'; self.out.mkdir()
        self.reference = self.root/'reference.csv'
        C.write_csv(self.reference,[dict(shot_index=i,detection_events=n,integer_weight=2,observable_mask=0) for i,n in enumerate([2,0])])
        self.case = dict(inputs=dict(reference=C.R.ref(self.reference)))
        C.R.write(self.out/'correctness.json',dict(status='passed',shots=2,weight_differences=0,observable_differences=0,actual_candidate_witnesses=0))
        C.R.write(self.out/'warmup.json',dict(requested_seconds=2,actual_elapsed_ns=2_000_000_000))
        self.rows = [dict(shot_index=i,shot_execution_index=i,input_events=n,integer_weight=2,observable_mask=0,
            c0_normalized=n,c0_residual=n,c0_pairs=0,c0_boundaries=0,c1_normalized=0,c1_residual=0,c1_pairs=0,c1_boundaries=0) for i,n in enumerate([2,0])]
        self.meta = dict(complete=True,timed=False,mode=1,shots=2,completed_shots=1,
            components=[dict(normalized=2,residual=2,pairs=0,boundaries=0),dict(normalized=0,residual=0,pairs=0,boundaries=0)])

    def save(self):
        C.write_csv(self.out/'syndromes.csv',self.rows)
        C.R.write(self.out/'syndromes.json',self.meta)

    def validate(self, policy='skip'):
        return C.validate(self.out,self.case,policy,'statistics',2)

    def test_valid_independent_component_totals(self):
        self.save(); r=self.validate()
        self.assertEqual((r['normalized'],r['residual'],r['completed_shots']),(2,2,1))
        self.assertNotIn('mean_us',r)

    def test_conservation_and_skip_are_enforced(self):
        for field,value in [('c0_residual',1),('c0_pairs',1)]:
            with self.subTest(field=field):
                self.rows[0][field]=value; self.save()
                with self.assertRaises(ValueError): self.validate()
                (self.out/'syndromes.csv').unlink(); self.rows[0][field]=2 if field=='c0_residual' else 0

    def test_order_coverage_and_optimum_are_enforced(self):
        for field,value in [('shot_index',1),('integer_weight',3),('input_events',3)]:
            with self.subTest(field=field):
                original=self.rows[0][field]; self.rows[0][field]=value; self.save()
                with self.assertRaises(ValueError): self.validate()
                (self.out/'syndromes.csv').unlink(); self.rows[0][field]=original

    def test_statistics_cannot_supply_timing(self):
        self.save(); (self.out/'measurement.csv').write_text('unexpected')
        with self.assertRaises(ValueError): self.validate()

    def test_mismatched_policy_and_missing_tie_witness_rejected(self):
        self.save()
        with self.assertRaises(ValueError): self.validate('coordinate')
        C.R.write(self.out/'correctness.json',dict(status='passed',shots=2,weight_differences=0,observable_differences=1,actual_candidate_witnesses=0))
        with self.assertRaises(ValueError): self.validate()

    def test_forbidden_or_duplicate_nodes_before_output(self):
        for nodes in ['ares7,ares2,ares3','ares2,ares2,ares3']:
            target=self.root/'must-not-exist'
            with self.assertRaises(ValueError): C.freeze(SimpleNamespace(output=target,nodes=nodes))
            self.assertFalse(target.exists())

    def test_bundle_paths_cannot_escape(self):
        with self.assertRaises(ValueError): D.relative(self.root,'../outside.json')
        with self.assertRaises(ValueError): D.relative(self.root,'/tmp/outside.json')

    def test_incomplete_bundle_is_rejected(self):
        path=self.root/'SOURCE.json'
        C.R.write(path,dict(complete=False,schema=1))
        with self.assertRaises(ValueError): D.verify(path)


if __name__=='__main__': unittest.main()
