import itertools

import numpy as np
import pytest

import pymatching

stim = pytest.importorskip("stim")
SIMD_VARIANTS = ["scalar", "auto", "avx2", "avx512vl", "avx512"]


def canonical_edges(edges):
    return sorted(tuple(sorted(map(int, edge))) for edge in edges)


def configure_grid_for_test(matching, **kwargs):
    simd = kwargs.get("simd", "auto")
    if simd not in ["scalar", "auto"] and not matching.static_grid_info[f"{simd}_available"]:
        pytest.skip(f"{simd} unavailable on this CPU")
    matching.configure_static_grid(**kwargs)


def assert_storage_info(component):
    slots = component["storage_directions"]
    assert slots in [12, 16]
    assert component["directions"] == 16
    assert component["scan_directions"] == 12
    assert component["weight_row_bytes"] == 4 * slots
    assert component["weight_row_alignment"] == (16 if slots == 12 else 64)
    assert component["weight_rows_aligned"]
    assert component["weight_rows_aligned_64"] == (slots == 16)
    mapping = component["storage_slot_global_directions"]
    assert len(mapping) == slots
    active = [(i, d) for i, d in enumerate(mapping) if d is not None]
    expected_stencils = [list(range(10)) + [12, 13],
                         list(range(8)) + [10, 11, 14, 15], list(range(12))]
    assert [d for _, d in active] in expected_stencils
    assert component["scan_slot_mask"] == sum(1 << i for i, _ in active)
    for expected_index, (i, d) in enumerate(active):
        assert mapping[i ^ 1] == d ^ 1
        assert i == (expected_index if slots == 12 else d)


@pytest.mark.parametrize("simd", SIMD_VARIANTS)
@pytest.mark.parametrize("num_observables", [8, 80])
@pytest.mark.parametrize("weights", [[1, 1, 1, 1, 2, 2], [0, 0, 0, 0, 0, 0], [-2, 1, 1, 2, -1, 2]])
def test_static_grid_ties_return_a_valid_optimal_correction(simd, num_observables, weights):
    # A square has equal-length paths with different observable XORs. Enumerate
    # every edge subset instead of selecting the generic decoder's tie winner.
    endpoints = [(0, 1), (1, 2), (2, 3), (3, 0), (0, None), (2, None)]
    fault_ids = list(range(6)) if num_observables == 8 else [0, 1, 63, 65, 66, 79]
    optimum = {}
    for selected in itertools.product([False, True], repeat=len(endpoints)):
        parity = observable = weight = 0
        for use, (u, v), w, fault_id in zip(selected, endpoints, weights, fault_ids):
            if use:
                parity ^= (1 << u) ^ (0 if v is None else 1 << v)
                observable ^= 1 << fault_id
                weight += w
        if parity not in optimum or weight < optimum[parity][0]:
            optimum[parity] = (weight, {observable})
        elif weight == optimum[parity][0]:
            optimum[parity][1].add(observable)
    if weights[0] == 1:
        assert len(optimum[0b0101][1]) > 1

    for structured in [False, True]:
        matching = pymatching.Matching()
        # Reverse insertion makes original adjacency order differ from grid order.
        for (u, v), weight, fault_id in reversed(list(zip(endpoints, weights, fault_ids))):
            if v is None:
                matching.add_boundary_edge(u, weight=weight, fault_ids={fault_id})
            else:
                matching.add_edge(u, v, weight=weight, fault_ids={fault_id})
        matching.ensure_num_fault_ids(num_observables)
        if structured:
            configure_grid_for_test(matching,
                distance=3, rounds=3, geometry="unrotated", simd=simd,
                detector_coordinates=[[0, 0, 0], [2, 0, 0], [2, 2, 0], [0, 2, 0]])
        edge_indices = {tuple(sorted((u, -1 if v is None else v))): i
                        for i, (u, v) in enumerate(endpoints)}
        for parity in list(range(16)) + list(reversed(range(16))):
            shot = [(parity >> i) & 1 for i in range(4)]
            prediction, weight = matching.decode(shot, return_weight=True)
            mask = sum(int(value) << i for i, value in enumerate(prediction))
            expected_weight, allowed_observables = optimum[parity]
            assert weight == expected_weight
            assert mask in allowed_observables
            # Search may independently select another optimal path. Verify its
            # actual edges, parity and objective rather than require the same XOR.
            selected = set()
            for edge in canonical_edges(matching.decode_to_edges_array(shot)):
                selected.symmetric_difference_update({edge_indices[edge]})
            actual_parity = actual_observable = actual_weight = 0
            for i in selected:
                u, v = endpoints[i]
                actual_parity ^= (1 << u) ^ (0 if v is None else 1 << v)
                actual_observable ^= 1 << fault_ids[i]
                actual_weight += weights[i]
            assert actual_parity == parity
            assert actual_weight == expected_weight
            assert actual_observable in allowed_observables


def surface_case(geometry, basis="z", distance=3, rounds=None, correlations=False):
    rounds = distance if rounds is None else rounds
    circuit = stim.Circuit.generated(
        f"surface_code:{geometry}_memory_{basis}", distance=distance, rounds=rounds,
        after_clifford_depolarization=0.01, before_measure_flip_probability=0.01,
        after_reset_flip_probability=0.01)
    dem = circuit.detector_error_model(decompose_errors=True)
    matching = pymatching.Matching.from_detector_error_model(dem, enable_correlations=correlations)
    return circuit, dem, matching


@pytest.mark.parametrize("geometry,basis,distance,simd", itertools.product(
    ["rotated", "unrotated"], ["x", "z"], [3, 5, 9], SIMD_VARIANTS))
def test_static_grid_batch_matches_generic(geometry, basis, distance, simd):
    circuit, dem, matching = surface_case(geometry, basis, distance)
    shots = circuit.compile_detector_sampler(seed=721).sample(256)
    expected, weights = matching.decode_batch(shots, return_weights=True)
    configure_grid_for_test(matching,
        distance=distance, rounds=distance, geometry=geometry,
        detector_coordinates=dem.get_detector_coordinates(), simd=simd)
    info = matching.static_grid_info
    assert info["backend"] == "static_grid"
    assert info["independent_decoders"] == 2
    assert len(info["components"]) == 2
    assert sum(c["detectors"] for c in info["components"]) == dem.num_detectors
    for component in info["components"]:
        assert component["node_records"] == component["detectors"] < component["cells"]
        assert component["collision_state_bytes"] == component["cells"] * 8
        assert component["cell_node_index_bytes"] == component["cells"] * 4
        assert_storage_info(component)
        assert component["simd"] == ("scalar" if simd == "auto" else simd)
    for _ in range(2):
        actual, actual_weights = matching.decode_batch(shots, return_weights=True)
        np.testing.assert_array_equal(actual, expected)
        np.testing.assert_array_equal(actual_weights, weights)
    packed = np.packbits(shots, axis=1, bitorder="little")
    np.testing.assert_array_equal(matching.decode_batch(packed, bit_packed_shots=True), expected)
    matching.disable_static_grid()
    assert matching.static_grid_info["backend"] == "generic"
    np.testing.assert_array_equal(matching.decode_batch(shots), expected)


@pytest.mark.parametrize("geometry", ["rotated", "unrotated"])
def test_static_grid_paths_use_global_detector_ids(geometry):
    circuit, dem, matching = surface_case(geometry)
    shots = circuit.compile_detector_sampler(seed=153).sample(64)
    expected = matching.decode_batch(shots)
    edges = [matching.decode_to_edges_array(s) for s in shots]
    pairs = [matching.decode_to_matched_dets_array(s) for s in shots]
    dictionaries = [matching.decode_to_matched_dets_dict(s) for s in shots]
    matching.configure_static_grid(
        distance=3, rounds=3, geometry=geometry, detector_coordinates=dem.get_detector_coordinates())
    np.testing.assert_array_equal(matching.decode_batch(shots), expected)
    for s, edge, pair, dictionary in zip(shots, edges, pairs, dictionaries):
        assert canonical_edges(matching.decode_to_edges_array(s)) == canonical_edges(edge)
        assert canonical_edges(matching.decode_to_matched_dets_array(s)) == canonical_edges(pair)
        assert matching.decode_to_matched_dets_dict(s) == dictionary


@pytest.mark.parametrize("geometry", ["rotated", "unrotated"])
def test_static_grid_rejects_correlations_without_changing_generic(geometry):
    circuit, dem, matching = surface_case(geometry, correlations=True)
    shots = circuit.compile_detector_sampler(seed=153).sample(16)
    expected = matching.decode_batch(shots, enable_correlations=True)
    uncorrelated = matching.decode_batch(shots)
    matching.configure_static_grid(
        distance=3, rounds=3, geometry=geometry, detector_coordinates=dem.get_detector_coordinates())
    for invoke in [lambda: matching.decode(shots[0], enable_correlations=True),
                   lambda: matching.decode_batch(shots, enable_correlations=True),
                   lambda: matching.decode_to_edges_array(shots[0], enable_correlations=True)]:
        with pytest.raises(ValueError, match="[Cc]orrelat"):
            invoke()
    np.testing.assert_array_equal(matching.decode_batch(shots), uncorrelated)
    matching.disable_static_grid()
    np.testing.assert_array_equal(matching.decode_batch(shots, enable_correlations=True), expected)


def test_static_grid_negative_weights_and_large_observables():
    matching = pymatching.Matching()
    matching.add_edge(0, 1, weight=-2, fault_ids={65})
    matching.add_edge(1, 2, weight=4, fault_ids={1})
    matching.add_boundary_edge(0, weight=-3, fault_ids={2})
    matching.add_boundary_edge(2, weight=1, fault_ids={3})
    shots = np.array(list(itertools.product([0, 1], repeat=3)), dtype=np.uint8)
    expected, weights = matching.decode_batch(shots, return_weights=True)
    edges = [matching.decode_to_edges_array(s) for s in shots]
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated", detector_coordinates=[[0, 0, 0], [2, 0, 0], [4, 0, 0]])
    for _ in range(3):
        actual, actual_weights = matching.decode_batch(shots, return_weights=True)
        np.testing.assert_array_equal(actual, expected)
        np.testing.assert_array_equal(actual_weights, weights)
        for s, edge in zip(shots, edges):
            np.testing.assert_array_equal(matching.decode_to_edges_array(s), edge)


def test_static_grid_failure_and_reconfiguration():
    circuit, dem, matching = surface_case("rotated")
    shot = circuit.compile_detector_sampler(seed=19).sample(1)[0]
    expected = matching.decode(shot)
    options = dict(distance=3, rounds=3, geometry="rotated", detector_coordinates=dem.get_detector_coordinates())
    for invalid in [dict(distance=4), dict(rounds=2), dict(geometry="unknown"), dict(simd="unknown"),
                    dict(detector_coordinates=np.zeros((dem.num_detectors, 3))),
                    dict(detector_coordinates=np.full((dem.num_detectors, 3), np.nan)),
                    dict(groups=list(range(dem.num_detectors)))]:
        with pytest.raises((ValueError, TypeError)):
            matching.configure_static_grid(**(options | invalid))
        assert matching.static_grid_info["backend"] == "generic"
        np.testing.assert_array_equal(matching.decode(shot), expected)
    matching.configure_static_grid(**options)
    with pytest.raises(ValueError):
        matching.configure_static_grid(**(options | dict(distance=4)))
    assert matching.static_grid_info["backend"] == "static_grid"
    np.testing.assert_array_equal(matching.decode(shot), expected)


def test_static_grid_reuse_after_unsatisfiable_syndrome():
    matching = pymatching.Matching()
    matching.add_edge(0, 1, weight=1, fault_ids={0})
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated", detector_coordinates=[[0, 0, 0], [2, 0, 0]])
    for _ in range(3):
        with pytest.raises(ValueError, match="No perfect matching"):
            matching.decode([1, 0])
        np.testing.assert_array_equal(matching.decode([1, 1]), [1])


@pytest.mark.parametrize("geometry", ["rotated", "unrotated"])
def test_static_grid_three_distance_rounds(geometry):
    circuit, dem, matching = surface_case(geometry, distance=3, rounds=9)
    shots = circuit.compile_detector_sampler(seed=751).sample(256)
    expected = matching.decode_batch(shots)
    matching.configure_static_grid(
        distance=3, rounds=9, geometry=geometry, detector_coordinates=dem.get_detector_coordinates())
    np.testing.assert_array_equal(matching.decode_batch(shots), expected)


def test_static_grid_explicit_groups_hole_and_weight_update():
    matching = pymatching.Matching()
    coordinates = []
    groups = []
    for group in [10, 20]:
        cells = {}
        for y in range(3):
            for x in range(3):
                if (x, y) == (1, 1):
                    continue
                cells[x, y] = len(coordinates)
                coordinates.append([2 * x, 2 * y, 0])
                groups.append(group)
        for (x, y), node in cells.items():
            for dx, dy in [(1, 0), (0, 1)]:
                if (x + dx, y + dy) in cells:
                    matching.add_edge(node, cells[x + dx, y + dy], weight=(node % 3), fault_ids={0})
            matching.add_boundary_edge(node, weight=1 + node % 2)
    shots = np.random.default_rng(579).integers(2, size=(512, len(coordinates)), dtype=np.uint8)

    def exact_subset_weights(graph):
        # Each edge is either absent or present. State bits encode detector
        # parity and the one observable; no decoder chooses the tie winner.
        detectors = len(coordinates)
        assert detectors == 16
        states = np.arange(1 << (detectors + 1), dtype=np.uint32)
        unreachable = np.iinfo(np.int64).max // 4
        costs = np.full(len(states), unreachable, dtype=np.int64)
        costs[0] = 0
        for u, v, edge in graph.edges():
            weight = int(edge["weight"])
            assert edge["weight"] == weight and 0 <= weight <= 7
            assert edge["fault_ids"].issubset({0})
            toggle = (1 << u) ^ (0 if v is None else 1 << v)
            if 0 in edge["fault_ids"]:
                toggle ^= 1 << detectors
            # Both alternatives read the previous edge-prefix table, so an
            # edge cannot be reused. All finite costs are exact small integers.
            costs = np.minimum(costs, costs[states ^ toggle] + weight)
        return costs.reshape(2, 1 << detectors)

    syndrome_ids = shots.astype(np.uint64) @ (np.uint64(1) << np.arange(len(coordinates), dtype=np.uint64))

    def assert_optimal_batch(oracle):
        predictions, weights = matching.decode_batch(shots, return_weights=True)
        assert predictions.shape == (len(shots), 1)
        assert np.all((predictions == 0) | (predictions == 1))
        optimum = oracle.min(axis=0)[syndrome_ids]
        np.testing.assert_array_equal(weights, optimum)
        np.testing.assert_array_equal(oracle[predictions[:, 0], syndrome_ids], optimum)

    original = exact_subset_weights(matching)
    assert np.any(original[0] == original[1])  # Exercise equal-weight observable ties.
    assert_optimal_batch(original)  # Generic backend before the weight update.
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated", detector_coordinates=coordinates, groups=groups)
    assert [c["group"] for c in matching.static_grid_info["components"]] == [10, 20]
    assert_optimal_batch(original)  # Structured backend, including the two holes.
    matching.add_boundary_edge(0, weight=7, merge_strategy="replace")
    updated = exact_subset_weights(matching)
    assert np.any(original.min(axis=0)[syndrome_ids] != updated.min(axis=0)[syndrome_ids])
    assert_optimal_batch(updated)  # The structured cache must observe the new edge weight.
    matching.disable_static_grid()
    assert_optimal_batch(updated)  # The generic cache must also observe the update.


def test_static_grid_independent_caches_follow_graph_mutations():
    def make_matching():
        graph = pymatching.Matching()
        graph.add_edge(0, 1, weight=2, fault_ids={0})
        graph.add_edge(1, 2, weight=3, fault_ids={1})
        graph.add_boundary_edge(0, weight=5, fault_ids={2})
        graph.add_boundary_edge(2, weight=4, fault_ids={3})
        return graph

    matching = make_matching()
    reference = make_matching()
    options = dict(
        distance=3, rounds=3, geometry="unrotated", simd="scalar",
        detector_coordinates=[[0, 0, 0], [2, 0, 0], [4, 0, 0]])
    shots = np.array(list(itertools.product([0, 1], repeat=3)), dtype=np.uint8)
    original = matching.decode_batch(shots)  # Populate the generic cache.
    matching.configure_static_grid(**options)
    np.testing.assert_array_equal(matching.decode_batch(shots), original)

    # Both cached decoders must observe changed edge weights and a larger
    # observable space, including the search-graph path above 64 observables.
    for graph in (matching, reference):
        graph.add_edge(0, 1, weight=0.25, fault_ids={0}, merge_strategy="replace")
        graph.add_boundary_edge(1, weight=0.75, fault_ids={66})
        graph.ensure_num_fault_ids(70)
    expected, expected_weights = reference.decode_batch(shots, return_weights=True)
    assert expected.shape == (8, 70)
    assert expected[:, 66].any()
    for _ in range(2):
        actual, weights = matching.decode_batch(shots, return_weights=True)
        np.testing.assert_array_equal(actual, expected)
        np.testing.assert_array_equal(weights, expected_weights)
        packed = matching.decode_batch(shots, bit_packed_predictions=True)
        np.testing.assert_array_equal(packed, np.packbits(expected, axis=1, bitorder="little"))
        matching.disable_static_grid()
        np.testing.assert_array_equal(matching.decode_batch(shots), expected)
        matching.configure_static_grid(**options)

    # Mutating while the generic backend is selected also invalidates a
    # previously constructed structured decoder.
    matching.disable_static_grid()
    for graph in (matching, reference):
        graph.add_boundary_edge(2, weight=0.5, fault_ids={3}, merge_strategy="replace")
    expected = reference.decode_batch(shots)
    matching.configure_static_grid(**options)
    np.testing.assert_array_equal(matching.decode_batch(shots), expected)
    for shot, prediction in zip(shots, expected):
        np.testing.assert_array_equal(matching.decode(shot), prediction)
        np.testing.assert_array_equal(matching.decode_to_edges_array(shot), reference.decode_to_edges_array(shot))


def test_static_grid_rejects_new_unsupported_topology_without_losing_generic_graph():
    matching = pymatching.Matching()
    matching.add_edge(0, 1, weight=2, fault_ids={0})
    matching.add_edge(1, 2, weight=2, fault_ids={1})
    matching.add_boundary_edge(0, weight=3)
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated",
        detector_coordinates=[[0, 0, 0], [2, 0, 0], [4, 0, 0]])
    np.testing.assert_array_equal(matching.decode([1, 0, 1]), [1, 1])
    matching.add_edge(0, 2, weight=1, fault_ids={2})
    with pytest.raises(ValueError):
        matching.decode([1, 0, 1])
    matching.disable_static_grid()
    np.testing.assert_array_equal(matching.decode([1, 0, 1]), [0, 0, 1])


@pytest.mark.parametrize("distance,multiple", itertools.product([21, 25], [1, 3]))
def test_static_grid_large_registry_surface_models(distance, multiple):
    rounds = multiple * distance
    circuit, dem, matching = surface_case("rotated", basis="x", distance=distance, rounds=rounds)
    shots = circuit.compile_detector_sampler(seed=318).sample(8)
    expected, expected_weights = matching.decode_batch(shots, return_weights=True)
    matching.configure_static_grid(
        distance=distance, rounds=rounds, geometry="rotated",
        detector_coordinates=dem.get_detector_coordinates(), simd="scalar")
    assert len(matching.static_grid_info["components"]) == 2
    assert sum(c["detectors"] for c in matching.static_grid_info["components"]) == dem.num_detectors
    actual, weights = matching.decode_batch(shots, return_weights=True)
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_array_equal(weights, expected_weights)


def test_static_grid_interleaved_components_negative_weights_and_80_observables():
    matching = pymatching.Matching()
    matching.add_edge(0, 2, weight=-0.713, fault_ids={0, 79})
    matching.add_edge(1, 3, weight=190.123, fault_ids={65, 79})
    matching.add_boundary_edge(0, weight=1.029, fault_ids={79})
    matching.add_boundary_edge(1, weight=-212.983, fault_ids={65})
    matching.add_edge(3, 3, weight=-3.71, fault_ids={72})
    matching.ensure_num_fault_ids(80)
    shots = np.array(list(itertools.product([0, 1], repeat=4)), dtype=np.uint8)
    expected, expected_weights = matching.decode_batch(shots, return_weights=True)
    expected_edges = [matching.decode_to_edges_array(s) for s in shots]
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated", simd="scalar",
        detector_coordinates=[[0, 0, 0], [0, 0, 0], [2, 0, 0], [2, 0, 0]])
    info = matching.static_grid_info
    assert info["independent_decoders"] == 2
    assert [c["global_detectors"] for c in info["components"]] == [[0, 2], [1, 3]]
    assert expected.shape == (16, 80)
    assert expected[:, 72].all()  # Negative self-loop contributes even with no hits.
    for _ in range(3):
        actual, weights = matching.decode_batch(shots, return_weights=True)
        np.testing.assert_array_equal(actual, expected)
        np.testing.assert_array_equal(weights, expected_weights)
        np.testing.assert_array_equal(
            matching.decode_batch(shots, bit_packed_predictions=True),
            np.packbits(expected, axis=1, bitorder="little"))
        for shot, prediction, edges in zip(shots, expected, expected_edges):
            np.testing.assert_array_equal(matching.decode(shot), prediction)
            assert canonical_edges(matching.decode_to_edges_array(shot)) == canonical_edges(edges)


def test_static_grid_shared_boundary_keeps_components_separate():
    matching = pymatching.Matching()
    matching.add_edge(0, 2, weight=2, fault_ids={0})
    matching.add_edge(1, 3, weight=4, fault_ids={1})
    matching.add_edge(2, 4, weight=6, fault_ids={0})
    matching.add_edge(3, 4, weight=8, fault_ids={1})
    matching.set_boundary_nodes({4})
    shots = np.array(list(itertools.product([0, 1], repeat=5)), dtype=np.uint8)
    expected, expected_weights = matching.decode_batch(shots, return_weights=True)
    edges = [matching.decode_to_edges_array(s) for s in shots]
    pairs = [matching.decode_to_matched_dets_dict(s) for s in shots]
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated",
        detector_coordinates=[[0, 0, 0], [0, 0, 0], [2, 0, 0], [2, 0, 0], [4, 0, 0]])
    info = matching.static_grid_info
    assert info["independent_decoders"] == 2
    assert [c["global_detectors"] for c in info["components"]] == [[0, 2], [1, 3]]
    actual, weights = matching.decode_batch(shots, return_weights=True)
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_array_equal(weights, expected_weights)
    for shot, edge, pair in zip(shots, edges, pairs):
        assert canonical_edges(matching.decode_to_edges_array(shot)) == canonical_edges(edge)
        assert matching.decode_to_matched_dets_dict(shot) == pair


def test_static_grid_other_component_recovers_after_unsatisfiable_shot():
    matching = pymatching.Matching()
    matching.add_edge(0, 2, weight=2, fault_ids={0})
    matching.add_edge(1, 3, weight=3, fault_ids={1})
    matching.configure_static_grid(
        distance=3, rounds=3, geometry="unrotated",
        detector_coordinates=[[0, 0, 0], [0, 0, 0], [2, 0, 0], [2, 0, 0]])
    for _ in range(3):
        with pytest.raises(ValueError, match="No perfect matching"):
            matching.decode([1, 1, 1, 0])
        np.testing.assert_array_equal(matching.decode([1, 1, 1, 1]), [1, 1])
    np.testing.assert_array_equal(matching.decode([0, 0, 0, 0]), [0, 0])


@pytest.mark.parametrize("geometry,simd", itertools.product(["rotated", "unrotated"], SIMD_VARIANTS))
def test_static_grid_distinct_stencils_with_interleaved_ids_and_search(geometry, simd):
    # These ordinary edges need different twelve-direction stencils. The two
    # components overlap in space and their user labels do not encode X or Z.
    lattice_coordinates = [(1, 1, 0), (1, 1, 0), (0, 0, 1), (0, 2, 1)]
    coordinates = [
        [2 * (x - y), 2 * (x + y), t] if geometry == "rotated" else [2 * x, 2 * y, t]
        for x, y, t in lattice_coordinates
    ]
    matching = pymatching.Matching()
    matching.add_edge(0, 2, weight=2, fault_ids={0, 79})
    matching.add_edge(1, 3, weight=3, fault_ids={65, 79})
    matching.add_boundary_edge(0, weight=7, fault_ids={79})
    matching.add_boundary_edge(1, weight=11, fault_ids={65})
    matching.ensure_num_fault_ids(80)
    shots = np.array(list(itertools.product([0, 1], repeat=4)), dtype=np.uint8)
    expected, expected_weights = matching.decode_batch(shots, return_weights=True)
    expected_edges = [matching.decode_to_edges_array(shot) for shot in shots]
    configure_grid_for_test(matching,
        distance=3, rounds=3, geometry=geometry, detector_coordinates=coordinates,
        groups=[23, 7, 23, 7], simd=simd)
    info = matching.static_grid_info
    assert info["independent_decoders"] == 2
    assert {tuple(c["global_detectors"]) for c in info["components"]} == {(0, 2), (1, 3)}
    assert {c["group"] for c in info["components"]} == {7, 23}
    for component in info["components"]:
        assert_storage_info(component)
    for _ in range(3):
        actual, weights = matching.decode_batch(shots, return_weights=True)
        np.testing.assert_array_equal(actual, expected)
        np.testing.assert_array_equal(weights, expected_weights)
        for shot, edges in zip(shots, expected_edges):
            assert canonical_edges(matching.decode_to_edges_array(shot)) == canonical_edges(edges)


@pytest.mark.parametrize("geometry", ["rotated", "unrotated"])
def test_static_grid_rejects_mixed_stencil_without_dropping_edges(geometry):
    # (-1,-1,+1) and (-1,+1,+1) cannot share any supported twelve-direction
    # stencil. A small degree alone is insufficient to accept this component.
    lattice_coordinates = [(1, 1, 0), (0, 0, 1), (0, 2, 1)]
    coordinates = [
        [2 * (x - y), 2 * (x + y), t] if geometry == "rotated" else [2 * x, 2 * y, t]
        for x, y, t in lattice_coordinates
    ]
    matching = pymatching.Matching()
    matching.add_edge(0, 1, weight=2, fault_ids={0})
    matching.add_edge(0, 2, weight=3, fault_ids={1})
    matching.add_boundary_edge(0, weight=7)
    shots = np.array(list(itertools.product([0, 1], repeat=3)), dtype=np.uint8)
    expected, expected_weights = matching.decode_batch(shots, return_weights=True)
    expected_edges = [matching.decode_to_edges_array(shot) for shot in shots]
    with pytest.raises(ValueError, match="[Ss]tencil"):
        matching.configure_static_grid(
            distance=3, rounds=3, geometry=geometry, detector_coordinates=coordinates, simd="scalar")
    assert matching.static_grid_info["backend"] == "generic"
    actual, weights = matching.decode_batch(shots, return_weights=True)
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_array_equal(weights, expected_weights)
    for shot, edges in zip(shots, expected_edges):
        assert canonical_edges(matching.decode_to_edges_array(shot)) == canonical_edges(edges)


@pytest.mark.parametrize("extra_pairs", [
    [(0, -1, 1), (-1, -1, 1)],
    [(-1, 1, 1), (0, 1, 1)],
    [(0, -1, 1), (-1, 1, 1)],
], ids=["rotated_x", "rotated_z", "unrotated"])
def test_static_grid_boundary_cannot_use_an_excluded_direction(extra_pairs):
    # All twelve ordinary directions are occupied at the center. Its extra
    # boundary edge must be rejected, rather than placed in a slot the scalar
    # kernel does not visit. Each individual ordinary direction set is valid.
    pairs = [(0, 0, 1), (1, 0, 0), (0, 1, 0), (-1, 0, 1)] + extra_pairs
    offsets = [tuple(sign * value for value in delta) for delta in pairs for sign in [1, -1]]
    coordinates = [[2, 2, 1]] + [[2 * (1 + dx), 2 * (1 + dy), 1 + dt] for dx, dy, dt in offsets]
    matching = pymatching.Matching()
    for node in range(1, 13):
        matching.add_edge(0, node, weight=node + 2, fault_ids={node % 3})
    matching.add_boundary_edge(0, weight=1, fault_ids={3})
    shots = np.eye(13, dtype=np.uint8)
    expected, expected_weights = matching.decode_batch(shots, return_weights=True)
    with pytest.raises(ValueError, match="[Bb]oundary"):
        matching.configure_static_grid(
            distance=3, rounds=3, geometry="unrotated", detector_coordinates=coordinates, simd="scalar")
    assert matching.static_grid_info["backend"] == "generic"
    actual, weights = matching.decode_batch(shots, return_weights=True)
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_array_equal(weights, expected_weights)
