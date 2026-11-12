"""Small orchestration tests; no compiler, native decoder, PMU or plot is run."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('ae_local_run', Path(__file__).with_name('run.py'))
R = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(R)


class LocalWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='bb-ae-workflow-test-')
        self.root = Path(self.temporary.name)
        self.config = {'source': str(self.root / 'source'), 'work': str(self.root / 'work'),
            'source_identity': {'commit': 'fixed', 'status': ''}, 'profile': 'paper', 'jobs': 2,
            'offline': True, 'python': {'executable': '/mock/python'}, 'cpus': list(range(8)),
            'backend': 'avx512vl', 'toolchain': 'nightly-fixed', 'shots': 1000000, 'repeats': 3,
            'external_shots': 10000, 'render': False, 'excluded': ['Apple M4 hardware acquisition'],
            'pmu_levels': [2, 3], 'dependencies': {
                'stim_source': '/deps/stim', 'stim_archive': '/deps/libstim.a', 'googletest_source': '/deps/gtest',
                'upstream_repository': '/deps/upstream', 'crate_cache': '/deps/crates', 'reference_build': None,
                'reference_record': None, 'native_build': None, 'external_build': None}}
        self.workflow = R.Workflow(self.config)
        R.save(self.workflow.root / 'config.json', self.config)

    def tearDown(self):
        self.temporary.cleanup()

    def test_cpu_discovery_uses_largest_socket_and_one_sibling(self):
        topology = self.root / 'cpus'
        for cpu, socket, core in [(2, 0, 0), (4, 0, 1), (6, 0, 1), (3, 1, 0), (5, 1, 1), (7, 1, 2)]:
            p = topology / f'cpu{cpu}/topology'; p.mkdir(parents=True)
            (p / 'physical_package_id').write_text(str(socket)); (p / 'core_id').write_text(str(core))
        with patch.object(R.platform, 'system', return_value='Linux'):
            self.assertEqual(R.physical_cpus([2, 3, 4, 5, 6, 7], topology), [3, 5, 7])

    def test_dependency_paths_recovered_from_reference_command(self):
        root = self.root / 'reference'; root.mkdir()
        stim = self.root / 'stim'; stim.mkdir()
        lib = self.root / 'libstim.a'; lib.write_bytes(b'archive')
        R.save(root / 'configure-command.json', ['cmake', '-DFETCHCONTENT_SOURCE_DIR_STIM=' + str(stim),
            '-DCACHED_STIM_ARCHIVE=' + str(lib)])
        self.assertEqual(R.dependency_hints([root]), {'stim_source': str(stim), 'stim_archive': str(lib)})

    def test_completed_stage_reused_without_execution(self):
        def run(argv, **kwargs):
            output = Path(argv[-1]); output.mkdir(parents=True)
            R.save(output / 'complete.json', {'complete': True})
        with patch.object(R.subprocess, 'run', side_effect=run) as execute, contextlib.redirect_stdout(io.StringIO()):
            first = self.workflow.stage('matrix', 'runs', lambda out: [['mock', str(out)]])
            second = self.workflow.stage('matrix', 'runs', lambda out: [['must-not-run', str(out)]])
            self.assertEqual(first, second); self.assertEqual(execute.call_count, 1)
        R.save(first / 'complete.json', {'complete': False})
        with self.assertRaisesRegex(ValueError, 'receipt changed'):
            self.workflow.stage('matrix', 'runs', lambda out: [])

    def test_failure_preserved_and_resume_uses_new_attempt(self):
        def fail(argv, **kwargs):
            out = Path(argv[-1]); out.mkdir(parents=True); (out / 'partial.csv').write_text('evidence')
            raise subprocess.CalledProcessError(1, argv)
        def pass_run(argv, **kwargs):
            out = Path(argv[-1]); out.mkdir(parents=True); R.save(out / 'complete.json', {'complete': True})
        with patch.object(R.subprocess, 'run', side_effect=fail), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                self.workflow.stage('matrix', 'runs', lambda out: [['mock', str(out)]])
        with patch.object(R.subprocess, 'run', side_effect=pass_run), contextlib.redirect_stdout(io.StringIO()):
            path = R.Workflow(self.config).stage('matrix', 'runs', lambda out: [['mock', str(out)]])
        self.assertEqual(path.name, 'matrix-attempt-2')
        self.assertEqual((self.workflow.root / 'runs/matrix/partial.csv').read_text(), 'evidence')

    def test_only_counter_capability_failure_is_optional(self):
        def fail(argv, **kwargs):
            kwargs['stdout'].write('perf_event_open cache-read counter: Operation not permitted\n')
            kwargs['stdout'].flush()
            raise subprocess.CalledProcessError(1, argv)
        with patch.object(R.subprocess, 'run', side_effect=fail), contextlib.redirect_stdout(io.StringIO()):
            result = self.workflow.stage('probe', 'capabilities', lambda out: [['mock']], optional_pmu=True)
            self.assertIsNone(result)
        def correctness_failure(argv, **kwargs):
            kwargs['stdout'].write('Weight validation failed\n'); kwargs['stdout'].flush()
            raise subprocess.CalledProcessError(1, argv)
        with patch.object(R.subprocess, 'run', side_effect=correctness_failure), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                self.workflow.stage('bad-correctness', 'capabilities', lambda out: [['mock']], optional_pmu=True)

    def test_collect_never_starts_a_missing_build(self):
        with patch.object(R.subprocess, 'run') as execute:
            with self.assertRaisesRegex(ValueError, 'has not been built'):
                self.workflow.stage('native', 'builds', lambda out: [['compiler']], build_allowed=False)
            execute.assert_not_called()

    def test_full_plan_builds_first_then_serial_policies_and_fresh_export(self):
        stages = []
        def stage(name, category, commands, *args, **kwargs):
            output = self.workflow.root / category / name
            stages.append((name, category, commands(output)))
            return output
        with patch.object(self.workflow, 'stage', side_effect=stage), contextlib.redirect_stdout(io.StringIO()):
            builds = self.workflow.builds()
            build_count = len(stages)
            self.workflow.collect(*builds)
        self.assertEqual(build_count, 10)  # 2 references, 7 candidate variants, 1 external build.
        self.assertTrue(all(s[1] == 'builds' for s in stages[:build_count]))
        self.assertTrue(all(s[1] != 'builds' for s in stages[build_count:]))
        commands = {name: commands[0] for name, category, commands in stages}
        batch = commands['native-batch']
        self.assertEqual(batch[batch.index('--methods') + 1], R.METHODS)
        self.assertNotIn('unstructured', R.METHODS)
        for p in range(3):
            for role in ['timing', 'counts']:
                command = commands[f'policy-{p}-{role}']
                self.assertEqual(command[command.index('--methods') + 1], 'predecoded')
                self.assertIn('--input-run', command)
                self.assertIn('--link-inputs', command)
            command = commands[f'policy-{p}-counts']
            self.assertEqual(command[command.index('--measurement') + 1], 'statistics')
            for build_kind in ['l3', 'statistics']:
                command = commands[f'policy-{p}-{build_kind}']
                self.assertEqual(command[command.index('--predecode-mode') + 1], str([0, 1, 3][p]))
        self.assertIn('xeon-macros', commands['figures'])
        self.assertNotIn('--m4-run', commands['figures'])
        self.assertEqual(commands['external'][commands['external'].index('--limit') + 1], '10000')
        for level in [2, 3]:
            command = commands[f'pmu-l{level}']
            self.assertEqual(command[command.index('--distances')+1], '33')
            self.assertEqual(command[command.index('--error-rates')+1], '.003')

    def test_unavailable_pmu_does_not_hide_other_groups(self):
        stages = {}
        def stage(name, category, commands, *args, **kwargs):
            out = self.workflow.root / category / name; stages[name] = commands(out)
            return None if name == 'pmu-l2-probe' else out
        with patch.object(self.workflow, 'stage', side_effect=stage), contextlib.redirect_stdout(io.StringIO()):
            self.workflow.collect({f'policy-{p}-{kind}': Path('/build') for p in range(3)
                for kind in ['l2', 'l3', 'statistics']}, {2: Path('/ref2'), 3: Path('/ref3')}, Path('/external'))
        self.assertNotIn('pmu-l2', stages); self.assertIn('pmu-l3', stages)
        self.assertIn('xeon-macros', stages['figures'][0])
        self.assertIn('comparison', stages['figures'][0]); self.assertIn('predecoder', stages['figures'][0])
        self.assertEqual(R.read(self.workflow.root / 'complete.json')['pmu_levels_unavailable'], [2])

    def test_existing_build_from_another_commit_not_relabelled(self):
        root = self.root / 'old-build'; root.mkdir()
        R.save(root / 'build.json', {'complete': True, 'source': {'commit': 'old', 'status': ''}})
        before = (root / 'build.json').read_bytes()
        with self.assertRaisesRegex(ValueError, 'another or dirty source'):
            self.workflow.external_build(root, 'candidate', 'build.json')
        self.assertEqual((root / 'build.json').read_bytes(), before)
        self.config['dependencies']['native_build'] = str(root)
        with patch.object(self.workflow, 'stage') as stage:
            with self.assertRaisesRegex(ValueError, 'another or dirty source'):
                self.workflow.builds()
            stage.assert_not_called()

    def test_plan_has_no_build_or_workflow_execution(self):
        with patch.object(R, 'configuration', return_value=self.config), patch.object(R, 'Workflow') as workflow,\
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(R.main(['plan', '--work', str(self.root / 'plan-only')]), 0)
            workflow.assert_not_called()

    def test_smoke_uses_small_external_inputs_and_records_actual_count(self):
        args = R.parser().parse_args(['plan', '--work', str(self.root/'smoke'), '--source', str(R.HERE.parent), '--profile', 'smoke'])
        with patch.object(R, 'select_cpus', return_value=list(range(8))),\
             patch.object(R, 'python_environment', return_value={'executable':'mock','available':True}),\
             patch.object(R, 'identity', return_value={'commit':'mock','status':''}):
            c = R.configuration(args)
        self.assertEqual(c['shots'], 1025); self.assertEqual(c['external_shots'], 32)


if __name__ == '__main__':
    unittest.main()
