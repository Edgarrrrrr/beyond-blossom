"""Fresh-result export contracts using small sealed fixtures; no decoder runs."""
import csv
import json
from pathlib import Path
import tempfile
import unittest

import export_results as E


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value)+'\n')


def reseal_native_call(root, path):
    complete=E.read(root/'complete.json')
    relative=str(path.relative_to(root))
    call=next(call for call in complete['calls'] if relative in call['files'])
    call['files'][relative]=E.R.digest(path)
    dump(root/'complete.json',complete)


def reseal_external_group(root, path):
    complete=E.read(root/'complete.json')
    group=next(group for group in complete['groups'] if group['summary']==str(path.relative_to(root)))
    group['sha256']=E.R.digest(path)
    dump(root/'complete.json',complete)


def native_fixture(root, measurement='batch', *, policy=0, m4=False, level='L2', full=False, serial_policy=False):
    distances = E.DISTANCES if full else [9]
    rates = E.RATES if full and measurement not in ('statistics',) else [.003]
    if policy != -1:rates=[.003]
    methods = ['pipelined'] if policy != -1 else E.STAGES+['unstructured']
    if policy != -1 and serial_policy:methods=['predecoded']
    if measurement=='pmu':methods=['reference','structured']
    repeats=3 if measurement=='batch' else 1
    plan=dict(profile='smoke',source=dict(commit='fixture-only',status=''),measurement=measurement,
        distances=distances,error_rates=rates,shots=2,repeats=repeats,backend='scalar' if m4 else 'avx512vl',
        host='fixture-host',platform='Darwin-fixture' if m4 else 'Linux-fixture',machine='arm64' if m4 else 'x86_64',
        methods={m:dict(threads=E.R.METHODS[m]['threads'],execution=E.R.METHODS[m]['execution']) for m in methods},timing='fixture complete decode scope',
        binary=dict(sha256='fixture-binary'),reference=dict(independent=True),seed=1)
    dump(root/'plan.json',plan)
    inputs,calls=[],[]
    for p in rates:
        for d in distances:
            case=f'd{d:02d}-p{p:g}'
            directory=root/'inputs'/case;directory.mkdir(parents=True)
            for name in ('dataset.bin','model.dem','circuit.stim'):
                (directory/name).write_text(case+' '+name)
            inputs.append(dict(case=case,files={str(f.relative_to(root)):E.R.digest(f) for f in directory.iterdir()}))
            for method in methods:
                for repeat in range(1,repeats+1):
                    folder=root/'calls'/case/method/f'{repeat:02d}'/'native';folder.mkdir(parents=True)
                    dump(folder/'correctness.json',dict(status='passed',weight_differences=0,shots=2,observable_differences=0,actual_candidate_witnesses=0))
                    if measurement=='batch':
                        base={'reference':10000,'structured':8000,'predecoded':4000,'component-parallel':3000,
                              'pipelined':2500,'parallel-predecode':2000,'unstructured':500}[method]
                        if policy!=-1:base={0:4000,1:10000,2:6000}[policy]
                        elapsed=2*(base+(repeat-1)*1000)
                        mode='pipe-batch' if E.R.METHODS[method]['execution']=='pipeline' else 'batch'
                        (folder/'measurement.csv').write_text('shots,weight_diff,elapsed_ns,total_threads,execution,mode\n'+
                            f"2,0,{elapsed},{E.R.METHODS[method]['threads']},{E.R.METHODS[method]['execution']},{mode}\n")
                        dump(folder/'measurement_scope.json',dict(mode=mode,internal_instrumentation=False,timed_scope='fixture complete decode scope'))
                    elif measurement=='statistics':
                        residual={0:4,1:20,2:8}[policy]
                        dump(folder/'syndromes.json',dict(complete=True,timed=False,shots=2,mode=policy,completed_shots=0,
                            execution='serial' if serial_policy else 'pipeline',scope='fixture untimed replay',components=[dict(normalized=10,residual=residual//2,pairs=(20-residual)//4,boundaries=0)]*2))
                    else:
                        spec=E.P.LEVELS[level];loads=10000;misses=100 if method=='reference' else 50
                        dump(folder/'pmu.json',dict(mode='pmu',shots=2,events=spec['events'],latency_collected=False,
                            scaled=False,multiplexed=False,pinned_group=True,inherit=False,exclude_kernel=True,exclude_hv=True,
                            time_enabled_ns=10,time_running_ns=10,group_raw=[2,10,10,loads,1,misses,2],scope='fixture exact PMU events',
                            **{spec['prefix']+'_loads':loads,spec['prefix']+'_load_misses':misses,spec['metric']:100*misses/loads}))
                    calls.append(dict(case=case,distance=d,error_rate=p,method=method,repeat=repeat,
                        directory=str(folder.relative_to(root)),files={str(f.relative_to(root)):E.R.digest(f) for f in folder.iterdir()}))
    dump(root/'complete.json',dict(complete=True,plan_sha256=E.R.digest(root/'plan.json'),inputs=inputs,calls=calls))
    return root


def external_fixture(root, native_path):
    source=E.native_run(native_path,'batch','smoke')
    dump(root/'plan.json',dict(profile='smoke',ae_plan_sha256=E.R.digest(native_path/'plan.json')))
    groups=[]
    for case,digest in source['inputs'].items():
        distance=int(case.split('-')[0][1:]);probability=float(case.split('-p')[1])
        folder=root/case/'n2/run';folder.mkdir(parents=True)
        prepared=folder.parent/'input';prepared.mkdir()
        (prepared/'native-input.json').write_text('{}')
        (folder/'oracle.jsonl').write_text('{}\n')
        results=[]
        for index,method in enumerate(E.EXTERNAL):
            with (folder/f'{method}.csv').open('w',newline='') as stream:
                writer=csv.writer(stream);writer.writerow(['repeat','shot','decode_ns','weight','integer_weight','observable'])
                for repeat in range(3):
                    for shot in range(2):writer.writerow([repeat,shot,10000+index*1000,0,0,0])
            (folder/f'{method}.corrections.jsonl').write_text('[]\n[]\n')
            results.append(dict(method=method,shots=2,repeats=3,mean_us_per_shot=10+index,
                timing_scope='fixture preserved native timing',files={name:E.R.digest(folder/name) for name in [f'{method}.csv',f'{method}.corrections.jsonl']}))
        dump(folder/'summary.json',dict(complete=True,configuration=dict(input_provenance=dict(
            source=dict(sha256=digest,distance=distance,error_rate=probability),
            selected_shots=2,repeats=3,files={'native-input.json':E.R.digest(prepared/'native-input.json')})),
            oracle=dict(sha256=E.R.digest(folder/'oracle.jsonl')),results=results))
        groups.append(dict(case=case,shots=2,methods=E.EXTERNAL,summary=str((folder/'summary.json').relative_to(root)),sha256=E.R.digest(folder/'summary.json')))
    dump(root/'summary.json',dict(complete=True))
    (root/'summary.csv').write_text('fixture-only\n')
    dump(root/'complete.json',dict(complete=True,plan_sha256=E.R.digest(root/'plan.json'),
        summary_sha256=E.R.digest(root/'summary.json'),summary_csv_sha256=E.R.digest(root/'summary.csv'),groups=groups))
    return root


class FreshExportTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)

    def test_raw_integer_aggregation_and_official_baseline_role(self):
        path=native_fixture(self.root/'native',policy=-1)
        run=E.native_run(path,'batch','smoke');rows=E.aggregate(run)
        self.assertEqual(rows[9,E.Decimal('.003'),'reference']['mean_us'],11)
        self.assertEqual(rows[9,E.Decimal('.003'),'structured']['repeat_elapsed_ns'],[16000,18000,20000])
        self.assertEqual(E.ALIASES['pymatching'],'reference')
        self.assertNotIn('unstructured',E.CANDIDATES)
        output=self.root/'export'
        self.assertEqual(E.main(['--native-run',str(path),'--profile','smoke','--groups','ablation','--no-render','--output',str(output)]),0)
        self.assertTrue(E.read(output/'complete.json')['complete'])
        self.assertEqual(E.read(output/'canonical.json')['profile'],'smoke')

    def test_changed_measurements_and_missing_statistics_are_rejected(self):
        path=native_fixture(self.root/'native',policy=-1)
        measurement=next(path.glob('calls/*/reference/*/native/measurement.csv'))
        measurement.write_text(measurement.read_text().replace('20000','20001'))
        with self.assertRaisesRegex(ValueError,'Changed'):
            E.native_run(path,'batch','smoke')
        with self.assertRaisesRegex(ValueError,'three policy'):
            E.policy_results({}, {}, 'smoke')

    def test_policy_conservation_and_input_pairing(self):
        timing={m:E.native_run(native_fixture(self.root/f't{m}',policy=m),'batch','smoke') for m in E.POLICIES}
        counts={m:E.native_run(native_fixture(self.root/f'c{m}','statistics',policy=m),'statistics','smoke') for m in E.POLICIES}
        rows=E.policy_results(timing,counts,'smoke')
        degree=next(r for r in rows if r['policy']==0)
        self.assertEqual(degree['mean_us'],5)
        self.assertEqual(degree['mean_residual'],2)
        self.assertEqual(degree['matched_percent'],80)
        counts[0]['inputs']['d09-p0.003']='different'
        with self.assertRaisesRegex(ValueError,'Different saved'):
            E.policy_results(timing,counts,'smoke')

    def test_native_batch_scope_must_be_sealed_and_uninstrumented(self):
        path=native_fixture(self.root/'native',policy=-1)
        scope_path=next(path.glob('calls/*/structured/*/native/measurement_scope.json'))
        scope=E.read(scope_path)
        scope['internal_instrumentation']=True
        dump(scope_path,scope);reseal_native_call(path,scope_path)
        with self.assertRaisesRegex(ValueError,'instrumented'):
            E.native_run(path,'batch','smoke')
        scope['internal_instrumentation']=False;scope['mode']='latency'
        dump(scope_path,scope);reseal_native_call(path,scope_path)
        with self.assertRaisesRegex(ValueError,'scope/execution'):
            E.native_run(path,'batch','smoke')
        complete=E.read(path/'complete.json')
        for call in complete['calls']:call['files'].pop(str(scope_path.relative_to(path)),None)
        dump(path/'complete.json',complete)
        with self.assertRaisesRegex(ValueError,'scope is not sealed'):
            E.native_run(path,'batch','smoke')

    def test_native_correctness_covers_saved_shots_and_observable_ties(self):
        path=native_fixture(self.root/'native',policy=-1)
        correctness_path=next(path.glob('calls/*/structured/*/native/correctness.json'))
        correctness=E.read(correctness_path);correctness['shots']=1
        dump(correctness_path,correctness);reseal_native_call(path,correctness_path)
        with self.assertRaisesRegex(ValueError,'all saved shots'):
            E.native_run(path,'batch','smoke')
        correctness['shots']=2;correctness['observable_differences']=1
        dump(correctness_path,correctness);reseal_native_call(path,correctness_path)
        with self.assertRaisesRegex(ValueError,'correction witnesses'):
            E.native_run(path,'batch','smoke')
        correctness['actual_candidate_witnesses']=1
        dump(correctness_path,correctness);reseal_native_call(path,correctness_path)
        self.assertTrue(E.native_run(path,'batch','smoke')['records'])

    def test_external_counts_and_paper_group_source_are_bound(self):
        native_path=native_fixture(self.root/'native',policy=-1)
        path=external_fixture(self.root/'external',native_path)
        native=E.native_run(native_path,'batch','smoke')
        summary_path=next(path.glob('*/n2/run/summary.json'))
        summary=E.read(summary_path)
        summary['configuration']['input_provenance']['selected_shots']=1
        dump(summary_path,summary);reseal_external_group(path,summary_path)
        with self.assertRaisesRegex(ValueError,'prepared input identity'):
            E.external_run(path,native,'smoke')
        summary['configuration']['input_provenance']['selected_shots']=2
        summary['configuration'].update(profile='paper',build={'source':{'dirty':False,'commit':'fixture-only'}},
            host='fixture-host',platform='Linux-fixture',machine='x86_64')
        summary['oracle']['revision']='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
        plan=E.read(path/'plan.json');plan.update(profile='paper',source={'dirty':False,'commit':'fixture-only'})
        dump(path/'plan.json',plan)
        complete=E.read(path/'complete.json');complete['plan_sha256']=E.R.digest(path/'plan.json')
        dump(path/'complete.json',complete)
        for changes in ({'profile':'smoke'}, {'build':{'source':{'dirty':True,'commit':'fixture-only'}}},
                        {'build':{'source':{'dirty':False,'commit':'another-commit'}}}):
            candidate=json.loads(json.dumps(summary));candidate['configuration'].update(changes)
            dump(summary_path,candidate);reseal_external_group(path,summary_path)
            with self.assertRaisesRegex(ValueError,'build/profile/oracle'):
                E.external_run(path,native,'paper')
        summary['oracle']['revision']='different-official-version'
        dump(summary_path,summary);reseal_external_group(path,summary_path)
        with self.assertRaisesRegex(ValueError,'build/profile/oracle'):
            E.external_run(path,native,'paper')

    def test_all_current_macros_update_from_complete_fixture_matrices(self):
        native=E.native_run(native_fixture(self.root/'native',policy=-1,full=True),'batch','smoke')
        m4=E.native_run(native_fixture(self.root/'m4',policy=-1,full=True,m4=True),'batch','smoke')
        timing={m:E.native_run(native_fixture(self.root/f't{m}',policy=m,full=True),'batch','smoke') for m in E.POLICIES}
        counts={m:E.native_run(native_fixture(self.root/f'c{m}','statistics',policy=m,full=True),'statistics','smoke') for m in E.POLICIES}
        policies=E.policy_results(timing,counts,'smoke')
        pmu={level:E.native_run(native_fixture(self.root/level,'pmu',policy=-1,full=True,level=level),'pmu','smoke') for level in ('L2','L3')}
        performance,predecoder,audit=E.paper_macros(native,pmu,policies,m4)
        self.assertIn('\\DefinePerfValue{m4pro.decoder-comparison.p0p05.best.vs-pymatching.max-speedup}{3.67}',performance)
        self.assertIn('\\DefinePerfValue{m4pro.realtime.pymatching.num-supported-settings}{18}',performance)
        self.assertIn('\\DefinePerfValue{xeon.ablation.d33.p0p3.pymatching.L2miss}{1.000}',performance)
        self.assertIn('\\DefinePerfValue{xeon.predecoder-comparison.d33.p0p3.degree-ordered.matched-percent}{80.00}',predecoder)
        self.assertEqual(predecoder.count('\\DefinePerfValue'),58)
        self.assertEqual(len(audit['performance_values']),len(__import__('migrate_paper_numbers').declarations(performance)))
        external=external_fixture(self.root/'external',self.root/'native')
        output=self.root/'all-export'
        command=['--profile','smoke','--native-run',str(self.root/'native'),'--external-run',str(external),
                 '--m4-run',str(self.root/'m4'),'--l2-run',str(self.root/'L2'),'--l3-run',str(self.root/'L3'),
                 '--policy-timing',*[f'{mode}={self.root}/t{mode}' for mode in E.POLICIES],
                 '--policy-counts',*[f'{mode}={self.root}/c{mode}' for mode in E.POLICIES],
                 '--no-render','--output',str(output)]
        self.assertEqual(E.main(command),0)
        self.assertTrue(E.read(output/'complete.json')['complete'])
        self.assertEqual(len(E.read(output/'canonical.json')['external']),105)

    def test_paper_profile_does_not_accept_smoke_receipts(self):
        path=native_fixture(self.root/'native',policy=-1)
        with self.assertRaisesRegex(ValueError,'formal'):
            E.native_run(path,'batch','paper')

    def test_serial_policies_are_explicit_and_cannot_mix_pipeline_counts(self):
        timing={m:E.native_run(native_fixture(self.root/f't{m}',policy=m,full=True,serial_policy=True),'batch','smoke') for m in E.POLICIES}
        counts={m:E.native_run(native_fixture(self.root/f'c{m}','statistics',policy=m,full=True,serial_policy=True),'statistics','smoke') for m in E.POLICIES}
        rows=E.policy_results(timing,counts,'smoke')
        self.assertTrue(all(r['execution']=='serial' and r['threads']==1 for r in rows))
        self.assertIn('Fresh serial table, 1 thread(s)',E.predecoder_macros(rows))
        counts[0]=E.native_run(native_fixture(self.root/'pipeline','statistics',policy=0,full=True),'statistics','smoke')
        with self.assertRaisesRegex(ValueError,'consistent serial or pipeline'):
            E.policy_results(timing,counts,'smoke')

    def test_xeon_macro_subset_does_not_require_or_fill_missing_platform_or_pmu(self):
        path=native_fixture(self.root/'native',policy=-1,full=True)
        native=E.native_run(path,'batch','smoke')
        performance,predecoder,audit=E.paper_macros(native,{},[],None,xeon_only=True)
        keys={item.key for item in __import__('migrate_paper_numbers').declarations(performance)}
        self.assertTrue(keys)
        self.assertFalse(any(k.startswith('m4pro.') or k.endswith(('L2miss','L3miss')) for k in keys))
        self.assertEqual(set(audit['performance_values']),keys)
        self.assertTrue(audit['omitted_performance_keys'])
        self.assertEqual(predecoder,'')
        output=self.root/'xeon-export'
        self.assertEqual(E.main(['--native-run',str(path),'--profile','smoke','--groups','xeon-macros','--no-render','--output',str(output)]),0)
        self.assertFalse((output/'predecoder_numbers.tex').exists())
        self.assertEqual(E.read(output/'macro-audit.json')['scope'],'xeon-only subset')
        l2=E.native_run(native_fixture(self.root/'l2','pmu',policy=-1,full=True,level='L2'),'pmu','smoke')
        performance,_,audit=E.paper_macros(native,{'L2':l2},[],None,xeon_only=True)
        keys={item.key for item in __import__('migrate_paper_numbers').declarations(performance)}
        self.assertTrue(any(k.endswith('L2miss') for k in keys))
        self.assertFalse(any(k.endswith('L3miss') for k in keys))
        # Only the d33/p=.003 PMU point is required by the actual article.
        l2['records']=[r for r in l2['records'] if r['distance']==33 and r['error_rate']==.003]
        l2['inputs']={k:v for k,v in l2['inputs'].items() if k=='d33-p0.003'}
        performance,_,_=E.paper_macros(native,{'L2':l2},[],None,xeon_only=True)
        self.assertIn('pymatching.L2miss}{1.000}',performance)

    def test_small_smoke_xeon_export_never_claims_full_matrix_macros(self):
        path=native_fixture(self.root/'small',policy=-1)
        output=self.root/'small-export'
        self.assertEqual(E.main(['--native-run',str(path),'--profile','smoke','--groups','xeon-macros','--no-render','--output',str(output)]),0)
        audit=E.read(output/'macro-audit.json')
        self.assertFalse(audit['native_matrix_complete'])
        self.assertEqual(audit['performance_values'],{})
        self.assertTrue(audit['omitted_performance_keys'])

    def test_real_simple_policy_is_distinct_from_legacy_coordinate_and_preserves_macro_keys(self):
        timing={m:E.native_run(native_fixture(self.root/f't{m}',policy=m,full=True,serial_policy=True),'batch','smoke') for m in E.POLICIES}
        counts={m:E.native_run(native_fixture(self.root/f'c{m}','statistics',policy=m,full=True,serial_policy=True),'statistics','smoke') for m in E.POLICIES}
        legacy=E.policy_results(timing,counts,'smoke')
        self.assertTrue(all(r['strategy']=='coordinate' and r['physical_policy']==2 for r in legacy if r['policy']==2))
        for row in counts[2]['records']:row['policy']=3
        timing[2]['plan']['binary']['build_record']={'configuration':{'BB_TAILRESET_PREDECODE_MODE':'3'}}
        actual=E.policy_results(timing,counts,'smoke')
        self.assertTrue(all(r['strategy']=='simple-equal-rate' and r['physical_policy']==3 for r in actual if r['policy']==2))
        macros=E.predecoder_macros(actual)
        self.assertIn('third policy=simple-equal-rate',macros)
        self.assertIn('.coordinate.mean-time-us}',macros)
        timing[2]['plan']['binary']['build_record']['configuration']['BB_TAILRESET_PREDECODE_MODE']='2'
        with self.assertRaisesRegex(ValueError,'Timing binary policy differs'):
            E.policy_results(timing,counts,'smoke')

    def test_local_workflow_smoke_exports_serial_simple_single_point_pmu_without_m4(self):
        native=native_fixture(self.root/'native',policy=-1)
        external=external_fixture(self.root/'external',native)
        for mode in E.POLICIES:
            native_fixture(self.root/f't{mode}',policy=mode,serial_policy=True)
            native_fixture(self.root/f'c{mode}','statistics',policy=mode,serial_policy=True)
        counts=self.root/'c2'
        path=next(counts.glob('calls/*/*/*/native/syndromes.json'))
        value=E.read(path);value['mode']=3
        dump(path,value);reseal_native_call(counts,path)
        timing=self.root/'t2';plan=E.read(timing/'plan.json')
        plan['binary']['build_record']={'configuration':{'BB_TAILRESET_PREDECODE_MODE':'3'}}
        dump(timing/'plan.json',plan)
        done=E.read(timing/'complete.json');done['plan_sha256']=E.R.digest(timing/'plan.json');dump(timing/'complete.json',done)
        for level in ('L2','L3'):native_fixture(self.root/level,'pmu',policy=-1,level=level)
        output=self.root/'workflow-export'
        command=['--profile','smoke','--native-run',str(native),'--external-run',str(external),
            '--policy-timing',*[f'{m}={self.root}/t{m}' for m in E.POLICIES],
            '--policy-counts',*[f'{m}={self.root}/c{m}' for m in E.POLICIES],
            '--l2-run',str(self.root/'L2'),'--l3-run',str(self.root/'L3'),
            '--groups','ablation','comparison','predecoder','xeon-macros','--no-render','--output',str(output)]
        self.assertEqual(E.main(command),0)
        self.assertTrue(E.read(output/'complete.json')['complete'])
        self.assertEqual(E.read(output/'macro-audit.json')['performance_values'],{})
        rows=E.read(output/'canonical.json')['predecoder']
        self.assertEqual(next(r for r in rows if r['policy']==2)['physical_policy'],3)
        self.assertFalse((output/'predecoder_numbers.tex').exists())


if __name__=='__main__':unittest.main()
