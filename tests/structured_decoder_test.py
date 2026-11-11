# Copyright 2026 Beyond Blossom Contributors
# Licensed under the Apache License, Version 2.0.
import os

import numpy as np
import pytest

import beyond_blossom as bb

stim = pytest.importorskip("stim")


def model_and_shots():
    circuit = stim.Circuit.generated(
        "surface_code:rotated_memory_z", distance=3, rounds=3,
        after_clifford_depolarization=0.01,
        before_measure_flip_probability=0.01,
        after_reset_flip_probability=0.01,
    )
    return circuit.detector_error_model(decompose_errors=True), circuit.compile_detector_sampler(seed=923).sample(128)


@pytest.mark.parametrize("strategy", ["structured", "predecoded"])
def test_structured_dense_sparse_packed_and_reuse(strategy):
    dem, shots = model_and_shots()
    decoder = bb.StructuredDecoder.from_detector_error_model(
        dem, distance=3, rounds=3, strategy=strategy, simd="scalar", include_search_graph=True,
    )
    expected, expected_weights = bb.Matching.from_detector_error_model(dem).decode_batch(shots, return_weights=True)
    for _ in range(2):
        actual, weights = decoder.decode_batch(shots, return_weights=True)
        np.testing.assert_array_equal(actual, expected)
        np.testing.assert_allclose(weights, expected_weights, rtol=0, atol=1e-9)
    packed = np.packbits(shots, axis=1, bitorder="little")
    actual = decoder.decode_batch(packed, bit_packed_shots=True, bit_packed_predictions=True)
    np.testing.assert_array_equal(actual, np.packbits(expected, axis=1, bitorder="little"))
    for k in range(8):
        np.testing.assert_array_equal(decoder.decode(shots[k]), expected[k])
        np.testing.assert_array_equal(decoder.decode_detection_events(np.flatnonzero(shots[k]).tolist()), expected[k])
    assert decoder.num_detectors == dem.num_detectors
    assert decoder.num_observables == dem.num_observables
    assert decoder.metadata["strategy"] == strategy
    assert decoder.metadata["supports_edges"]
    assert decoder.decode_to_edges_array(shots[0]).shape[1] == 2
    assert decoder.decode_to_matched_dets_array(shots[0]).shape[1] == 2
    assert decoder.decode_batch(np.zeros((0, dem.num_detectors), dtype=np.uint8)).shape == (0, dem.num_observables)


def tiny_model(observable=0, negative=False):
    label = "" if observable is None else f"L{observable}"
    return stim.DetectorErrorModel(f"""
        error({0.7 if negative else 0.11}) D0 D1 {label}
        error(0.03) D0
        error(0.02) D1
        detector(0, 0, 0) D0
        detector(2, 0, 0) D1
    """)


@pytest.mark.parametrize("observable", [0, 70])
@pytest.mark.parametrize("negative", [False, True])
def test_extended_observables_and_negative_weights(observable, negative):
    dem = tiny_model(observable, negative)
    decoder = bb.StructuredDecoder(dem, distance=3, rounds=3, geometry="unrotated")
    shots = np.array([[0, 0], [0, 1], [1, 0], [1, 1]], dtype=np.uint8)
    expected, weights = bb.Matching.from_detector_error_model(dem).decode_batch(shots, return_weights=True)
    actual, actual_weights = decoder.decode_batch(shots, return_weights=True)
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_allclose(actual_weights, weights, rtol=0, atol=1e-9)
    packed = decoder.decode_batch(shots, bit_packed_predictions=True)
    np.testing.assert_array_equal(packed, np.packbits(expected, axis=1, bitorder="little"))


@pytest.mark.parametrize("observable_count", [0, 63, 64, 65])
def test_observable_word_boundaries_and_zero_width(observable_count):
    dem = tiny_model(None if observable_count == 0 else observable_count - 1)
    decoder = bb.StructuredDecoder(dem, distance=3, rounds=3, geometry="unrotated")
    shots = np.array([[0, 0], [0, 1], [1, 0], [1, 1]], dtype=np.uint8)
    expected, expected_weights = bb.Matching.from_detector_error_model(dem).decode_batch(shots, return_weights=True)
    actual, weights = decoder.decode_batch(shots, return_weights=True)
    assert actual.shape == (4, observable_count)
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_allclose(weights, expected_weights, rtol=0, atol=1e-9)
    packed = decoder.decode_batch(np.packbits(shots, axis=1, bitorder="little"),
                                  bit_packed_shots=True, bit_packed_predictions=True)
    np.testing.assert_array_equal(packed, np.packbits(expected, axis=1, bitorder="little"))
    for index, shot in enumerate(shots):
        np.testing.assert_array_equal(decoder.decode_detection_events(np.flatnonzero(shot).tolist()), expected[index])


def test_invalid_inputs_and_error_recovery():
    dem = tiny_model()
    decoder = bb.StructuredDecoder(dem, distance=3, rounds=3, geometry="unrotated")
    for invalid in [[0, 2], [0, -1], [0, 256], [0.0, 1.0], [[0, 1]], [0]]:
        with pytest.raises(ValueError):
            decoder.decode(invalid)
    with pytest.raises(ValueError, match="padding"):
        decoder.decode_batch(np.array([[128]], dtype=np.uint8), bit_packed_shots=True)
    with pytest.raises(ValueError):
        decoder.decode_detection_events([2])
    np.testing.assert_array_equal(decoder.decode([0, 0]), [0])
    with pytest.raises(ValueError):
        bb.StructuredDecoder(dem, distance=3, rounds=3, strategy="unstructured")
    with pytest.raises(ValueError):
        bb.StructuredDecoder(dem, distance=0, rounds=3)
    with pytest.raises(ValueError):
        bb.StructuredDecoder(dem, distance=3, rounds=3, geometry="invalid")


@pytest.mark.parametrize("strategy,threads", [("component-parallel", 2), ("pipelined", 4), ("parallel-predecode", 8)])
@pytest.mark.parametrize("single", [False, True])
def test_parallel_strategies_batch_and_team_lifecycle(strategy, threads, single):
    if not hasattr(os, "sched_getaffinity"):
        pytest.skip("Explicit Linux CPU affinity required for this test")
    cpus = sorted(os.sched_getaffinity(0))[:1 if single else threads]
    if len(cpus) != (1 if single else threads):
        pytest.skip("Not enough assigned CPUs")
    dem, shots = model_and_shots()
    decoder = bb.StructuredDecoder(
        dem, distance=3, rounds=3, strategy=strategy, simd="scalar", cpus=cpus,
        same_algorithm_single_thread=single,
    )
    expected = bb.StructuredDecoder(dem, distance=3, rounds=3, simd="scalar").decode_batch(shots)
    for _ in range(2):
        np.testing.assert_array_equal(decoder.decode_batch(shots), expected)
    assert decoder.metadata["actual_threads"] == (1 if single else threads)
    np.testing.assert_array_equal(decoder.decode(shots[1]), expected[1])
