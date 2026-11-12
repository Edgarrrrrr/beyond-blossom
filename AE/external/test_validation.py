"""Reject corrupted native measurements and distinguish UF gaps from exact MWPM."""
import csv
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np

from common import check_chain, attach_sparse_samples, close_sparse_samples, SPARSE_HEADER, SPARSE_MAGIC, SPARSE_FORMAT
from input_adapter import partition_graph
from run import load_oracle, validate_input, validate_prepared_graph, validate_result


class MeasurementValidationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.prefix = Path(self.directory.name) / 'parity'
        self.payload = dict(num_detectors=2, num_observables=1, repeats=2, warmup_seconds=0,
                            samples=[[], [0], [0, 1]], warmup=[[]],
                            edges=[[0, -1, 1.0, 1, 2], [0, 1, 1.5, 0, 4], [1, -1, 2.0, 0, 6]])
        self.optimum = np.array([0, 2, 4], dtype=np.int64)

    def write_result(self, chains=None):
        chains = chains or [[], [0], [1]]
        rows = []
        for repeat in range(self.payload['repeats']):
            for shot, (sample, chain) in enumerate(zip(self.payload['samples'], chains)):
                w, q, obs = check_chain(chain, sample, self.payload['edges'], 2)
                rows.append(dict(repeat=repeat, shot=shot, decode_ns=(repeat * 3 + shot + 1) * 1000,
                                 weight=w, integer_weight=q, observable=obs))
        self.write_rows(rows)
        self.prefix.with_suffix('.corrections.jsonl').write_text(''.join(json.dumps(c) + '\n' for c in chains))
        return rows

    def write_rows(self, rows):
        with self.prefix.with_suffix('.csv').open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=['repeat', 'shot', 'decode_ns', 'weight', 'integer_weight', 'observable'])
            writer.writeheader()
            writer.writerows(rows)

    def test_exact_metrics_and_absent_truth(self):
        validate_input(self.payload)
        self.write_result()
        result = validate_result(self.prefix, self.payload, self.optimum, 'parity')
        self.assertEqual(result['repeat_mean_us'], [2, 5])
        self.assertEqual(result['mean_us_per_shot'], 3.5)
        self.assertEqual(result['p50_us'], 3)
        self.assertEqual(result['p99_us'], 6)
        self.assertEqual(result['independent_chain_checks'], 3)
        self.assertIsNone(result['logical_failures'])

    def test_nonoptimal_uf_allowed_but_exact_rejected(self):
        self.write_result([[], [0], [0, 2]])
        result = validate_result(self.prefix, self.payload, self.optimum, 'unionfind')
        self.assertEqual(result['weight_gaps'][0]['nonoptimal_shots'], 1)
        with self.assertRaisesRegex(ValueError, 'optimum'):
            validate_result(self.prefix, self.payload, self.optimum, 'fusion')

    def test_mismatched_witness_rejected(self):
        self.write_result()
        self.prefix.with_suffix('.corrections.jsonl').write_text('[]\n[2]\n[1]\n')
        with self.assertRaisesRegex(ValueError, 'syndrome'):
            validate_result(self.prefix, self.payload, self.optimum, 'parity')

    def test_repeat_order_and_incomplete_output_rejected(self):
        rows = self.write_result()
        rows[4]['shot'] = 0
        self.write_rows(rows)
        with self.assertRaisesRegex(ValueError, 'order'):
            validate_result(self.prefix, self.payload, self.optimum, 'parity')
        self.write_rows(rows[:2])
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            validate_result(self.prefix, self.payload, self.optimum, 'parity')

    def test_corrupt_integer_weight_rejected(self):
        rows = self.write_result()
        rows[4]['integer_weight'] = 0
        self.write_rows(rows)
        with self.assertRaisesRegex(ValueError, 'optimum'):
            validate_result(self.prefix, self.payload, self.optimum, 'parity')

    def test_duplicate_correction_and_negative_edge_rejected(self):
        with self.assertRaisesRegex(ValueError, 'repeated'):
            check_chain([0, 0], [], self.payload['edges'], 2)
        self.payload['edges'][0][4] = -2
        with self.assertRaisesRegex(ValueError, 'quantization'):
            validate_input(self.payload)

    def test_oracle_witness_recomputed(self):
        path = Path(self.directory.name) / 'oracle.jsonl'
        path.write_text(''.join(json.dumps(dict(shot=i, weight=w, integer_weight=q, observable=obs, edge_ids=chain)) + '\n'
            for i, (w, q, obs, chain) in enumerate([(0, 0, 0, []), (1, 2, 1, [0]), (1.5, 4, 0, [1])])))
        np.testing.assert_array_equal(load_oracle(path, self.payload), self.optimum)
        path.write_text(path.read_text().replace('"integer_weight": 2', '"integer_weight": 4'))
        with self.assertRaisesRegex(ValueError, 'disagree'):
            load_oracle(path, self.payload)

    def sidecar(self, payload, mapping):
        value = dict(payload)
        samples = value.pop('samples')
        value.pop('blossom_samples', None)
        offsets = [0]
        for sample in samples: offsets.append(offsets[-1] + len(sample))
        path = Path(self.directory.name)/'samples.bin'
        path.write_bytes(SPARSE_HEADER.pack(SPARSE_MAGIC, 1, len(samples), offsets[-1]) +
                         struct.pack(f'<{len(offsets)}Q', *offsets) +
                         b''.join(struct.pack(f'<{len(sample)}Q', *sample) for sample in samples))
        value.update(sample_storage=dict(format=SPARSE_FORMAT, file=path.name,
                     shots=len(samples), total_events=offsets[-1]), blossom_vertex_map=mapping)
        result = attach_sparse_samples(value, path.parent)
        self.addCleanup(close_sparse_samples, result)
        return result

    def test_sidecar_three_full_passes_match_inline_metrics_and_oracle(self):
        self.payload['repeats'] = 3
        self.write_result()
        inline = validate_result(self.prefix, self.payload, self.optimum, 'parity')
        payload = self.sidecar(self.payload, [0, 1])
        validate_input(payload)
        self.assertEqual(validate_result(self.prefix, payload, self.optimum, 'parity'), inline)
        self.assertEqual(inline['repeat_mean_us'], [2, 5, 8])
        path = Path(self.directory.name)/'oracle.jsonl'
        path.write_text(''.join(json.dumps(dict(shot=i, weight=w, integer_weight=q, observable=obs, edge_ids=chain))+'\n'
            for i,(w,q,obs,chain) in enumerate([(0,0,0,[]),(1,2,1,[0]),(1.5,4,0,[1])])))
        np.testing.assert_array_equal(load_oracle(path, payload), load_oracle(path, self.payload))
        # No validation shortcut is permitted for a streaming shot view.
        self.prefix.with_suffix('.corrections.jsonl').write_text('[]\n[2]\n[1]\n')
        with self.assertRaisesRegex(ValueError, 'syndrome'):
            validate_result(self.prefix, payload, self.optimum, 'parity')

    def test_redundant_native_mappings_must_match_shared_graph(self):
        graph = dict(num_detectors=3, num_observables=1,
                     edges=[[0, -1, 1, 1, 2], [0, 1, 2, 0, 4], [1, 2, 2, 0, 4], [2, -1, 1, 0, 2]],
                     detector_coordinates=[[0, 0, 0], [0, 0, 1], [0, 0, 2]])
        initializer, partition, inverse = partition_graph(graph, 3)
        payload = dict(graph, samples=[[], [0], [2]], warmup=[[]], repeats=1, warmup_seconds=0,
                       initializer=initializer, partition=partition, blossom_edge_to_original=list(range(4)),
                       blossom_samples=[[], [inverse[0]], [inverse[2]]], blossom_warmup=[[]])
        provenance = dict(source=dict(distance=3), selected_shots=3, repeats=1,
                          warmup=dict(selected_shots=1, seconds=0))
        validate_prepared_graph(payload, graph, provenance)
        sparse = self.sidecar(payload, inverse[:graph['num_detectors']])
        validate_input(sparse)
        validate_prepared_graph(sparse, graph, provenance)
        wrong_map = dict(sparse, blossom_vertex_map=list(reversed(sparse['blossom_vertex_map'])))
        with self.assertRaisesRegex(ValueError, 'mapping differs'):
            validate_prepared_graph(wrong_map, graph, provenance)
        for field, value in [('blossom_edge_to_original', [1, 0, 2, 3]),
                             ('blossom_samples', [[], [], [inverse[2]]]),
                             ('blossom_warmup', [[inverse[0]]]),
                             ('num_observables', 0)]:
            with self.subTest(field=field):
                changed = copy.deepcopy(payload)
                changed[field] = value
                with self.assertRaises(ValueError):
                    validate_prepared_graph(changed, graph, provenance)


if __name__ == '__main__':
    unittest.main()
