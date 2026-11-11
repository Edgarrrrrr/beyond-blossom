import contextlib
import copy
from decimal import Decimal
import io
import json
from pathlib import Path
import tempfile
import unittest

import generate_predecoder_macros as G


class PredecoderMacroTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = json.loads((G.HERE/'predecoder_macro_source.json').read_text())
        cls.summary = G.D.verify(G.HERE/'data/tailreset/SOURCE.json')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def invoke(self, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return G.main(list(map(str,args)))

    def test_actual_table_and_narrative_values(self):
        values = G.calculate(self.summary,self.config)
        self.assertEqual(len(values),58)
        prefix='xeon.predecoder-comparison.d33.p0p3.'
        expected={'none.mean-time-us':'382.49','none.mean-remaining-syndromes':'1929.86',
            'coordinate.mean-time-us':'223.65','coordinate.mean-remaining-syndromes':'161.61','coordinate.matched-percent':'91.63',
            'degree-ordered.mean-time-us':'156.51','degree-ordered.mean-remaining-syndromes':'100.45','degree-ordered.matched-percent':'94.79'}
        for suffix,value in expected.items(): self.assertEqual(G.format_value(values[prefix+suffix]['value']),value)
        bounds='xeon.predecoder-comparison.p0p3.degree-ordered.matched-percent.'
        self.assertEqual(G.format_value(values[bounds+'min']['value']),'94.79')
        self.assertEqual(G.format_value(values[bounds+'max']['value']),'95.18')

    def test_integer_operands_and_final_rounding(self):
        altered=copy.deepcopy(self.summary)
        for row in altered['rows']: row['mean_decoding_us_per_shot']=row['remaining_syndromes_per_shot']=999
        for call in altered['calls']: call['mean_us']=999
        a=G.calculate(self.summary,self.config); b=G.calculate(altered,self.config)
        self.assertEqual(a,b)
        self.assertEqual(G.format_value(Decimal('1.005')),'1.01')
        item=a['xeon.predecoder-comparison.d33.p0p3.degree-ordered.matched-percent']
        self.assertEqual(item['operands']['initial_total'],1929856873)
        self.assertEqual(item['operands']['residual_total'],100453438)

    def test_missing_duplicate_zero_and_inconsistent_counts_fail(self):
        for kind in ['missing','duplicate','zero','residual','normalized','matched']:
            with self.subTest(kind=kind):
                s=copy.deepcopy(self.summary)
                stat=next(c for c in s['calls'] if c['method']=='degree' and c['phase']=='statistics')
                if kind=='missing': s['calls'].remove(next(c for c in s['calls'] if c['method']=='degree' and c['phase']=='batch'))
                elif kind=='duplicate': s['calls'].append(copy.deepcopy(s['calls'][0]))
                elif kind=='zero': next(c for c in s['calls'] if c['method']=='skip' and c['phase']=='statistics')['normalized']=0
                elif kind=='residual': stat['residual']=-1
                elif kind=='normalized': stat['normalized']+=1
                else: stat['components'][0]['pairs']+=1
                with self.assertRaises((ValueError,KeyError)): G.calculate(s,self.config)

    def test_generated_file_audit_and_idempotence(self):
        target=self.root/'numbers.tex'
        self.assertEqual(self.invoke('--output',target),0)
        first=target.read_bytes()
        self.assertEqual(self.invoke('--output',target),0)
        self.assertEqual(target.read_bytes(),first)
        audit=json.loads(target.with_name(target.name+'.audit.json').read_text())
        self.assertTrue(audit['complete']); self.assertEqual(len(audit['values']),58)
        self.assertEqual(audit['source']['sha256'],self.config['manifest']['sha256'])
        self.assertEqual(audit['script']['sha256'],G.M.digest(G.__file__))
        self.assertEqual(self.invoke('--check','--numbers',target),0)
        manuscript=self.root/'manuscript.tex'
        manuscript.write_text('\n'.join('\\PerfValue{'+key+'}' for key in audit['values']))
        self.assertEqual(self.invoke('--check','--numbers',target,'--manuscript',manuscript),0)
        manuscript.write_text('\\PerfValue{missing-key}')
        self.assertEqual(self.invoke('--check','--numbers',target,'--manuscript',manuscript),2)
        target.write_text(target.read_text().replace('{382.49}','{0.00}'))
        prior=target.read_bytes()
        self.assertEqual(self.invoke('--check','--numbers',target),2)
        self.assertEqual(target.read_bytes(),prior)

    def test_checksum_failure_writes_nothing(self):
        config=copy.deepcopy(self.config)
        config['manifest']['path']=str(G.HERE/'data/tailreset/SOURCE.json')
        config['manifest']['sha256']='0'*64
        path=self.root/'config.json';path.write_text(json.dumps(config))
        output=self.root/'must-not-exist.tex'
        self.assertEqual(self.invoke('--config',path,'--output',output),2)
        self.assertFalse(output.exists());self.assertFalse(output.with_name(output.name+'.audit.json').exists())

    def test_inputs_and_unrelated_outputs_are_protected(self):
        source=G.HERE/'data/tailreset/evidence/analysis-r01/summary.json'
        before=source.read_bytes()
        self.assertEqual(self.invoke('--output',source),2)
        self.assertEqual(source.read_bytes(),before)
        output=self.root/'numbers.tex'; unrelated=self.root/'other.tex';unrelated.write_text('keep')
        self.assertEqual(self.invoke('--output',unrelated),2)
        self.assertEqual(unrelated.read_text(),'keep')
        self.assertEqual(self.invoke('--output',output,'--audit',G.HERE/'paper_num_template.tex'),2)
        self.assertFalse(output.exists())

    def test_unconfirmed_aliases_and_moving_percent_denominator_fail(self):
        for key,value in [('variants',{'none':'Official','coordinate':'coordinate','degree-ordered':'degree'}),
                          ('physical_error_rate','0.001'),('decimal_places',3)]:
            config=copy.deepcopy(self.config);config[key]=value
            with self.assertRaises(ValueError): G.calculate(self.summary,config)


if __name__=='__main__': unittest.main()
