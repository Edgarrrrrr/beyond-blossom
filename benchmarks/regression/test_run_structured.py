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
    def __init__(self, fail_weight=False, change_corpus=False, change_binary=False):
        self.commands = []
        self.fail_weight = fail_weight
        self.change_corpus = change_corpus
        self.change_binary = change_binary

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
        elapsed = shots * (1000 if variant == 'source' else 800)
        (native / 'correctness.json').write_text(json.dumps(dict(status='passed', shots=shots,
            weight_differences=int(self.fail_weight), observable_differences=0, actual_candidate_witnesses=0)))
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
    args = setup_case(tmp_path)
    fake = FakeNative()
    result = execute(args, fake)
    commands = [dict(zip(c[1::2], c[2::2])) for c in fake.commands]
    assert sum(c['--mode'] == 'prepare' for c in commands) == 2
    assert len(commands) == 2 + 2 * 2 * 2 * 2
    for offset in (0, 9):
        selected = fake.commands[offset + 1:offset + 9]
        assert [Path(c[0]).name for c in selected] == ['source', 'candidate', 'source', 'candidate', 'candidate', 'source', 'candidate', 'source']
        modes = [dict(zip(c[1::2], c[2::2]))['--mode'] for c in selected]
        assert modes == ['batch', 'batch', 'pipe-batch', 'pipe-batch', 'pipe-batch', 'pipe-batch', 'batch', 'batch']
        assert len({dict(zip(c[1::2], c[2::2]))['--dataset'] for c in selected}) == 1
        assert all(dict(zip(c[1::2], c[2::2]))['--warmup-seconds'] == '2' for c in selected)
    assert len(result['rows']) == 4
    assert all(row['speedup_source_over_candidate'] == 1.25 for row in result['rows'])
    assert all(row['independent_shots'] == 8 and row['repeats'] == 2 for row in result['rows'])
    assert runner.summarize(args.output) == result


def test_correctness_failure_prevents_successful_summary(tmp_path):
    args = setup_case(tmp_path, repeats=1, distances='3')
    with pytest.raises(ValueError, match='correctness failed'):
        execute(args, FakeNative(fail_weight=True))
    assert (args.output / 'failure.json').is_file()
    assert not (args.output / 'complete.json').exists()
    assert not (args.output / 'summary.json').exists()


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
