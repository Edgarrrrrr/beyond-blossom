"""PMU provenance, integrity, units, macro generation and collector safety checks."""
import contextlib
import copy
from decimal import Decimal, localcontext
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest

import migrate_paper_numbers as M
import pmu_data as P


class PmuTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ae-pmu-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.template = self.root/'template.tex'
        self.original = (M.HERE/'paper_num_template.tex').read_bytes().replace(b'\r\n',b'\n').replace(b'\n',b'\r\n')
        self.template.write_bytes(self.original)
        self.config = copy.deepcopy(M.read_json(M.HERE/'paper_num_mapping.json'))
        self.config['source']['run'] = str(M.resolve_path(self.config['source']['run'], M.HERE))
        self.config['pmu_source']['manifest'] = str(M.resolve_path(self.config['pmu_source']['manifest'], M.HERE))
        self.mapping = self.root/'mapping.json'

    def invoke(self, *args):
        self.mapping.write_text(M.json_text(self.config))
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return M.main(['--template',str(self.template),'--mapping',str(self.mapping),*map(str,args)])

    def copied_data(self):
        original = Path(self.config['pmu_source']['manifest'])
        destination = self.root/'pmu'
        shutil.copytree(original.parent, destination)
        self.config['pmu_source']['manifest'] = str(destination/'SOURCE.json')
        return destination

    def test_four_macros_from_integer_counts_and_m4_crlf_preserved(self):
        target = self.root/'generated.tex'
        self.assertEqual(self.invoke('--output',target),0)
        values = {d.key:d.value for d in M.declarations(target.read_bytes().decode())}
        for suffix, expected in {
            'pymatching.L3miss':'0.094', 'structured-simd.L3miss':'0.017',
            'pymatching.L2miss':'60.212', 'structured-simd.L2miss':'26.988',
        }.items():
            self.assertEqual(values['xeon.ablation.d33.p0p3.'+suffix],expected)
        data = target.read_bytes()
        self.assertEqual(data[data.index(b'% Apple M4'):],self.original[self.original.index(b'% Apple M4'):])
        self.assertEqual(data.count(b'\r\n'),self.original.count(b'\r\n'))
        audit = json.loads(target.with_name(target.name+'.audit.json').read_text())
        changes = [v for v in audit['changes'] if v.get('rule',{}).get('op')=='pmu_miss_percent']
        self.assertEqual(len(changes),4)
        self.assertEqual(sum(v['status']=='updated' for v in audit['changes']),28)
        for value in changes:
            row = value['operands'][0]
            self.assertEqual(row['unit'],'percent')
            with localcontext() as ctx:
                ctx.prec=60
                self.assertEqual(Decimal(value['raw_value']),Decimal(100)*row['misses']/row['loads'])

    def test_data_hash_failure_prevents_any_output(self):
        copied = self.copied_data()
        manifest = P.read(copied/'SOURCE.json')
        pmu = copied/manifest['levels']['L2']['measurements'][0]['pmu']['path']
        pmu.write_text('{}')
        out = self.root/'bad.tex'
        self.assertEqual(self.invoke('--output',out),2)
        self.assertFalse(out.exists())
        self.assertEqual(self.template.read_bytes(),self.original)

    def test_missing_or_duplicate_measurements_and_missing_level_rejected(self):
        copied = self.copied_data()
        original = P.read(copied/'SOURCE.json')
        for kind in ['missing-call','duplicate-call','missing-level']:
            with self.subTest(kind=kind):
                manifest = copy.deepcopy(original)
                if kind=='missing-call': manifest['levels']['L2']['measurements'].pop()
                elif kind=='duplicate-call': manifest['levels']['L2']['measurements'][-1]=manifest['levels']['L2']['measurements'][0]
                else: manifest['levels'].pop('L3')
                P.write(copied/'SOURCE.json',manifest)
                self.config['pmu_source']['sha256']=P.digest(copied/'SOURCE.json')
                target=self.root/(kind+'.tex')
                self.assertEqual(self.invoke('--output',target),2)
                self.assertFalse(target.exists())

    def test_wrong_events_multiplexing_and_zero_denominator_rejected(self):
        manifest = P.read(self.config['pmu_source']['manifest'])
        base = Path(self.config['pmu_source']['manifest']).parent
        item = manifest['levels']['L2']['measurements'][0]
        original = P.read(base/item['pmu']['path'])
        for kind in ['events','coverage','scaled','multiplexed','zero','cached-percent']:
            with self.subTest(kind=kind):
                value=copy.deepcopy(original)
                if kind=='events': value['events']=P.LEVELS['L3']['events']
                elif kind=='coverage': value['time_running_ns']-=1
                elif kind=='zero': value['l2_loads']=0
                elif kind=='cached-percent': value['l2_load_miss_percent']+=1
                else: value[kind]=True
                with self.assertRaises(P.PmuError): P.counters(value,'L2',1000000)

    def test_unit_level_and_unmeasured_alias_rejected(self):
        key='xeon.ablation.d33.p0p3.pymatching.L3miss'
        original=copy.deepcopy(self.config)
        for kind in ['unit','level','alias']:
            with self.subTest(kind=kind):
                self.config=copy.deepcopy(original)
                if kind=='unit': self.config['rules'][key]['unit']='fraction'
                elif kind=='level': self.config['rules'][key]['level']='L2'
                else: self.config['aliases']['pymatching']='Official'
                out=self.root/(kind+'.tex')
                self.assertEqual(self.invoke('--output',out),2)
                self.assertFalse(out.exists())

    def test_output_and_audit_cannot_overwrite_pmu_evidence(self):
        copied=self.copied_data()
        manifest=P.read(copied/'SOURCE.json')
        evidence=copied/manifest['levels']['L3']['measurements'][0]['pmu']['path']
        original=evidence.read_bytes()
        self.assertEqual(self.invoke('--output',evidence),2)
        self.assertEqual(evidence.read_bytes(),original)
        out=self.root/'out.tex'
        self.assertEqual(self.invoke('--output',out,'--audit',copied/'SOURCE.json'),2)
        self.assertFalse(out.exists())

    def test_bundle_paths_cannot_escape(self):
        data=P.PmuData(self.config['pmu_source']['manifest'])
        for path in ['/etc/passwd','../outside.json']:
            with self.assertRaises(P.PmuError): data.checked(dict(path=path,sha256='0'*64))


if __name__=='__main__':
    unittest.main()
