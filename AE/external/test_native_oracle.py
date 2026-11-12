"""Validate the compiled official oracle against exhaustive small-graph solutions.

Set BB_EXTERNAL_ORACLE to the built native oracle executable before discovery.
These are correctness tests; they produce no benchmark measurements.
"""
import copy
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest


def fixture(num_observables):
    masks = [0] * 5 if num_observables == 0 else [1 << 63, 3, (1 << 64) - 1, 0, 1 << 62]
    edges = [[0, -1, 1.25, masks[0], 4], [0, 1, 2.5, masks[1], 6],
             [1, 2, .75, masks[2], 2], [2, 3, 3.25, masks[3], 8],
             [3, -1, 1.5, masks[4], 10]]
    return dict(num_detectors=4, num_observables=num_observables, edges=edges,
                samples=[[i for i in range(4) if bits & (1 << i)] for bits in range(16)])


def exhaustive_optima(edges, num_detectors):
    """Enumerate every edge subset, independently of the native matching code."""
    best = {}
    for chosen in itertools.product((False, True), repeat=len(edges)):
        syndrome = [0] * num_detectors
        cost = 0
        for selected, edge in zip(chosen, edges):
            if selected:
                u, v, _, _, q = edge
                syndrome[u] ^= 1
                if v >= 0:
                    syndrome[v] ^= 1
                cost += q
        key = tuple(i for i, value in enumerate(syndrome) if value)
        best[key] = min(best.get(key, math.inf), cost)
    return best


def sidecar(directory, name, samples):
    """Independent binary fixture writer; deliberately does not use the adapter."""
    offsets = [0]
    hits = []
    for sample in samples:
        hits.extend(sample)
        offsets.append(len(hits))
    values = [0x4242535041525331, 1, len(samples), len(hits), *offsets, *hits]
    (directory / name).write_bytes(struct.pack('<' + 'Q' * len(values), *values))
    return dict(format='bb-sparse-u64-v1', file=name, shots=len(samples), total_events=len(hits))


@unittest.skipUnless(os.environ.get('BB_EXTERNAL_ORACLE'), 'Set BB_EXTERNAL_ORACLE to the compiled official oracle')
class NativeOracleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.oracle = str(Path(os.environ['BB_EXTERNAL_ORACLE']).resolve())
        self.counter = 0

    def run_oracle(self, payload, suffix=''):
        self.counter += 1
        source = self.directory / f'input-{self.counter}.json'
        output = self.directory / f'output-{self.counter}.jsonl'
        source.write_text(json.dumps(payload) + suffix)
        result = subprocess.run([self.oracle, str(source), str(output)],
                                capture_output=True, text=True, timeout=30)
        return result, source, output

    def check_exhaustive(self, observable_count):
        payload = fixture(observable_count)
        result, _, output = self.run_oracle(payload)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['completed'])
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        self.assertEqual(len(rows), 16)
        optima = exhaustive_optima(payload['edges'], payload['num_detectors'])
        for shot, row in enumerate(rows):
            self.assertEqual(row['shot'], shot)
            syndrome = payload['samples'][shot]
            self.assertEqual(row['integer_weight'], optima[tuple(syndrome)])
            ids = row['edge_ids']
            self.assertEqual(ids, sorted(set(ids)))
            parity = [0] * payload['num_detectors']
            integer_weight = observable = 0
            weights = []
            for index in ids:
                self.assertTrue(0 <= index < len(payload['edges']))
                u, v, weight, mask, q = payload['edges'][index]
                parity[u] ^= 1
                if v >= 0:
                    parity[v] ^= 1
                integer_weight += q
                observable ^= mask
                weights.append(weight)
            self.assertEqual([i for i, bit in enumerate(parity) if bit], syndrome)
            self.assertEqual(row['integer_weight'], integer_weight)
            self.assertEqual(row['observable'], observable)
            self.assertTrue(math.isclose(row['weight'], math.fsum(weights), rel_tol=1e-14, abs_tol=1e-14))

    def test_every_syndrome_with_zero_observables(self):
        self.check_exhaustive(0)

    def test_every_syndrome_with_full_64_bit_observable_masks(self):
        self.check_exhaustive(64)

    def test_invalid_graphs_and_samples_fail(self):
        changes = {
            'duplicate_endpoints': lambda p: p['edges'].append(p['edges'][0]),
            'reversed_duplicate': lambda p: p['edges'].append([1, 0, 1.0, 0, 4]),
            'self_loop': lambda p: p['edges'][0].__setitem__(1, 0),
            'negative_weight': lambda p: p['edges'][0].__setitem__(4, -2),
            'zero_weight': lambda p: p['edges'][0].__setitem__(4, 0),
            'odd_weight': lambda p: p['edges'][0].__setitem__(4, 3),
            'float_integer_weight': lambda p: p['edges'][0].__setitem__(4, 4.0),
            'overflow_weight': lambda p: p['edges'][0].__setitem__(4, 1 << 31),
            'float_detector': lambda p: p.__setitem__('samples', [[0.0]]),
            'negative_detector': lambda p: p.__setitem__('samples', [[-1]]),
            'out_of_range_detector': lambda p: p.__setitem__('samples', [[4]]),
            'duplicate_detector': lambda p: p.__setitem__('samples', [[0, 0]]),
            'unsorted_detectors': lambda p: p.__setitem__('samples', [[1, 0]]),
            'empty_workload': lambda p: p.__setitem__('samples', []),
            'too_many_observables': lambda p: p.__setitem__('num_observables', 65),
            'mask_exceeds_observables': lambda p: p.__setitem__('num_observables', 63),
        }
        for name, change in changes.items():
            with self.subTest(name=name):
                payload = copy.deepcopy(fixture(64))
                change(payload)
                result, _, _ = self.run_oracle(payload)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr.strip())

    def test_trailing_json_and_output_reuse_fail(self):
        result, _, _ = self.run_oracle(fixture(0), suffix=' {}')
        self.assertNotEqual(result.returncode, 0)
        result, source, output = self.run_oracle(fixture(0))
        self.assertEqual(result.returncode, 0, result.stderr)
        original = output.read_bytes()
        repeated = subprocess.run([self.oracle, str(source), str(output)], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertEqual(output.read_bytes(), original)

    def test_sidecar_matches_inline_for_zero_and_64_observables(self):
        for observable_count in (0, 64):
            payload = fixture(observable_count)
            inline, _, output = self.run_oracle(payload)
            self.assertEqual(inline.returncode, 0, inline.stderr)
            expected = output.read_bytes()
            streamed = copy.deepcopy(payload)
            streamed['sample_storage'] = sidecar(self.directory, 'samples.bin', streamed.pop('samples'))
            result, _, output = self.run_oracle(streamed)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_bytes(), expected)


@unittest.skipUnless(os.environ.get('BB_EXTERNAL_BUILD'), 'Set BB_EXTERNAL_BUILD to all compiled external adapters')
class NativeStreamingInputTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.build = Path(os.environ['BB_EXTERNAL_BUILD']).resolve()
        self.counter = 0

    def payload(self, observable_count, permuted=False):
        payload = fixture(observable_count)
        # Fusion's partition binary searches require time-ordered defects.
        # Serial Parity additionally tests a nonmonotonic map without sorting it.
        inverse = [4, 3, 0, 1, 5, 2] if permuted else [0, 2, 3, 4, 1, 5]
        mapped = []
        boundary = 4
        for u, v, _, _, q in payload['edges']:
            if v < 0:
                v = boundary
                boundary += 1
            mapped.append([inverse[u], inverse[v], q])
        payload.update(initializer=dict(vertex_num=6, virtual_vertices=[inverse[4], inverse[5]], weighted_edges=mapped),
                       partition=dict(vertex_num=6, partitions=[[0, 3], [4, 6]] if permuted else [[0, 2], [3, 6]], fusions=[[0, 1]]),
                       blossom_edge_to_original=list(range(5)), warmup=payload['samples'][:4],
                       repeats=3, warmup_seconds=0)
        payload['blossom_samples'] = [[inverse[v] for v in shot] for shot in payload['samples']]
        payload['blossom_warmup'] = [[inverse[v] for v in shot] for shot in payload['warmup']]
        payload['blossom_vertex_map'] = inverse[:4]
        return payload

    def streamed(self, payload):
        result = copy.deepcopy(payload)
        result['sample_storage'] = sidecar(self.directory, 'samples.bin', result.pop('samples'))
        result['warmup_storage'] = sidecar(self.directory, 'warmup.bin', result.pop('warmup'))
        result.pop('blossom_samples')
        result.pop('blossom_warmup')
        return result

    def invoke(self, method, payload):
        self.counter += 1
        source = self.directory / f'input-{self.counter}.json'
        prefix = self.directory / f'output-{self.counter}'
        source.write_text(json.dumps(payload))
        binary = 'blossom' if method in ('parity', 'fusion', 'parity-uf') else method
        command = [str(self.build / binary), str(source), str(prefix)]
        if binary == 'blossom':
            command.append(method)
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        return result, prefix

    def test_all_five_methods_preserve_every_shot_and_three_passes(self):
        import csv
        for observables in (0, 64):
            payload = self.payload(observables)
            streamed = self.streamed(payload)
            for method in ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf'):
                with self.subTest(observables=observables, method=method):
                    results = []
                    chains = []
                    for native in (payload, streamed):
                        result, prefix = self.invoke(method, native)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        receipt = json.loads(result.stdout)
                        self.assertTrue(receipt['completed'])
                        self.assertEqual(receipt['warmup_calls'], 4)
                        with prefix.with_suffix('.csv').open() as source:
                            rows = list(csv.DictReader(source))
                        self.assertEqual([(int(r['repeat']), int(r['shot'])) for r in rows],
                                         [(r, shot) for r in range(3) for shot in range(16)])
                        for row in rows:
                            row.pop('decode_ns')
                        results.append(rows)
                        chains.append(prefix.with_suffix('.corrections.jsonl').read_bytes())
                    self.assertEqual(results[0], results[1])
                    self.assertEqual(chains[0], chains[1])

    def test_serial_blossom_preserves_nonmonotonic_detector_mapping(self):
        import csv
        payload = self.payload(64, permuted=True)
        results = []
        for native in (payload, self.streamed(payload)):
            result, prefix = self.invoke('parity', native)
            self.assertEqual(result.returncode, 0, result.stderr)
            with prefix.with_suffix('.csv').open() as source:
                rows = list(csv.DictReader(source))
            for row in rows:
                row.pop('decode_ns')
            results.append((rows, prefix.with_suffix('.corrections.jsonl').read_bytes()))
        self.assertEqual(results[0], results[1])

    def test_all_native_readers_reject_corrupt_sidecars(self):
        changes = {
            'magic': lambda b: struct.pack_into('<Q', b, 0, 0),
            'version': lambda b: struct.pack_into('<Q', b, 8, 2),
            'header_count': lambda b: struct.pack_into('<Q', b, 16, 15),
            'first_offset': lambda b: struct.pack_into('<Q', b, 32, 1),
            'decreasing_offset': lambda b: struct.pack_into('<Q', b, 32 + 5 * 8, 0),
            'offset_out_of_range': lambda b: struct.pack_into('<Q', b, 32 + 16 * 8, 33),
            'invalid_detector': lambda b: struct.pack_into('<Q', b, 32 + 17 * 8, 4),
            'duplicate_detector': lambda b: struct.pack_into('<Q', b, 32 + 17 * 8 + 3 * 8, 0),
            'truncated': lambda b: b.pop(),
            'trailing': lambda b: b.extend(b'\0'),
        }
        for name, change in changes.items():
            payload = self.streamed(self.payload(0))
            source = self.directory / 'samples.bin'
            data = bytearray(source.read_bytes())
            change(data)
            source.write_bytes(data)
            for method in ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf', 'oracle'):
                with self.subTest(case=name, method=method):
                    result, _ = self.invoke(method, payload)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertTrue(result.stderr.strip())

    def test_sidecar_path_traversal_symlinks_and_mapping_are_rejected(self):
        for filename in ('../samples.bin', str(self.directory / 'samples.bin'), 'alias.bin'):
            payload = self.streamed(self.payload(0))
            if filename == 'alias.bin':
                (self.directory / filename).symlink_to(self.directory / 'samples.bin')
            payload['sample_storage']['file'] = filename
            for method in ('parity', 'unionfind', 'oracle'):
                with self.subTest(filename=filename, method=method):
                    result, _ = self.invoke(method, payload)
                    self.assertNotEqual(result.returncode, 0)
        for mapping in ([4, 3, 0], [4, 3, 0, 0], [4, 3, 0, 5], [4, 3, 0, 99]):
            payload = self.streamed(self.payload(0))
            payload['blossom_vertex_map'] = mapping
            result, _ = self.invoke('parity', payload)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
