"""Control-flow tests use fake native artifacts; no native timing is started."""
import csv
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch

import pytest


PATH = Path(__file__).resolve().parent / 'run_structured.py'
SPEC = importlib.util.spec_from_file_location('bb_structured_regression', PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def setup_case(tmp_path, *, repeats=2, distances='3,5'):
    build = tmp_path / 'build'
    build.mkdir()
    records = []
    for variant in ('source', 'candidate'):
        binary = build / variant
        binary.write_text(variant)
        records.append(dict(variant=variant, binary=str(binary), binary_sha256=runner.ae.digest(binary),
                            lto=False, flags_make=str(build / 'absent-flags.make'), flags_make_sha256='unused'))
    (build / 'build.json').write_text(json.dumps(dict(kind='pure_timing', builds=records)))
    reference = tmp_path / 'reference'
    reference.write_text('independent-reference')
    receipt = tmp_path / 'reference.json'
    receipt.write_text(json.dumps(dict(complete=True, upstream_revision=runner.REFERENCE_REVISION,
                                       binary=dict(sha256=runner.ae.digest(reference)))))
    return runner.make_parser().parse_args([
        '--build', str(build), '--reference-binary', str(reference), '--reference-record', str(receipt),
        '--output', str(tmp_path / 'output'), '--cpus', '0,2,4,6', '--distances', distances,
        '--error-rates', '0.003', '--methods', 'structured,pipelined', '--shots', '8', '--repeats', str(repeats)])


class FakeNative:
    def __init__(self, fail_weight=False, change_corpus=False, change_binary=False, fail_variant=None):
        self.commands = []
        self.fail_weight = fail_weight
        self.change_corpus = change_corpus
        self.change_binary = change_binary
        self.fail_variant = fail_variant

    def __call__(self, command, directory):
        self.commands.append(command)
        options = dict(zip(command[1::2], command[2::2]))
        directory.mkdir(parents=True)
        (directory / 'command.json').write_text(json.dumps(command))
        native = directory / 'native'
        native.mkdir()
        (native / 'process.log').write_text('mock only; no process executed\n')
        shots = int(options['--shots'])
        dataset = Path(options['--dataset'])
        if options['--mode'] == 'prepare':
            probability_bits = struct.unpack('<Q', struct.pack('<d', float(options['--error-rate'])))[0]
            header = [0x5354454E43494C31, 2, int(options['--distance']), probability_bits,
                      shots, 4, 0, 11, 22, 33, struct.unpack('<Q', struct.pack('<d', 1.0))[0], int(options['--stream-id'])]
            dataset.write_bytes(struct.pack('<12Q', *header))
            (native / 'model.dem').write_text('detector(0, 0, 0) D0\n')
            (native / 'circuit.stim').write_text('R 0\n')
            return native
        variant = Path(command[0]).name
        elapsed = shots * {'reference': 1200, 'source': 1000, 'candidate': 800}[variant]
        (native / 'correctness.json').write_text(json.dumps(dict(status='passed', shots=shots,
            weight_differences=int(self.fail_weight or self.fail_variant == variant),
            observable_differences=0, actual_candidate_witnesses=0)))
        row = dict(mode=options['--mode'], shots=shots, elapsed_ns=elapsed, execution=options['--execution'],
                   total_threads=len(options['--cpus'].split(',')), weight_diff=0, observable_diff=0,
                   syndrome_hash=11, reference_result_hash=22)
        with (native / 'measurement.csv').open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        (native / 'warmup.json').write_text(json.dumps(dict(requested_seconds=2, actual_elapsed_ns=2000000000, shots=100)))
        (native / 'measurement_scope.json').write_text(json.dumps(dict(mode=options['--mode'], internal_instrumentation=False)))
        if self.change_corpus:
            dataset.write_bytes(dataset.read_bytes() + b'changed')
        if self.change_binary:
            Path(command[0]).write_text('changed frozen binary')
        return native


def execute(args, fake):
    with patch.object(runner.ae, 'invoke', fake), patch.object(runner, 'validate_cpus'), \
            patch.object(runner.ae, 'source_identity', return_value=dict(commit='development-snapshot', status=' M source.cc')):
        return runner.collect(args)


def test_saved_once_and_adjacent_variant_rotation(tmp_path):
    args = setup_case(tmp_path, repeats=3)
    fake = FakeNative()
    result = execute(args, fake)
    commands = [dict(zip(c[1::2], c[2::2])) for c in fake.commands]
    assert sum(c['--mode'] == 'prepare' for c in commands) == 2
    assert len(commands) == 2 + 2 * 3 * (2 * 2 + 1)
    for offset in (0, 16):
        selected = fake.commands[offset + 1:offset + 16]
        expected = [
            ['reference', 'source', 'candidate', 'source', 'candidate'],
            ['candidate', 'source', 'reference', 'candidate', 'source'],
            ['source', 'candidate', 'source', 'candidate', 'reference'],
        ]
        for repeat in range(3):
            batch = selected[repeat * 5:(repeat + 1) * 5]
            assert [Path(c[0]).name for c in batch] == expected[repeat]
            paired = [dict(zip(c[1::2], c[2::2])) for c in batch if Path(c[0]).name != 'reference']
            assert [c['--mode'] for c in paired] == (['pipe-batch'] * 2 + ['batch'] * 2 if repeat == 1 else ['batch'] * 2 + ['pipe-batch'] * 2)
            official, = [dict(zip(c[1::2], c[2::2])) for c in batch if Path(c[0]).name == 'reference']
            assert {name: official[name] for name in ['--backend', '--execution', '--predecode', '--mode', '--cpus']} == {
                '--backend': 'generic', '--execution': 'serial', '--predecode': '0', '--mode': 'batch', '--cpus': '0'}
        assert len({dict(zip(c[1::2], c[2::2]))['--dataset'] for c in selected}) == 1
        assert all(dict(zip(c[1::2], c[2::2]))['--warmup-seconds'] == '2' for c in selected)
        assert all(dict(zip(c[1::2], c[2::2]))['--block-size'] == '1024' for c in selected)
        assert all(dict(zip(c[1::2], c[2::2]))['--repeats'] == '1' for c in selected)
    assert len(result['rows']) == 4
    assert all(row['speedup_source_over_candidate'] == 1.25 for row in result['rows'])
    assert all(row['speedup_official_over_candidate'] == 1.5 for row in result['rows'])
    assert all(row['official_median_ns'] == 1200 and row['official_paired_speedup_min'] == 1.5 for row in result['rows'])
    assert all(row['independent_shots'] == 8 and row['repeats'] == 3 for row in result['rows'])
    assert result['official_revision'] == runner.REFERENCE_REVISION
    plan = json.loads((args.output / 'plan.json').read_text())
    assert plan['reference']['binary']['sha256'] == runner.ae.digest(args.reference_binary)
    assert plan['official_method'] == runner.ae.METHODS['reference']
    assert runner.summarize(args.output) == result


def test_correctness_failure_prevents_successful_summary(tmp_path):
    args = setup_case(tmp_path, repeats=1, distances='3')
    with pytest.raises(ValueError, match='correctness failed'):
        execute(args, FakeNative(fail_weight=True))
    assert (args.output / 'failure.json').is_file()
    assert not (args.output / 'complete.json').exists()
    assert not (args.output / 'summary.json').exists()


def test_official_correctness_failure_prevents_summary(tmp_path):
    args = setup_case(tmp_path, repeats=1, distances='3')
    with pytest.raises(ValueError, match='correctness failed'):
        execute(args, FakeNative(fail_variant='reference'))
    assert (args.output / 'failure.json').is_file()
    assert not (args.output / 'summary.json').exists()


@pytest.mark.parametrize('mutation', ['revision', 'binary'])
def test_wrong_official_source_or_binary_is_rejected_before_preparation(tmp_path, mutation):
    args = setup_case(tmp_path)
    if mutation == 'revision':
        receipt = json.loads(args.reference_record.read_text())
        receipt['upstream_revision'] = runner.SOURCE_REVISION
        args.reference_record.write_text(json.dumps(receipt))
    else:
        args.reference_binary.write_text('modified official executable')
    fake = FakeNative()
    with pytest.raises(ValueError, match='official executable'):
        execute(args, fake)
    assert not fake.commands


def test_full_method_schedule_keeps_pairs_adjacent():
    methods = list(runner.METHODS)
    for repeat in range(3):
        schedule = runner.measurement_schedule(methods, repeat, 3)
        assert len(schedule) == 7
        assert sum(len(variants) for _, variants in schedule) == 13
        assert [method for method, _ in schedule].index('reference') == repeat * 3
        assert all(variants == ('reference',) if method == 'reference' else set(variants) == {'source', 'candidate'}
                   for method, variants in schedule)


@pytest.mark.parametrize('mutation', ['change_corpus', 'change_binary'])
def test_changed_inputs_or_executed_binary_are_rejected(tmp_path, mutation):
    args = setup_case(tmp_path, repeats=1, distances='3')
    with pytest.raises(ValueError, match='Changed or missing artifact'):
        execute(args, FakeNative(**{mutation: True}))
    assert (args.output / 'failure.json').is_file()
    assert not (args.output / 'summary.json').exists()


def test_changed_build_binary_is_rejected_before_preparation(tmp_path):
    args = setup_case(tmp_path)
    (args.build / 'candidate').write_text('changed after build')
    fake = FakeNative()
    with pytest.raises(ValueError, match='Changed executable'):
        execute(args, fake)
    assert not fake.commands
