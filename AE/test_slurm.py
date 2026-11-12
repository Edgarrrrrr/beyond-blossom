"""Scheduling/controller tests with mocked Slurm; never submit or decode."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from AE import slurm as S


class SlurmWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='bb-ae-slurm-test-')
        self.root = Path(self.tmp.name)
        self.work = self.root / 'work'

    def tearDown(self):
        self.tmp.cleanup()

    def plan(self, *extra):
        args = S.parser().parse_args(['prepare', '--work', str(self.work), '--nodes', 'node01,node02,node03,node04,node05,node06',
                                     '--partition', 'batch', '--cpus-per-task', '52', *extra])
        with patch.object(S.local, 'identity', return_value={'commit':'fixed','status':''}), contextlib.redirect_stdout(io.StringIO()):
            return S.prepare(args)

    def test_prepare_is_inert_and_assigns_each_case_once(self):
        with patch.object(S.subprocess, 'run') as execute:
            plan = self.plan()
        execute.assert_not_called()
        cases = [c for shard in plan['shards'] for c in shard['cases']]
        self.assertEqual(len(cases), 21)
        self.assertEqual(len({c['name'] for c in cases}), 21)
        self.assertEqual({(c['distance'], c['error_rate']) for c in cases}, {(d,p) for d in S.local.DISTANCES for p in S.local.ERROR_RATES})
        self.assertTrue(all(s['cases'] for s in plan['shards']))
        self.assertEqual(S.assign_cases([(d,p) for d in S.local.DISTANCES for p in S.local.ERROR_RATES],6), [s['cases'] for s in plan['shards']])
        self.assertEqual(plan['build_command'][2], 'build')
        self.assertIn('--source', plan['build_command'])
        self.assertNotIn('source-2411603', str(plan))

    def test_smoke_is_one_real_case_and_worker(self):
        plan = self.plan('--profile','smoke')
        self.assertEqual(len(plan['shards']), 1)
        self.assertEqual(plan['shards'][0]['cases'], [{'distance':9,'error_rate':.003,'name':'d9-p0.003'}])

    def test_sbatch_resources_and_export_stay_in_requested_nodes(self):
        plan = self.plan()
        worker = S.sbatch_command(plan, plan['shards'][1])
        self.assertIn('--cpus-per-task=52', worker)
        self.assertIn('--exclusive', worker)
        self.assertIn('--nodelist=node02', worker)
        export = S.sbatch_command(plan, dependencies=['101','102'])
        self.assertIn('--dependency=afterok:101:102', export)
        self.assertIn('--nodelist=node01', export)
        self.assertIn('--cpus-per-task=1', export)

    def test_build_precedes_submissions_then_auto_export_depends_on_all_workers(self):
        plan = self.plan()
        events = []
        def run(command, **kwargs):
            events.append(command)
            return subprocess.CompletedProcess(command, 0, str(100+len(events))+'\n', '')
        def build(plan):
            events.append(['build-complete'])
            manifest=Path(plan['builds_manifest']);manifest.parent.mkdir(parents=True)
            S.local.save(manifest, {'complete':True})
            return S.local.digest(manifest)
        with patch.object(S, 'ensure_builds', side_effect=build), patch.object(S.subprocess,'run',side_effect=run), contextlib.redirect_stdout(io.StringIO()):
            S.submit(plan)
        self.assertEqual(events[0], ['build-complete'])
        self.assertEqual(len(events), 8)
        self.assertIn('--dependency=afterok:102:103:104:105:106:107',events[-1])
        with self.assertRaisesRegex(ValueError,'already submitted'):
            S.submit(plan)

    def test_resume_cancels_only_stale_export_after_workers_terminal(self):
        plan = self.plan('--profile','smoke')
        S.local.save(self.work/'submissions.json', {'workers':{'0':[{'job_id':'101'}]}, 'exports':[{'job_id':'102'}]})
        commands=[]
        def run(command, **kwargs):
            commands.append(command)
            if command[0]=='squeue':
                return subprocess.CompletedProcess(command,0,'102|PENDING|DependencyNeverSatisfied\n' if '--jobs=102' in command else '', '')
            return subprocess.CompletedProcess(command,0,'201\n' if command[0]=='sbatch' else '', '')
        with patch.object(S, 'ensure_builds', return_value='fixed'), patch.object(S.subprocess,'run',side_effect=run), contextlib.redirect_stdout(io.StringIO()):
            S.submit(plan, resume=True)
        self.assertIn(['scancel','102'], commands)
        self.assertTrue(S.local.read(self.work/'submissions.json')['exports'][0]['cancelled_utc'])

    def test_resume_never_cancels_active_worker(self):
        plan=self.plan('--profile','smoke')
        S.local.save(self.work/'submissions.json', {'workers':{'0':[{'job_id':'101'}]},'exports':[{'job_id':'102'}]})
        with patch.object(S.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'101|RUNNING|None\n','')) as run:
            with self.assertRaisesRegex(ValueError,'workers are still active'):
                S.submit(plan,resume=True)
        self.assertEqual(run.call_count,1)

    def test_unspecified_node_resume_returns_to_actual_placement(self):
        plan=self.plan('--profile','smoke')
        plan['shards'][0]['node']=None
        location=self.work/'workers/0'; location.mkdir(parents=True)
        S.local.save(location/'placement.json',{'node':'original-node'})
        S.local.save(self.work/'submissions.json',{'workers':{'0':[{'job_id':'100'}]},'exports':[]})
        commands=[]
        def run(command,**kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command,0,'' if command[0]=='squeue' else '200\n','')
        with patch.object(S,'ensure_builds',return_value='fixed'),patch.object(S.subprocess,'run',side_effect=run),contextlib.redirect_stdout(io.StringIO()):
            S.submit(plan,resume=True)
        self.assertIn('--nodelist=original-node',next(c for c in commands if c[0]=='sbatch'))
        self.assertIsNone(plan['shards'][0]['node'])

    def test_worker_requires_slurm_before_any_native_call(self):
        plan=self.plan('--profile','smoke')
        with patch.dict(os.environ,{},clear=True), patch.object(S.subprocess,'run') as execute:
            with self.assertRaisesRegex(ValueError,'inside a Slurm'):
                S.worker(plan,0)
        execute.assert_not_called()

    def completed_fixture(self):
        plan=self.plan('--profile','smoke')
        manifest=Path(plan['builds_manifest']);manifest.parent.mkdir(parents=True)
        S.local.save(manifest,{'complete':True})
        frozen={'plan_sha256':S.local.digest(self.work/'plan.json'),'builds_manifest_sha256':S.local.digest(manifest),
                'workers':{},'exports':[]}
        S.local.save(self.work/'submissions.json',frozen)
        case=plan['shards'][0]['cases'][0]
        directory=self.work/'cases'/case['name'];directory.mkdir(parents=True)
        config={'source_identity':plan['source_identity'],'distances':[case['distance']], 'error_rates':[case['error_rate']],
                'backend':plan['backend'],'placement':{'host':'host01'},'cpus':list(range(8))}
        S.local.save(directory/'config.json',config)
        S.local.save(directory/'complete.json',{'complete':True,'config_sha256':S.local.digest(directory/'config.json'),
            'bundle':{'native_run':'/native','external_run':'/external','policy_timing':{},'policy_counts':{}}})
        worker=self.work/'workers/0';worker.mkdir(parents=True)
        done={'complete':True,'host':'host01','node':'node01','cpus':list(range(8)),
              'plan_sha256':frozen['plan_sha256'],'builds_manifest_sha256':frozen['builds_manifest_sha256'],
              'cases':[{'name':case['name'],'complete':str(directory/'complete.json'),'sha256':S.local.digest(directory/'complete.json')}]}
        S.local.save(worker/'complete.json',done)
        return plan

    def test_submitted_plan_or_build_cannot_be_relabelled_on_resume(self):
        plan=self.completed_fixture()
        path=self.work/'plan.json';path.write_text(path.read_text()+' ')
        with self.assertRaisesRegex(ValueError,'plan changed'),patch.object(S.subprocess,'run') as execute:
            S.submit(plan,resume=True)
        execute.assert_not_called()

    def test_submitted_build_hash_is_checked_before_worker(self):
        plan=self.completed_fixture()
        Path(plan['builds_manifest']).write_text('{}')
        with patch.dict(os.environ,{'SLURM_JOB_ID':'1'}),patch.object(S.subprocess,'run') as execute:
            with self.assertRaisesRegex(ValueError,'build manifest changed'):
                S.worker(plan,0)
        execute.assert_not_called()

    def test_export_rejects_same_length_wrong_case_assignment(self):
        plan=self.completed_fixture()
        path=self.work/'workers/0/complete.json';done=S.local.read(path)
        done['cases'][0]['name']='d13-p0.003';S.local.save(path,done)
        with self.assertRaisesRegex(ValueError,'assigned cases'):
            S.export(plan)

    def test_export_reuse_binds_shards_and_checks_artifacts(self):
        plan=self.completed_fixture()
        def run(command,**kwargs):
            output=Path(command[command.index('--output')+1]);output.mkdir()
            artifact=output/'table.csv';artifact.write_text('measured')
            S.local.save(output/'complete.json',{'complete':True,
                'sources':{'shards_manifest':{'manifest':{'sha256':S.local.digest(self.work/'shards.json')}}},
                'files':{'table.csv':S.local.digest(artifact)}})
        with patch.object(S.subprocess,'run',side_effect=run) as execute,contextlib.redirect_stdout(io.StringIO()):
            S.export(plan);S.export(plan)
        self.assertEqual(execute.call_count,1)
        (self.work/'exports/table.csv').write_text('changed')
        with self.assertRaisesRegex(ValueError,'artifact changed'):
            S.export(plan)

    def test_export_does_not_reuse_success_from_other_shards(self):
        plan=self.completed_fixture()
        out=self.work/'exports';out.mkdir()
        S.local.save(out/'complete.json',{'complete':True,'sources':{},'files':{}})
        with self.assertRaisesRegex(ValueError,'another shard manifest'):
            S.export(plan)
        self.assertFalse((self.work/'complete.json').exists())

    def test_build_resume_does_not_accidentally_collect(self):
        plan=self.plan('--profile','smoke')
        build=self.work/'shared-build'; build.mkdir()
        S.local.save(build/'config.json',{})
        def run(command,**kwargs):
            self.assertEqual(command[2],'build-resume')
            self.assertEqual(command[3:],['--work',str(build)])
            S.local.save(build/'builds.json',{'complete':True,'source_identity':plan['source_identity']})
        with patch.object(S.subprocess,'run',side_effect=run):
            S.ensure_builds(plan)


if __name__=='__main__':
    unittest.main()
