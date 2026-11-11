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


if __name__ == '__main__':
    unittest.main()
