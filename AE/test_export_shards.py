"""Small sealed multi-host fixtures; no decoder or Slurm process is launched."""
import json
from pathlib import Path
import tempfile
import unittest

import export_results as E
from test_export_results import native_fixture,external_fixture,dump,reseal_native_call,reseal_external_group


class ShardExportTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def bundle(self,distance,host,*,policy=True,external=True,rate=.003):
        root=self.root/f'd{distance}-p{rate}'
        opts=dict(distances_override=[distance],rates_override=[rate],host=host)
        native=native_fixture(root/'native',policy=-1,**opts)
        bundle={'native_run':str(native.relative_to(self.root))}
        if external:bundle['external_run']=str(external_fixture(root/'external',native).relative_to(self.root))
        if policy:
            for label,measurement in [('policy_timing','batch'),('policy_counts','statistics')]:
                bundle[label]={}
                for mode in E.POLICIES:
                    path=native_fixture(root/f'{label}-{mode}',measurement,policy=mode,serial_policy=True,**opts)
                    bundle[label][str(mode)]=str(path.relative_to(self.root))
                    if mode==2:
                        if measurement=='statistics':
                            artifact=next(path.glob('calls/*/*/*/native/syndromes.json'))
                            value=E.read(artifact);value['mode']=3
                            dump(artifact,value);reseal_native_call(path,artifact)
                        else:
                            plan=E.read(path/'plan.json');plan['binary']['build_record']={'configuration':{'BB_TAILRESET_PREDECODE_MODE':'3'}}
                            dump(path/'plan.json',plan)
                            done=E.read(path/'complete.json');done['plan_sha256']=E.R.digest(path/'plan.json');dump(path/'complete.json',done)
        return bundle

    def manifest(self,bundles):
        path=self.root/'shards.json';dump(path,dict(schema=1,shards=bundles));return path

    def test_two_hosts_keep_each_case_paired_and_export_simple_policy(self):
        bundles=[self.bundle(9,'ares1'),self.bundle(13,'ares2')]
        for level in ('L2','L3'):
            path=native_fixture(self.root/level,'pmu',policy=-1,level=level,
                                distances_override=[13],rates_override=[.003],host='ares2')
            bundles[1][level.lower()+'_run']=str(path.relative_to(self.root))
        manifest=self.manifest(bundles);output=self.root/'export'
        self.assertEqual(E.main(['--shards',str(manifest),'--profile','smoke','--groups',
            'ablation','comparison','predecoder','xeon-macros','--no-render','--output',str(output)]),0)
        value=E.read(output/'canonical.json')
        self.assertEqual(value['placement']['scope'],'per-setting paired multi-host')
        self.assertEqual(value['placement']['cases']['d09-p0.003']['host'],'ares1')
        self.assertEqual(value['placement']['cases']['d13-p0.003']['host'],'ares2')
        for category in ('native','external','predecoder'):
            self.assertTrue(all(row['host']==('ares1' if row['distance']==9 else 'ares2') for row in value[category]))
        self.assertEqual({r['physical_policy'] for r in value['predecoder'] if r['policy']==2},{3})
        self.assertEqual(E.read(output/'macro-audit.json')['performance_values'],{})

    def test_duplicate_case_and_partial_paper_matrix_are_rejected(self):
        bundle=self.bundle(9,'ares1',policy=False,external=False)
        with self.assertRaisesRegex(ValueError,'Duplicate native shard case'):
            E.shard_runs(self.manifest([bundle,bundle]),{'ablation'},'smoke')
        native,*_=E.shard_runs(self.manifest([bundle]),{'ablation'},'smoke')
        with self.assertRaisesRegex(ValueError,'full 21-setting'):
            E.complete_matrix(E.aggregate(native),E.STAGES,'paper')

    def test_cross_host_policy_and_cross_commit_native_are_rejected(self):
        a=self.bundle(9,'ares1');b=self.bundle(13,'ares2')
        path=self.root/a['policy_counts']['0'];plan=E.read(path/'plan.json');plan['host']='ares2'
        dump(path/'plan.json',plan);done=E.read(path/'complete.json');done['plan_sha256']=E.R.digest(path/'plan.json');dump(path/'complete.json',done)
        with self.assertRaisesRegex(ValueError,'different hosts'):
            E.shard_runs(self.manifest([a,b]),{'predecoder'},'smoke')
        for bundle in (a,b):bundle.pop('policy_counts');bundle.pop('policy_timing')
        path=self.root/b['native_run'];plan=E.read(path/'plan.json');plan['source']['commit']='other-source'
        dump(path/'plan.json',plan);done=E.read(path/'complete.json');done['plan_sha256']=E.R.digest(path/'plan.json');dump(path/'complete.json',done)
        with self.assertRaisesRegex(ValueError,'different candidate source'):
            E.shard_runs(self.manifest([a,b]),{'ablation'},'smoke')

    def test_complete_distributed_matrix_produces_xeon_macros(self):
        bundles=[self.bundle(d,f'ares{(i%6)+1}',rate=p,policy=False,external=False)
                 for i,(d,p) in enumerate((d,p) for p in E.RATES for d in E.DISTANCES)]
        manifest=self.manifest(bundles);output=self.root/'complete-export'
        self.assertEqual(E.main(['--shards',str(manifest),'--profile','smoke','--groups',
            'ablation','xeon-macros','--no-render','--output',str(output)]),0)
        audit=E.read(output/'macro-audit.json')
        self.assertTrue(audit['native_matrix_complete'])
        self.assertTrue(audit['performance_values'])
        self.assertEqual(len(audit['placement']),21)
        self.assertTrue(all(not k.startswith('m4pro.') for k in audit['performance_values']))

    def test_external_shard_cannot_use_another_candidate_commit(self):
        bundle=self.bundle(9,'ares1',policy=False)
        root=self.root/bundle['external_run'];path=next(root.glob('*/n2/run/summary.json'))
        value=E.read(path);value['configuration']['build']['source']['commit']='foreign-source'
        dump(path,value);reseal_external_group(root,path)
        with self.assertRaisesRegex(ValueError,'External shard candidate source'):
            E.shard_runs(self.manifest([bundle]),{'comparison'},'smoke')

    def test_later_shard_timing_variant_cannot_hide_behind_first_plan(self):
        a=self.bundle(9,'ares1');b=self.bundle(13,'ares2')
        path=self.root/b['policy_timing']['2'];original=E.read(path/'plan.json')
        for field,value in [('BB_TAILRESET_PREDECODE_MODE','2'),
                            ('BB_TAILRESET_SYNDROME_STATS','ON'),('BB_AE_CACHE_LEVEL','2')]:
            with self.subTest(field=field):
                plan=json.loads(json.dumps(original))
                plan['binary']['build_record']['configuration'][field]=value
                dump(path/'plan.json',plan)
                done=E.read(path/'complete.json');done['plan_sha256']=E.R.digest(path/'plan.json');dump(path/'complete.json',done)
                with self.assertRaisesRegex(ValueError,'different native build variants'):
                    E.shard_runs(self.manifest([a,b]),{'predecoder'},'smoke')


if __name__=='__main__':unittest.main()
