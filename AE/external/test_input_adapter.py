"""Format, identity and partition checks; no sampling or decoder timing."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

_spec = importlib.util.spec_from_file_location('external_input_adapter', Path(__file__).with_name('input_adapter.py'))
adapter = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(adapter)


def write_dataset(path, records, version=3):
    """Create a small native-format fixture, including source checksum fields."""
    body = bytearray()
    syndrome_hash = result_hash = events = 0
    for shot_id, hits, observable, weight in records:
        if version == 3:
            body.extend(struct.pack('<Q', shot_id))
            syndrome_hash = adapter.mix(syndrome_hash, shot_id)
        body.extend(struct.pack('<Q', len(hits)))
        body.extend(struct.pack(f'<{len(hits)}Q', *hits))
        body.extend(struct.pack('<Qq', observable, weight))
        events += len(hits)
        syndrome_hash = adapter.mix(syndrome_hash, len(hits))
        for hit in hits:
            syndrome_hash = adapter.mix(syndrome_hash, hit)
        result_hash = adapter.mix(adapter.mix(result_hash, observable), weight & adapter.MASK64)
    bits = lambda value: struct.unpack('<Q', struct.pack('<d', value))[0]
    header = [adapter.MAGIC, version, 3, bits(.001), len(records), 4, events,
              syndrome_hash, result_hash, 12345, bits(8192.0), 7]
    Path(path).write_bytes(struct.pack('<12Q', *header) + body)


def graph_fixture():
    return dict(num_detectors=4, num_observables=1,
                detector_coordinates=[[0, 0, t] for t in range(4)],
                edges=[[0, -1, 1.0, 0, 2], [0, 1, 1.0, 1, 2],
                       [1, 2, 1.0, 0, 2], [2, 3, 1.0, 0, 2],
                       [3, -1, 1.0, 0, 2]],
                integer_scale=2.0, converter_version='test', stim_version='test')


class SparseDatasetTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'dataset.bin'
        self.records = [(8, [0, 2], 1, 123), (19, [], 0, 0), (22, [3], 0, -4)]

    def test_v3_preserves_noncontiguous_original_ids_and_signed_weight(self):
        write_dataset(self.path, self.records)
        saved = adapter.SparseDataset(self.path)
        self.assertEqual(list(saved.records()), self.records)
        self.assertEqual(saved.identity()['sha256'], adapter.sha256(self.path))
        self.assertEqual(saved.identity()['stream_id'], 7)

    def test_v2_assigns_sequence_ids(self):
        write_dataset(self.path, self.records, version=2)
        actual = list(adapter.SparseDataset(self.path).records())
        self.assertEqual([r[0] for r in actual], [0, 1, 2])
        self.assertEqual([r[1:] for r in actual], [r[1:] for r in self.records])

    def test_prefix_selection_still_rejects_corrupted_tail(self):
        write_dataset(self.path, self.records)
        data = bytearray(self.path.read_bytes())
        data[-1] ^= 1
        self.path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            list(adapter.SparseDataset(self.path).records(limit=1))

    def test_truncation_and_trailing_bytes_are_rejected(self):
        for suffix, change in [('truncated', lambda b: b[:-1]), ('trailing', lambda b: b + b'x')]:
            with self.subTest(suffix=suffix):
                write_dataset(self.path, self.records)
                self.path.write_bytes(change(self.path.read_bytes()))
                with self.assertRaises(ValueError):
                    list(adapter.SparseDataset(self.path).records())

    def test_duplicate_ids_and_invalid_detector_lists_are_rejected(self):
        for rows in [[(8, [0], 0, 0), (8, [1], 0, 0)], [(8, [1, 1], 0, 0)],
                     [(8, [2, 1], 0, 0)], [(8, [4], 0, 0)]]:
            with self.subTest(records=rows):
                write_dataset(self.path, rows)
                with self.assertRaises(ValueError):
                    list(adapter.SparseDataset(self.path).records())

    def test_partial_iteration_has_no_verified_identity(self):
        write_dataset(self.path, self.records)
        saved = adapter.SparseDataset(self.path)
        iterator = saved.records()
        next(iterator)
        with self.assertRaisesRegex(ValueError, 'completely verified'):
            saved.identity()
        iterator.close()


class PartitionTest(unittest.TestCase):
    def test_private_boundaries_share_endpoint_time_and_preserve_edge_indices(self):
        initializer, partition, inverse = adapter.partition_graph(graph_fixture(), 3)
        self.assertEqual(inverse, [0, 2, 3, 4, 1, 5])
        self.assertEqual(initializer, dict(vertex_num=6, virtual_vertices=[1, 5],
            weighted_edges=[[0, 1, 2], [0, 2, 2], [2, 3, 2], [3, 4, 2], [4, 5, 2]]))
        self.assertEqual(partition, dict(vertex_num=6, partitions=[[0, 2], [3, 6]], fusions=[[0, 1]]))

    def test_edge_bypassing_middle_slice_is_rejected(self):
        graph = graph_fixture()
        graph['edges'].append([0, 2, 1.0, 0, 2])
        with self.assertRaisesRegex(ValueError, 'bypasses'):
            adapter.partition_graph(graph, 3)

    def test_missing_middle_time_slice_is_rejected(self):
        graph = graph_fixture()
        graph['detector_coordinates'][1][2] = 0
        with self.assertRaisesRegex(ValueError, 'partition'):
            adapter.partition_graph(graph, 3)


class ConversionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.dataset = self.directory / 'dataset.bin'
        self.dem = self.directory / 'model.dem'
        self.dem.write_text('test-only DEM; graph construction mocked\n')
        write_dataset(self.dataset, [(8, [0, 2], 1, 123), (19, [], 0, 0), (22, [3], 0, -4)])
        self.output = self.directory / 'converted'

    def convert(self, **kwargs):
        with mock.patch.object(adapter, 'export_graph', return_value=copy.deepcopy(graph_fixture())):
            return adapter.convert(self.dataset, self.dem, self.output, **kwargs)

    def test_common_input_mapping_and_oracle_scope_are_explicit(self):
        manifest = self.convert(limit=2, warmup_prefix=1)
        payload = json.loads((self.output / 'native-input.json').read_text())
        self.assertEqual(payload['samples'], [[0, 2], []])
        self.assertEqual(payload['blossom_samples'], [[0, 3], []])
        self.assertEqual(payload['warmup'], [[0, 2]])
        self.assertEqual(payload['blossom_warmup'], [[0, 3]])
        self.assertEqual(payload['blossom_edge_to_original'], list(range(5)))
        refs = [json.loads(line) for line in (self.output / 'shot-reference.jsonl').read_text().splitlines()]
        self.assertEqual(refs[0], dict(execution_index=0, original_shot_id=8,
                                      source_oracle_observable=1, source_oracle_integer_weight=123))
        self.assertFalse(manifest['physical_observable_truth_available'])
        self.assertEqual(manifest['external_integer_oracle_status'], 'pending_native_validation')
        self.assertEqual(manifest['warmup']['policy'], 'reused_measurement_prefix')
        self.assertEqual(manifest['source']['saved_shots'], 3)
        self.assertEqual(manifest['selected_shots'], 2)
        for name, digest in manifest['files'].items():
            self.assertEqual(digest, adapter.sha256(self.output / name))

    def test_reusing_output_is_rejected(self):
        self.convert()
        with self.assertRaises(FileExistsError):
            self.convert()

    def test_separate_warmup_requires_identical_companion_dem(self):
        warm_dir = self.directory / 'warm'
        warm_dir.mkdir()
        warm = warm_dir / 'dataset.bin'
        write_dataset(warm, [(101, [1], 1, 100)])
        with self.assertRaisesRegex(ValueError, 'identical companion'):
            self.convert(warmup_dataset=warm)
        (warm_dir / 'model.dem').write_bytes(self.dem.read_bytes())
        manifest = self.convert(warmup_dataset=warm)
        self.assertEqual(manifest['warmup']['policy'], 'separate_saved_sequence')
        payload = json.loads((self.output / 'native-input.json').read_text())
        self.assertEqual(payload['warmup'], [[1]])

    def test_bad_source_leaves_failure_without_completion_receipt(self):
        self.dataset.write_bytes(self.dataset.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            self.convert(limit=1)
        self.assertFalse((self.output / 'input-provenance.json').exists())
        self.assertFalse(json.loads((self.output / 'failure.json').read_text())['complete'])

    def test_independent_warmup_rejects_same_file_and_identical_copy(self):
        with self.assertRaisesRegex(ValueError, 'separate saved'):
            self.convert(warmup_dataset=self.dataset)
        duplicate = self.directory / 'duplicate.bin'
        duplicate.write_bytes(self.dataset.read_bytes())
        with self.assertRaisesRegex(ValueError, 'duplicates'):
            self.convert(warmup_dataset=duplicate)
        self.assertFalse((self.output / 'input-provenance.json').exists())


if __name__ == '__main__':
    unittest.main()
