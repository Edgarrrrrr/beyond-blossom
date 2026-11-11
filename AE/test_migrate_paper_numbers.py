"""Arithmetic/provenance tests use synthetic data under a temporary directory only."""
import contextlib
import copy
from decimal import Decimal
import io
import json
from pathlib import Path
import tempfile
import unittest

import migrate_paper_numbers as M


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="paper-number-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.template = self.root / "template.tex"
        self.original = (M.HERE / "paper_num_template.tex").read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        self.template.write_bytes(self.original)
        self.config = copy.deepcopy(M.read_json(M.HERE / "paper_num_mapping.json"))
        # Keep the original timing fixture independent of measured PMU data.
        pmu_keys = {k for k, v in self.config['rules'].items() if v['op'] == 'pmu_miss_percent'}
        self.original = b''.join(line for line in self.original.splitlines(keepends=True)
            if not any(b'\\DefinePerfValue{' + key.encode() + b'}' in line for key in pmu_keys))
        self.template.write_bytes(self.original)
        for key in pmu_keys:
            self.config['rules'].pop(key)
        self.config.pop('pmu_source', None)
        self.config["metric"] = "batch3_us"
        self.config["aliases"] = {"pymatching": "Upstream", "structured-simd": "GridSIMD", "predecoding": "Predecode",
                                  "component-parallel": "Component", "tail-reset": "TailReset", "parallel-predecoding": "Spatial",
                                  "tail-reset-reference": "Component"}
        self.config["best_methods"] = ["structured-simd", "predecoding", "component-parallel", "tail-reset", "parallel-predecoding"]
        self.config["preserve"] = {}
        self.run = self.root / "synthetic-run"
        self.analysis = self.run / "analysis-r02"
        self.analysis.mkdir(parents=True)
        self.config["source"].update(run=str(self.run), analysis='analysis-r02', experiment_id="SYNTHETIC-TEST", implementation_commit="synthetic-test-source")
        self.methods = ["Upstream", "Official", "GridSIMD", "Predecode", "Component", "TailReset", "Spatial"]
        cases = []
        for d in [9, 13, 17, 21, 25, 29, 33]:
            for index, probability in enumerate(["0.0005", "0.001", "0.003"]):
                component = 80 + index
                values = {"Upstream": 1000+d, "Official": 2*(1000+d), "GridSIMD": 200+d+index,
                          "Predecode": Decimal(200+d+index)/2, "Component": component,
                          "TailReset": component-1 if d >= 17 or index == 2 else component+1,
                          "Spatial": 50+index-d}
                methods = {name: {"batch3_us": str(v), "min3_us": str(Decimal(v)*(Decimal('.8') if name == 'Upstream' else Decimal('.5'))),
                                  "median3_us": str(Decimal(v)*Decimal('.9')), "official_speedup": 999}
                           for name, v in values.items()}
                cases.append({"id": f"synthetic-d{d}-p{probability}", "d": d, "p": probability, "methods": methods})
        self.plan = {"experiment_id": "SYNTHETIC-TEST", "implementation_commit": "synthetic-test-source", "shots": 1_000_000,
                     "protocol": {"lto": "OFF", "repeats": 3, "warmup_seconds": 2, "modes": ["batch", "latency"]},
                     "cases": [{"id": c['id'], "d": c['d'], "p": c['p'], "rounds": c['d']} for c in cases], "methods": self.methods,
                     "lanes": [{"node": f"ares{i}"} for i in range(1, 7)]}
        self.write(self.run / "plan.json", self.plan)
        plan_ref = M.reference(self.run / "plan.json")
        lanes = []
        for i in range(1, 7):
            complete = self.run / f"lane-{i}.json"
            exit_path = self.run / f"exit-{i}.txt"
            self.write(complete, {"complete": True, "status": "complete", "plan": plan_ref})
            exit_path.write_text("0\n")
            lanes.append({"node": f"ares{i}", "complete": M.reference(complete), "exit": M.reference(exit_path)})
        self.summary = {"complete": True, "experiment_id": "SYNTHETIC-TEST", "implementation_commit": "synthetic-test-source",
                        "lto": "OFF", "formal_calls": 882, "plan": plan_ref, "lanes": lanes, "cases": cases}
        self.write(self.analysis / "summary.json", self.summary)
        self.receipt = {"complete": True, "summary": M.reference(self.analysis / "summary.json"), "plan": plan_ref,
                        "minimum_warmup_seconds": 2, "validated_calls": 882, "exact_differences": 0}
        self.write(self.analysis / "complete.json", self.receipt)
        self.mapping = self.root / "mapping.json"
        self.save_mapping()

    @staticmethod
    def write(path, obj):
        path.write_text(M.json_text(obj))

    def save_mapping(self):
        self.write(self.mapping, self.config)

    def invoke(self, *extra):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return M.main(["--template", str(self.template), "--mapping", str(self.mapping), *map(str, extra)])

    def test_known_arithmetic_precision_and_full_m4_block_preserved(self):
        out = self.root / "generated.tex"
        self.assertEqual(self.invoke("--output", out), 0)
        result = out.read_bytes()
        data = {i.key: i.value for i in M.declarations(result.decode())}
        self.assertEqual(data['xeon.decoder-comparison.p0p05.best.vs-pymatching.max-speedup'], '60.76')
        self.assertEqual(data['xeon.decoder-comparison.p0p1.best.vs-pymatching.max-speedup'], '57.39')
        self.assertEqual(data['xeon.decoder-comparison.p0p3.best.vs-pymatching.max-speedup'], '54.37')
        self.assertEqual(data['xeon.ablation.d33.p0p3.structured-simd.mean-time-us'], '235.00')
        self.assertEqual(data['xeon.ablation.d33.p0p3.structured-simd.vs-pymatching.speedup'], '4.40')
        self.assertEqual(data['xeon.ablation.d9.p0p05.structured-simd.mean-time-us'], '209.000')
        self.assertEqual(data['xeon.ablation.d33.p0p1.component-parallel.vs-predecoding.speedup'], '1.44')
        self.assertEqual(data['xeon.ablation.predecoding.vs-structured-simd.min-speedup'], '2.00')
        self.assertEqual(data['xeon.ablation.predecoding.vs-structured-simd.max-speedup'], '2.00')
        self.assertEqual(data['xeon.ablation.tail-reset.num-improved-settings'], '17')
        self.assertEqual(result[result.index(b'% Apple M4'):], self.original[self.original.index(b'% Apple M4'):])
        self.assertEqual(self.template.read_bytes(), self.original)
        self.assertEqual(result.count(b'\r\n'), self.original.count(b'\r\n'))
        audit = json.loads(out.with_name(out.name+'.audit.json').read_text())
        self.assertEqual(sum(r['status'] == 'updated' for r in audit['changes']), 24)
        self.assertEqual(audit['output']['sha256'], M.digest(out))
        self.assertEqual(audit['mapping_snapshot']['aliases']['pymatching'], 'Upstream')

    def test_summary_definitions_and_block_to_round_conversion(self):
        out = self.root/'summary.tex'
        self.assertEqual(self.invoke('--output', out), 0)
        values = {i.key: i.value for i in M.declarations(out.read_bytes().decode())}
        self.assertEqual(values['legacy.single-core.vs-pymatching.geomean-speedup'], '9.20')
        self.assertEqual(values['legacy.single-core.vs-pymatching.max-speedup'], '9.66')
        self.assertEqual(values['legacy.eight-core.vs-pymatching.max-speedup'], '60.76')
        self.assertEqual(values['legacy.d33.p0p1.beyond-blossom.mean-block-time-us'], '18.00')
        self.assertEqual(values['legacy.d33.p0p1.beyond-blossom.mean-round-time-us'], '0.55')

    def test_selected_statistic_and_baseline_use_times_not_cached_speedup(self):
        self.config['metric'] = 'min3_us'
        self.config['aliases']['pymatching'] = 'Official'
        self.save_mapping()
        out = self.root / 'selected.tex'
        self.assertEqual(self.invoke('--output', out), 0)
        values = {i.key: i.value for i in M.declarations(out.read_bytes().decode())}
        self.assertEqual(values['xeon.ablation.d33.p0p3.pymatching.mean-time-us'], '1033.00')
        self.assertEqual(values['xeon.ablation.d33.p0p3.structured-simd.mean-time-us'], '117.50')
        self.assertEqual(values['xeon.ablation.d33.p0p3.structured-simd.vs-pymatching.speedup'], '8.79')

    def test_incomplete_source_leaves_template_and_outputs_untouched(self):
        (self.analysis/'complete.json').unlink()
        self.assertEqual(self.invoke('--in-place'), 2)
        self.assertEqual(self.template.read_bytes(), self.original)
        self.assertFalse(list(self.root.glob('*.bak-*')))
        self.assertFalse(list(self.root.glob('*.audit.json')))

    def test_sha_mismatch_rejected(self):
        (self.analysis/'summary.json').write_text('{}')
        self.assertEqual(self.invoke('--output', self.root/'bad.tex'), 2)
        self.assertFalse((self.root/'bad.tex').exists())

    def test_analysis_exit_and_wrong_round_count_rejected(self):
        self.write(self.run/'analysis-submission-r02.json', {'returncode': 0, 'job_id': 'synthetic-job'})
        self.assertEqual(self.invoke('--output', self.root/'not-exited.tex'), 2)
        self.assertFalse((self.root/'not-exited.tex').exists())
        (self.run/'exit-synthetic-job_0.txt').write_text('0\n')
        self.assertEqual(self.invoke('--check'), 0)
        matrix = M.Matrix(self.config, self.root)
        for row in matrix.rows:
            if row['d'] == 33 and row['p'] == '0.001':
                row['rounds'] = 45
        with self.assertRaises(M.MigrationError):
            matrix.calculate(self.config['rules']['legacy.d33.p0p1.beyond-blossom.mean-round-time-us'])

    def test_unconfirmed_alias_rejected_without_output(self):
        self.config['aliases']['pymatching'] = None
        self.save_mapping()
        self.assertEqual(self.invoke('--output', self.root/'bad.tex'), 2)
        self.assertFalse((self.root/'bad.tex').exists())

    def test_m4_rule_rejected(self):
        self.config['rules']['m4pro.realtime.pymatching.num-supported-settings'] = {
            'op': 'time', 'method': 'pymatching', 'd': 9, 'p': '.001'}
        self.save_mapping()
        self.assertEqual(self.invoke('--in-place'), 2)
        self.assertEqual(self.template.read_bytes(), self.original)

    def test_missing_workload_and_duplicate_keys_rejected(self):
        self.config['rules']['xeon.ablation.d33.p0p3.pymatching.mean-time-us']['d'] = 23
        self.save_mapping()
        self.assertEqual(self.invoke('--output', self.root/'absent.tex'), 2)
        duplicate = self.original + b'\\DefinePerfValue{legacy.single-core.vs-pymatching.max-speedup}{9}\r\n'
        self.template.write_bytes(duplicate)
        self.assertEqual(self.invoke('--in-place'), 2)
        self.assertEqual(self.template.read_bytes(), duplicate)

    def test_preview_does_not_write_and_in_place_has_backup_and_is_idempotent(self):
        self.assertEqual(self.invoke('--check'), 0)
        self.assertEqual(self.template.read_bytes(), self.original)
        self.assertFalse(list(self.root.glob('*.bak-*')))
        self.assertEqual(self.invoke('--in-place'), 0)
        first = self.template.read_bytes()
        backups = list(self.root.glob('*.bak-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.original)
        self.assertEqual(self.invoke('--in-place'), 0)
        self.assertEqual(self.template.read_bytes(), first)


if __name__ == '__main__':
    unittest.main()
