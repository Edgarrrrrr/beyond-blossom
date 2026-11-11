
from pathlib import Path
import importlib.metadata as metadata
import json
import sys
import numpy as np
import stim
import beyond_blossom as bb
from beyond_blossom import _cpp_beyond_blossom as native

installed = Path(sys.argv[1]).resolve()
assert installed in Path(bb.__file__).resolve().parents, bb.__file__
assert installed in Path(native.__file__).resolve().parents, native.__file__
m = bb.Matching()
m.add_edge(0, 1, fault_ids=0, weight=1.0)
m.add_boundary_edge(0, fault_ids=1, weight=2.0)
assert m.decode([1, 1]).tolist() == [1, 0]
circuit = stim.Circuit.generated('surface_code:rotated_memory_x', distance=5, rounds=5,
                                after_clifford_depolarization=0.001)
model = circuit.detector_error_model(decompose_errors=True)
shots = np.ascontiguousarray(circuit.compile_detector_sampler(seed=17).sample(shots=100), dtype=np.uint8)
generic = bb.Matching.from_detector_error_model(model)
expected, expected_weights = generic.decode_batch(shots, return_weights=True)
decoder = bb.StructuredDecoder.from_detector_error_model(model, distance=5, rounds=5,
    strategy='predecoded', simd='scalar', include_search_graph=True)
predictions, weights = decoder.decode_batch(shots, return_weights=True)
assert predictions.shape == (100, 1) and weights.shape == (100,)
np.testing.assert_allclose(weights, expected_weights, rtol=0, atol=1e-9)
direct = native._StructuredDecoder(str(model), 5, 5, 'rotated', 'predecoded', 'scalar', [], False, True)
native_predictions, native_weights = direct.decode_batch(shots, False, False)
np.testing.assert_array_equal(native_predictions, predictions)
np.testing.assert_array_equal(native_weights, weights)
packed = np.packbits(shots, axis=1, bitorder='little')
packed_predictions, packed_weights = direct.decode_batch(packed, True, True)
np.testing.assert_array_equal(packed_predictions, np.packbits(predictions, axis=1, bitorder='little'))
np.testing.assert_array_equal(packed_weights, weights)
for row in range(8):
    events = np.flatnonzero(shots[row]).tolist()
    prediction, weight = direct.decode_sparse(events)
    np.testing.assert_array_equal(prediction, predictions[row])
    assert abs(weight - weights[row]) < 1e-9
    assert direct.decode_edges(events, False).shape[1] == 2
    assert direct.decode_edges(events, True).shape[1] == 2
record = dict(package=bb.__file__, extension=native.__file__, version=metadata.version('beyond-blossom'),
              python_version=sys.version,
              matching_result=[1, 0], structured_predictions=list(predictions.shape),
              native_sparse_cases=8, metadata=decoder.metadata,
              dependency_versions={name:metadata.version(name) for name in ['numpy','scipy','networkx','matplotlib','stim','rustworkx','pytest']})
Path('quickstart-result.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
