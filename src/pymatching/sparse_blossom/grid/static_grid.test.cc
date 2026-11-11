// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#include <random>
#if defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "gtest/gtest.h"

#include "pymatching/sparse_blossom/diagram/mwpm_diagram.h"
#include "pymatching/sparse_blossom/driver/backend_user_graph.h"
#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/diagram/mwpm_diagram.h"
#include "pymatching/sparse_blossom/structured/driver/component_decoder.h"
#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/grid/static_grid_kernels.h"

using namespace pm;

namespace pm::structured {
// Exercise the otherwise unreachable 32-bit wrap without a runtime test knob.
struct StaticGridComponentTestAccess {
    static void seed_epoch(ComponentDecoder& decoder, uint32_t epoch, uint32_t last_seen) {
        decoder.shot_epoch = epoch;
        for (auto& address : decoder.detector_addresses)
            address.last_seen_epoch = last_seen;
    }
    static uint32_t epoch(const ComponentDecoder& decoder) {
        return decoder.shot_epoch;
    }
};
}

// A grid layout change must never enlarge or modify a generic decoder object.
static_assert(!std::is_same_v<DetectorNode, structured::DetectorNode>);
static_assert(!std::is_same_v<GraphFillRegion, structured::GraphFillRegion>);
static_assert(!std::is_same_v<SearchDetectorNode, structured::SearchDetectorNode>);
static_assert(!std::is_same_v<Mwpm, structured::Mwpm>);
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(DetectorNode) == 160);
static_assert(sizeof(GraphFillRegion) == 128);
static_assert(sizeof(SearchDetectorNode) == 160);
static_assert(sizeof(structured::DetectorNode) == 64);
static_assert(alignof(structured::DetectorNode) == 64);
#endif

template <typename Node>
concept HasConstructionAdjacency = requires(Node n) {
    n.neighbors;
}
|| requires(Node n) {
    n.neighbor_weights;
}
|| requires(Node n) {
    n.neighbor_observables;
}
|| requires(Node n) {
    n.neighbor_implied_weights;
};
static_assert(!HasConstructionAdjacency<structured::DetectorNode>);

namespace {
template <typename Callback>
void for_test_stencil(structured::CircuitStencilKind kind, Callback&& callback) {
    switch (kind) {
        case structured::CircuitStencilKind::x:
            callback.template operator()<structured::CircuitStencil<structured::CircuitStencilKind::x>>();
            break;
        case structured::CircuitStencilKind::z:
            callback.template operator()<structured::CircuitStencil<structured::CircuitStencilKind::z>>();
            break;
        case structured::CircuitStencilKind::unrotated:
            callback.template operator()<structured::CircuitStencil<structured::CircuitStencilKind::unrotated>>();
            break;
    }
}

std::array<size_t, 12> test_global_directions(structured::CircuitStencilKind kind) {
    // Deliberately independent of CircuitStorage's mapping implementation.
    switch (kind) {
        case structured::CircuitStencilKind::x:
            return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13};
        case structured::CircuitStencilKind::z:
            return {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15};
        case structured::CircuitStencilKind::unrotated:
            return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    }
    throw std::logic_error("Unknown test stencil");
}

template <typename Box>
GridNextEvent reference_matching_event(
    const structured::MatchingGridComponent& c,
    const structured::DetectorNode& source,
    const VaryingCT* radii) {
    GridNextEvent best{SIZE_MAX, GRID_NO_TIME};
    const auto& a = c.collision_states[source.grid_cell];
    const auto ra = radii[a.top_region_id];
    const auto directions = test_global_directions(c.stencil_kind);
    for (size_t i = 0; i < directions.size(); i++) {
        const size_t d = directions[i];
        const size_t slot = structured::STRUCTURED_STORAGE_DIRECTIONS == 12 ? i : d;
        if (!(c.present[source.grid_cell] & (uint16_t{1} << slot)))
            continue;
        const auto delta = SURFACE_OFFSETS[d];
        const ptrdiff_t offset = delta[0] + ptrdiff_t(Box::nx) * delta[1] +
                                 ptrdiff_t(Box::nx * Box::ny) * delta[2];
        const auto& b = c.collision_states[ptrdiff_t(source.grid_cell) + offset];
        if (a.top_region_id == b.top_region_id)
            continue;
        const auto rb = radii[b.top_region_id];
        const int slope_a = int(ra.is_growing()) - int(ra.is_shrinking());
        const int slope_b = int(rb.is_growing()) - int(rb.is_shrinking());
        const int slope_sum = slope_a + slope_b;
        if (slope_sum <= 0)
            continue;
        const int64_t remaining = int64_t(c.weights[source.grid_cell][slot]) -
                                  (ra.y_intercept() + a.wrapped_radius_cached) -
                                  (rb.y_intercept() + b.wrapped_radius_cached);
        // Mathematical floor division is independent of SIMD arithmetic shifts.
        const int64_t time = remaining / slope_sum - (remaining < 0 && remaining % slope_sum != 0);
        if (time < best.second)
            best = {slot, time};
    }
    return best;
}

template <typename Box>
void expect_matching_kernels(
    const structured::MatchingGridComponent& c,
    const structured::DetectorNode& source,
    const VaryingCT* radii,
    GridNextEvent expected) {
    EXPECT_EQ(reference_matching_event<Box>(c, source, radii), expected);
    if (c.matching_next)
        EXPECT_EQ(c.matching_next(c, source, radii), expected);
    for_test_stencil(c.stencil_kind, [&]<typename Stencil>() {
        EXPECT_EQ((structured::grid_matching_scalar<Box, Stencil>(c, source, radii)), expected);
#if PM_GRID_AVX2
        if (grid_avx2_available())
            EXPECT_EQ((structured::grid_matching_avx2<Box, Stencil>(c, source, radii)), expected);
#endif
#if PM_GRID_AVX512VL
        if (grid_avx512vl_available())
            EXPECT_EQ((structured::grid_matching_avx512vl<Box, Stencil>(c, source, radii)), expected);
#endif
#if PM_GRID_AVX512
        if (grid_avx512_available())
            EXPECT_EQ((structured::grid_matching_avx512<Box, Stencil>(c, source, radii)), expected);
#endif
    });
}

std::pair<stim::DetectorErrorModel, StaticGridSpec> surface_model(
    bool rotated, size_t distance = 3, size_t rounds = 0, char basis = 'z') {
    rounds = rounds ? rounds : distance;
    std::string task = rotated ? "rotated_memory_" : "unrotated_memory_";
    task += basis;
    stim::CircuitGenParameters gen(rounds, distance, task);
    gen.after_clifford_depolarization = 0.03;
    gen.after_reset_flip_probability = 0.03;
    gen.before_measure_flip_probability = 0.03;
    auto circuit = stim::generate_surface_code_circuit(gen).circuit;
    auto dem = stim::ErrorAnalyzer::circuit_to_detector_error_model(circuit, true, true, false, 0, false, false);
    StaticGridSpec spec{distance, rounds, rotated, {}, {}, GridSimd::scalar};
    spec.coordinates.resize(dem.count_detectors());
    std::set<uint64_t> indices;
    for (size_t k = 0; k < spec.coordinates.size(); k++)
        indices.insert(k);
    for (const auto& [id, p] : dem.get_detector_coordinates(indices))
        for (size_t k = 0; k < 3; k++)
            spec.coordinates[id][k] = int64_t(p[k]);
    return {std::move(dem), std::move(spec)};
}

std::vector<std::pair<int64_t, int64_t>> canonical_edges(const std::vector<int64_t>& edges) {
    std::vector<std::pair<int64_t, int64_t>> result;
    for (size_t k = 0; k < edges.size(); k += 2)
        result.push_back(std::minmax(edges[k], edges[k + 1]));
    std::sort(result.begin(), result.end());
    return result;
}

void assert_valid_surface_correction(
    MatchingGraph& graph,
    const std::vector<uint64_t>& hits,
    const std::vector<int64_t>& edges,
    total_weight_int minimum_weight) {
    // These surface graphs have nonnegative, already quantized integer weights.
    // Validate the recovered correction directly, without prescribing one of
    // several equally optimal paths or observable classes.
    ASSERT_EQ(graph.negative_weight_sum, 0);
    ASSERT_EQ(edges.size() % 2, 0);
    std::vector<bool> expected_parity(graph.nodes.size()), actual_parity(graph.nodes.size());
    for (auto hit : hits)
        expected_parity[hit] = !expected_parity[hit];
    std::set<std::pair<int64_t, int64_t>> unique_edges;
    total_weight_int recovered_weight = 0;
    for (size_t k = 0; k < edges.size(); k += 2) {
        auto u = edges[k], v = edges[k + 1];
        if (u == -1)
            std::swap(u, v);
        ASSERT_GE(u, 0);
        ASSERT_LT(size_t(u), graph.nodes.size());
        ASSERT_GE(v, -1);
        ASSERT_TRUE(v == -1 || size_t(v) < graph.nodes.size());
        ASSERT_TRUE(unique_edges.insert(std::minmax(u, v)).second);
        auto& node = graph.nodes[u];
        size_t index = node.index_of_neighbor(v == -1 ? nullptr : &graph.nodes[v]);
        ASSERT_LT(index, node.neighbors.size());
        recovered_weight += node.neighbor_weights[index];
        actual_parity[u] = !actual_parity[u];
        if (v != -1)
            actual_parity[v] = !actual_parity[v];
    }
    EXPECT_EQ(actual_parity, expected_parity);
    EXPECT_EQ(recovered_weight, minimum_weight);
}
}  // namespace

TEST(StaticGrid, RadiusStoreOwnershipAndZeroState) {
    structured::RegionPool pool(2);
    auto* region = pool.allocate();
    auto id = region->region_id;
    auto& store = pool.radii;
    const auto* address = store.data();
    region->radius_value() = VaryingCT::frozen(17);
    EXPECT_EQ(store.data()[id], VaryingCT::frozen(17));
    static_assert(!std::is_move_constructible_v<structured::GraphFillRegion>);
    EXPECT_EQ(&region->radius_value(), address + id);
    EXPECT_EQ(store.data()[0], VaryingCT{0});
    pool.release(id);
    EXPECT_EQ(store.data()[id], VaryingCT{0});
    EXPECT_EQ(pool.allocate()->region_id, id);
    EXPECT_EQ(store.data(), address);
    EXPECT_THROW(store.at(0), std::out_of_range);
}

TEST(StaticGrid, StorageMappingPreservesDirectionPairsAndRowSize) {
    constexpr auto count = structured::STRUCTURED_STORAGE_DIRECTIONS;
    static_assert(count == 12 || count == 16);
    static_assert(sizeof(structured::StructuredGridWeightRow) == count * sizeof(weight_int));
    static_assert(alignof(structured::StructuredGridWeightRow) == (count == 12 ? 16 : 64));
    for (auto kind : {structured::CircuitStencilKind::x, structured::CircuitStencilKind::z,
                      structured::CircuitStencilKind::unrotated}) {
        for_test_stencil(kind, [&]<typename Stencil>() {
            using Storage = structured::CircuitStorage<Stencil>;
            const auto expected = test_global_directions(kind);
            uint16_t expected_mask = 0;
            for (size_t i = 0; i < expected.size(); i++) {
                const size_t d = expected[i], slot = count == 12 ? i : d;
                EXPECT_EQ(Storage::directions[i], d);
                EXPECT_EQ(Storage::slots[i], slot);
                EXPECT_EQ(Storage::global_to_slot[d], slot);
                EXPECT_EQ(Storage::slot_to_global[slot], d);
                EXPECT_EQ(Storage::global_to_slot[d ^ 1], slot ^ 1);
                expected_mask |= uint16_t{1} << slot;
            }
            EXPECT_EQ(Storage::mask, expected_mask);
            for (size_t d = 0; d < GRID_DIRECTIONS; d++) {
                if (std::find(expected.begin(), expected.end(), d) == expected.end())
                    EXPECT_EQ(Storage::global_to_slot[d], SIZE_MAX);
            }
        });
    }
}

TEST(StaticGrid, SimdSignedArithmeticMasksAndFirstMinimumAcrossBatches) {
    using Box = StaticGridBox<5, 5, 5>;
    constexpr size_t center = 2 + 2 * Box::nx + 2 * Box::nx * Box::ny;
    for (auto kind : {structured::CircuitStencilKind::x, structured::CircuitStencilKind::z,
                      structured::CircuitStencilKind::unrotated}) {
        structured::MatchingGridComponent c;
        c.stencil_kind = kind;
        c.collision_states.resize(Box::volume, {INT32_MIN, UINT32_MAX});
        c.weights.resize(Box::volume);
        c.present.resize(Box::volume);
        structured::DetectorNode source;
        source.grid_component = &c;
        source.grid_cell = center;
        const auto directions = test_global_directions(kind);
        std::array<size_t, 12> slots, cells;
        for (size_t i = 0; i < directions.size(); i++) {
            const auto delta = SURFACE_OFFSETS[directions[i]];
            cells[i] = center + delta[0] + ptrdiff_t(Box::nx) * delta[1] +
                       ptrdiff_t(Box::nx * Box::ny) * delta[2];
            slots[i] = structured::STRUCTURED_STORAGE_DIRECTIONS == 12 ? i : directions[i];
            c.present[center] |= uint16_t{1} << slots[i];
        }
        std::array<VaryingCT, 3> radii{VaryingCT{0}, VaryingCT{1}, VaryingCT{1}};
        for (int source_slope : {-1, 0, 1}) {
            for (int target_slope : {-1, 0, 1}) {
                for (int64_t base : {-(int64_t{1} << 40), int64_t{0}, int64_t{1} << 40}) {
                    radii[1] = VaryingCT::from_base_and_growth(base, source_slope);
                    radii[2] = VaryingCT::from_base_and_growth(base + 1, target_slope);
                    c.collision_states[center] = {-17, 1};
                    for (size_t i = 0; i < cells.size(); i++) {
                        c.collision_states[cells[i]] = {i % 2 ? INT32_MIN : INT32_MAX, 2};
                        c.weights[center][slots[i]] = std::array<weight_int, 4>{0, 1, 0x80000001U, UINT32_MAX}[i % 4];
                    }
                    expect_matching_kernels<Box>(c, source, radii.data(), reference_matching_event<Box>(c, source, radii.data()));
                    // Same-owner edges cannot collide even with extreme wraps.
                    for (size_t i = 0; i < cells.size(); i += 2)
                        c.collision_states[cells[i]].top_region_id = 1;
                    expect_matching_kernels<Box>(c, source, radii.data(), reference_matching_event<Box>(c, source, radii.data()));
                }
            }
        }
        radii[1] = radii[2] = VaryingCT{1};
        c.collision_states[center] = {0, 1};
        for (size_t i = 0; i < cells.size(); i++) {
            c.collision_states[cells[i]] = {0, 2};
            c.weights[center][slots[i]] = 21;
        }
        // All candidates tie, including candidates separated by 4/8-lane batch
        // boundaries. Removing each winner forces the next exact storage slot.
        for (size_t i = 0; i < slots.size(); i++) {
            expect_matching_kernels<Box>(c, source, radii.data(), {slots[i], 10});
            c.present[center] &= ~(uint16_t{1} << slots[i]);
        }
        expect_matching_kernels<Box>(c, source, radii.data(), {SIZE_MAX, GRID_NO_TIME});
    }
}

TEST(StaticGrid, SingleWeightRowDoesNotReadPastTwelveSlotTail) {
    using Box = StaticGridBox<5, 5, 5>;
    for (auto kind : {structured::CircuitStencilKind::x, structured::CircuitStencilKind::z,
                      structured::CircuitStencilKind::unrotated}) {
        structured::MatchingGridComponent c;
        c.stencil_kind = kind;
        c.weights.resize(1);
        c.present.resize(1);
        ASSERT_EQ(c.weights.capacity(), 1);
        c.collision_states.resize(Box::volume, {INT32_MIN, UINT32_MAX});
        structured::DetectorNode source;
        source.grid_component = &c;
        source.grid_cell = 0;
        c.collision_states[0] = {0, 1};
        const auto directions = test_global_directions(kind);
        const size_t d = directions[10];
        const auto delta = SURFACE_OFFSETS[d];
        const ptrdiff_t offset = delta[0] + ptrdiff_t(Box::nx) * delta[1] +
                                 ptrdiff_t(Box::nx * Box::ny) * delta[2];
        ASSERT_GT(offset, 0);
        const size_t slot = structured::STRUCTURED_STORAGE_DIRECTIONS == 12 ? 10 : d;
        c.present[0] = uint16_t{1} << slot;
        c.weights[0][slot] = UINT32_MAX;
        c.collision_states[offset] = {0, 2};
        std::array<VaryingCT, 3> radii{VaryingCT{0}, VaryingCT{1}, VaryingCT{1}};
        // Under ASan the 12-slot allocation ends immediately after its 48 B row.
        // Every other gathered endpoint contains an invalid radius-table index.
        expect_matching_kernels<Box>(c, source, radii.data(), {slot, int64_t(UINT32_MAX) / 2});
    }
}

#if PM_GRID_AVX512 && defined(__unix__)
namespace {
PM_GRID_AVX512_TARGET void read_last_weight_batch(const weight_int* row, int64_t* out) {
    _mm512_storeu_si512(out, structured::grid_load_weights_avx512<8>(row));
}
}  // namespace
#endif

TEST(StaticGrid, LastWeightBatchDoesNotTouchGuardPage) {
#if PM_GRID_AVX512 && defined(__unix__)
    if (!grid_avx512_available())
        GTEST_SKIP() << "AVX-512 unavailable";
    const long page = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page, 0);
    struct Mapping {
        void* data;
        size_t size;
        ~Mapping() {
            if (data != MAP_FAILED)
                munmap(data, size);
        }
    } mapping{mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), size_t(2 * page)};
    ASSERT_NE(mapping.data, MAP_FAILED);
    ASSERT_EQ(mprotect(static_cast<char*>(mapping.data) + page, page, PROT_NONE), 0);
    constexpr size_t count = structured::STRUCTURED_STORAGE_DIRECTIONS;
    auto* row = reinterpret_cast<weight_int*>(static_cast<char*>(mapping.data) + page - count * sizeof(weight_int));
    for (size_t i = 0; i < count; i++)
        std::construct_at(row + i, weight_int(0x80000000U + i));
    std::array<int64_t, 8> actual;
    read_last_weight_batch(row, actual.data());
    for (size_t lane = 0; lane < actual.size(); lane++)
        EXPECT_EQ(actual[lane], lane + 8 < count ? int64_t(0x80000000U + lane + 8) : 0);
#else
    GTEST_SKIP() << "Guard-page test requires Unix and x86 SIMD compiler support";
#endif
}

TEST(StaticGrid, IndependentComponentsAndVirtualBoundaries) {
    for (bool rotated : {false, true}) {
        auto [dem, spec] = surface_model(rotated);
        auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        user.configure_static_grid(spec);
        EXPECT_FALSE(user.get_structured_mwpm().has_search_graph());
        auto& decoder = user.get_structured_mwpm_with_search_graph();
        ASSERT_EQ(decoder.components().size(), 2);
        EXPECT_TRUE(decoder.has_search_graph());
        auto& first = decoder.components()[0].mwpm;
        auto& second = decoder.components()[1].mwpm;
        EXPECT_NE(&first.flooder.queue, &second.flooder.queue);
        EXPECT_NE(first.flooder.radius_store->data(), second.flooder.radius_store->data());
        EXPECT_NE(first.flooder.regions.get(), second.flooder.regions.get());
        EXPECT_NE(&first.node_arena, &second.node_arena);
        EXPECT_NE(
            first.flooder.graph.grid->components[0]->nodes.data(),
            second.flooder.graph.grid->components[0]->nodes.data());
        first.flooder.queue.cur_time = 17;
        EXPECT_EQ(second.flooder.queue.cur_time, 0);
        first.flooder.queue.reset();
        size_t total_detectors = 0;
        for (const auto& component : decoder.components()) {
            const auto& mwpm = component.mwpm;
            ASSERT_EQ(mwpm.flooder.graph.grid->components.size(), 1);
            total_detectors += component.global_detectors.size();
            EXPECT_EQ(mwpm.flooder.graph.num_nodes, component.global_detectors.size());
            EXPECT_EQ(mwpm.search_flooder.graph.num_nodes, component.global_detectors.size());
            EXPECT_TRUE(mwpm.flooder.graph.nodes.empty());
            EXPECT_TRUE(mwpm.search_flooder.graph.nodes.empty());
            for (const auto& c : mwpm.flooder.graph.grid->components) {
                for (const auto& row : c->weights)
                    EXPECT_EQ(reinterpret_cast<uintptr_t>(row.data()) % alignof(structured::StructuredGridWeightRow), 0);
                EXPECT_GT(std::count(c->kinds.begin(), c->kinds.end(), GridNodeKind::boundary), 0);
                EXPECT_EQ(c->nodes.size(), std::count(c->kinds.begin(), c->kinds.end(), GridNodeKind::detector));
                EXPECT_EQ(c->collision_states.size(), c->kinds.size());
                for (size_t cell = 0; cell < c->kinds.size(); cell++) {
                    EXPECT_EQ(c->collision_states[cell].top_region_id, 0);
                    EXPECT_EQ(c->collision_states[cell].wrapped_radius_cached, 0);
                    if (c->kinds[cell] != GridNodeKind::detector)
                        EXPECT_EQ(c->cell_node_indices[cell], UINT32_MAX);
                }
                for (const auto& node : c->nodes) {
                    EXPECT_EQ(&c->node_at_cell(node.grid_cell), &node);
                    for (size_t h = 0; h < GRID_DIRECTIONS; h++) {
                        if (!node.has_edge(h))
                            continue;
                        auto* other = node.neighbor(h);
                        if (other) {
                            EXPECT_EQ(other->grid_component, node.grid_component);
                            EXPECT_EQ(other->neighbor(h ^ 1), &node);
                            EXPECT_EQ(other->edge_weight(h ^ 1), node.edge_weight(h));
                        }
                    }
                }
            }
        }
        EXPECT_EQ(total_detectors, dem.count_detectors());
    }
}

TEST(StaticGrid, TwelveDirectionComponentsPreserveEveryOriginalEdge) {
    for (bool rotated : {false, true}) {
        for (char basis : {'x', 'z'}) {
            SCOPED_TRACE(std::string(rotated ? "rotated_" : "unrotated_") + basis);
            auto [dem, spec] = surface_model(rotated, 5, 5, basis);
            auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
            auto& generic = user.get_mwpm_with_search_graph();
            user.configure_static_grid(spec);
            auto& structured_decoder = user.get_structured_mwpm_with_search_graph();
            for (const auto& component : structured_decoder.components()) {
                const auto& graph = component.mwpm.flooder.graph;
                const auto& search = component.mwpm.search_flooder.graph;
                const auto& c = *graph.grid->components.at(0);
                uint16_t used_directions = 0;
                for (auto mask : c.present)
                    used_directions |= mask;
                EXPECT_LE(std::popcount(used_directions), 12);
                for (size_t local = 0; local < component.global_detectors.size(); local++) {
                    const auto global = component.global_detectors[local];
                    const auto& old = generic.flooder.graph.nodes[global];
                    const auto& old_search = generic.search_flooder.graph.nodes[global];
                    const auto& node = graph.node(local);
                    const auto& search_node = search.node(local);
                    const auto& search_c = *search_node.grid_component;
                    EXPECT_EQ(std::popcount(c.present[node.grid_cell]), old.neighbors.size());
                    EXPECT_EQ(std::popcount(search_c.present[search_node.grid_cell]), old_search.neighbors.size());
                    for (size_t direction = 0; direction < structured::STRUCTURED_STORAGE_DIRECTIONS; direction++) {
                        ASSERT_EQ(node.has_edge(direction), search_node.has_edge(direction));
                        if (!node.has_edge(direction))
                            continue;
                        const auto* target = node.neighbor(direction);
                        const auto* old_target =
                            target ? &generic.flooder.graph.nodes[component.global_detectors[target->detector_id]]
                                   : nullptr;
                        size_t rank =
                            std::find(old.neighbors.begin(), old.neighbors.end(), old_target) - old.neighbors.begin();
                        ASSERT_LT(rank, old.neighbors.size());
                        EXPECT_EQ(node.edge_weight(direction), old.neighbor_weights[rank]);
                        EXPECT_EQ(node.edge_observables(direction), old.neighbor_observables[rank]);
                        const auto* neighbor = node.neighbor(direction);
                        if (neighbor) {
                            ASSERT_TRUE(neighbor->has_edge(direction ^ 1));
                            EXPECT_EQ(neighbor->neighbor(direction ^ 1), &node);
                            EXPECT_EQ(neighbor->edge_weight(direction ^ 1), node.edge_weight(direction));
                            EXPECT_EQ(neighbor->edge_observables(direction ^ 1), node.edge_observables(direction));
                            ASSERT_NE(old.neighbors[rank], nullptr);
                            EXPECT_EQ(
                                component.global_detectors[neighbor->detector_id],
                                size_t(old.neighbors[rank] - generic.flooder.graph.nodes.data()));
                        } else {
                            EXPECT_EQ(old.neighbors[rank], nullptr);
                        }
                        const auto* search_target = search_node.neighbor(direction);
                        const auto* old_search_target =
                            search_target ? &generic.search_flooder.graph
                                                 .nodes[component.global_detectors[search_target->detector_id]]
                                          : nullptr;
                        size_t search_rank =
                            std::find(old_search.neighbors.begin(), old_search.neighbors.end(), old_search_target) -
                            old_search.neighbors.begin();
                        ASSERT_LT(search_rank, old_search.neighbors.size());
                        EXPECT_EQ(search_node.edge_weight(direction), old_search.neighbor_weights[search_rank]);
                        EXPECT_EQ(
                            search_node.edge_observables(direction),
                            old_search.neighbor_observable_indices[search_rank]);
                        const auto* search_neighbor = search_node.neighbor(direction);
                        if (search_neighbor) {
                            ASSERT_TRUE(search_neighbor->has_edge(direction ^ 1));
                            EXPECT_EQ(search_neighbor->neighbor(direction ^ 1), &search_node);
                            ASSERT_NE(old_search.neighbors[search_rank], nullptr);
                            EXPECT_EQ(
                                component.global_detectors[search_neighbor->detector_id],
                                size_t(old_search.neighbors[search_rank] - generic.search_flooder.graph.nodes.data()));
                        } else {
                            EXPECT_EQ(old_search.neighbors[search_rank], nullptr);
                        }
                    }
                }
            }
        }
    }
}

TEST(StaticGrid, ReusedDecoderMatchesGenericSurfaceGraphs) {
    for (bool rotated : {false, true}) {
        auto [dem, spec] = surface_model(rotated);
        auto generic = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
        auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        user.configure_static_grid(spec);
        auto& grid = user.get_structured_mwpm_with_search_graph();
        std::mt19937_64 rng(5);
        for (size_t shot = 0; shot < 400; shot++) {
            std::vector<uint64_t> hits;
            for (size_t k = 0; k < dem.count_detectors(); k++)
                if (rng() % 5 == 0)
                    hits.push_back(k);
            auto expected = decode_detection_events_for_up_to_64_observables(generic, hits, false);
            auto actual = decode_detection_events_for_up_to_64_observables(grid, hits, false);
            ASSERT_EQ(actual.weight, expected.weight) << "rotated=" << rotated << " shot=" << shot;
            std::vector<int64_t> expected_edges, actual_edges;
            decode_detection_events_to_edges(generic, hits, expected_edges);
            decode_detection_events_to_edges(grid, hits, actual_edges);
            assert_valid_surface_correction(generic.flooder.graph, hits, expected_edges, expected.weight);
            assert_valid_surface_correction(generic.flooder.graph, hits, actual_edges, expected.weight);
            for (const auto& component : grid.components()) {
                EXPECT_EQ(component.mwpm.flooder.radius_store->data()[0], VaryingCT{0});
                for (const auto& c : component.mwpm.flooder.graph.grid->components)
                    for (const auto& state : c->collision_states) {
                        EXPECT_EQ(state.top_region_id, 0);
                        EXPECT_EQ(state.wrapped_radius_cached, 0);
                    }
            }
        }
        for (auto& component : grid.components())
            component.mwpm.reset();
        EXPECT_EQ(
            decode_detection_events_for_up_to_64_observables(grid, {0, 1}, false),
            decode_detection_events_for_up_to_64_observables(generic, {0, 1}, false));
    }
}

TEST(StaticGrid, AllSimdKernelsMatchIndependentCollisionFormula) {
    std::array<VaryingCT, 9> radii;
    std::mt19937_64 rng(42);
    using Box = StaticGridBox<5, 5, 6>;
    for (bool rotated : {false, true}) {
        for (char basis : {'x', 'z'}) {
            auto [dem, spec] = surface_model(rotated, 3, 3, basis);
            auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
            user.configure_static_grid(spec);
            for (auto& component : user.get_structured_mwpm().components()) {
                auto& graph = component.mwpm.flooder.graph;
                for (size_t iteration = 0; iteration < 250; iteration++) {
                    radii[0] = VaryingCT{0};
                    for (size_t id = 1; id < radii.size(); id++)
                        radii[id] = VaryingCT::from_base_and_growth(int64_t(rng() % 1024) - 512, int(rng() % 3) - 1);
                    for (auto& c : graph.grid->components) {
                        for (auto& n : c->nodes) {
                            auto& state = c->collision_states[n.grid_cell];
                            state.top_region_id = rng() % radii.size();
                            state.wrapped_radius_cached = state.top_region_id ? int32_t(rng() % 256) - 128 : 0;
                        }
                        for (auto& n : c->nodes) {
                            expect_matching_kernels<Box>(
                                *c, n, radii.data(), reference_matching_event<Box>(*c, n, radii.data()));
                        }
                    }
                }
            }
        }
    }
}

TEST(StaticGrid, InvalidConfigurationPreservesDecoder) {
    auto [dem, spec] = surface_model(true);
    auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto expected = decode_detection_events_for_up_to_64_observables(user.get_mwpm(), {0, 1}, false);
    spec.coordinates[1] = spec.coordinates[0];
    EXPECT_THROW(user.configure_static_grid(spec), std::invalid_argument);
    EXPECT_FALSE(user.static_grid_spec().has_value());
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(user.get_mwpm(), {0, 1}, false), expected);
}

TEST(StaticGrid, FailedStencilConstructionPreservesAdjacencyForRetry) {
    structured::MatchingGraph graph(3, 2);
    graph.add_edge(0, 1, 2, {0});
    graph.add_edge(0, 2, 3, {1});
    graph.add_boundary_edge(0, 7, {});
    const auto original = graph.nodes;
    const auto* original_address = graph.nodes.data();
    StaticGridSpec spec{3, 3, false, {{{2, 2, 0}}, {{0, 0, 1}}, {{0, 4, 1}}}, {}, GridSimd::scalar};
    EXPECT_THROW(graph.configure_grid(spec), std::invalid_argument);
    EXPECT_EQ(graph.grid, nullptr);
    EXPECT_EQ(graph.nodes.data(), original_address);
    ASSERT_EQ(graph.nodes.size(), original.size());
    for (size_t id = 0; id < original.size(); id++) {
        EXPECT_EQ(graph.nodes[id].neighbors, original[id].neighbors);
        EXPECT_EQ(graph.nodes[id].neighbor_weights, original[id].neighbor_weights);
        EXPECT_EQ(graph.nodes[id].neighbor_observables, original[id].neighbor_observables);
        EXPECT_EQ(graph.nodes[id].neighbor_implied_weights.size(), original[id].neighbor_implied_weights.size());
    }
    spec.coordinates[2] = {4, 2, 0};
    ASSERT_NO_THROW(graph.configure_grid(spec));
    ASSERT_NE(graph.grid, nullptr);
    EXPECT_TRUE(graph.nodes.empty());
    EXPECT_EQ(graph.nodes.capacity(), 0);
    auto& source = graph.node(0);
    auto first = source.index_of_neighbor(&graph.node(1));
    auto second = source.index_of_neighbor(&graph.node(2));
    auto boundary = source.index_of_neighbor(nullptr);
    ASSERT_NE(first, SIZE_MAX);
    ASSERT_NE(second, SIZE_MAX);
    ASSERT_NE(boundary, SIZE_MAX);
    EXPECT_EQ(source.edge_weight(first), 2);
    EXPECT_EQ(source.edge_weight(second), 3);
    EXPECT_EQ(source.edge_weight(boundary), 7);
    EXPECT_EQ(source.edge_observables(first), obs_int{1});
    EXPECT_EQ(source.edge_observables(second), obs_int{2});
}

TEST(StaticGrid, UniformEventsRespectSlopesAndDirectionOrder) {
    auto [dem, spec] = surface_model(true);
    auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    user.configure_static_grid(spec);
    auto& graph = user.get_structured_mwpm().components().at(0).mwpm.flooder.graph;
    using Box = StaticGridBox<5, 5, 6>;
    std::array<VaryingCT, 3> radii{VaryingCT{0}, VaryingCT{1}, VaryingCT{0}};
    for (auto& c : graph.grid->components) {
        for (auto& source : c->nodes) {
            if (c->kinds[source.grid_cell] != GridNodeKind::detector)
                continue;
            c->collision_states[source.grid_cell].top_region_id = 1;
            size_t expected = SIZE_MAX;
            for (size_t d = 0; d < GRID_DIRECTIONS; d++) {
                if (!source.has_edge(d))
                    continue;
                c->weights[source.grid_cell][d] = 20;
                expected = std::min(expected, d);
            }
            auto check = [&](GridNextEvent want) {
                expect_matching_kernels<Box>(*c, source, radii.data(), want);
            };
            check({expected, 20});
            // Frozen/empty and shrinking/empty pairs have no positive slope sum.
            radii[1] = VaryingCT::frozen(0);
            check({SIZE_MAX, GRID_NO_TIME});
            radii[1] = VaryingCT::from_base_and_growth(0, -1);
            for (size_t d = 0; d < GRID_DIRECTIONS; d++) {
                if (source.has_edge(d) && source.neighbor(d))
                    c->collision_states[source.neighbor(d)->grid_cell].top_region_id = 2;
            }
            radii[2] = VaryingCT::from_base_and_growth(0, 1);
            check({SIZE_MAX, GRID_NO_TIME});
            for (auto& state : c->collision_states)
                state.top_region_id = 0;
            radii[1] = VaryingCT::from_base_and_growth(0, 1);
            radii[2] = VaryingCT::frozen(0);
        }
    }
}

TEST(StaticGrid, DiagramUsesOriginalDetectorIds) {
    auto [dem, spec] = surface_model(true);
    auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    user.configure_static_grid(spec);
    auto& component = user.get_structured_mwpm().components().at(0);
    auto& decoder = component.mwpm;
    auto [global_coordinates, global_boundaries] = pick_coords_for_drawing_from_dem(dem, 10);
    std::vector<std::pair<float, float>> coordinates, boundaries;
    for (auto global_id : component.global_detectors) {
        coordinates.push_back(global_coordinates[global_id]);
        boundaries.push_back(global_boundaries[global_id]);
    }
    for (size_t id : {0, 1, 3, 5, 8, 11})
        decoder.create_detection_event(&decoder.flooder.graph.node(id));
    for (size_t step = 0; step < 1000; step++) {
        auto event = decoder.flooder.run_until_next_mwpm_notification();
        std::stringstream svg;
        write_decoder_state_as_svg(coordinates, boundaries, decoder, event, svg);
        EXPECT_NE(svg.str().find("<svg"), std::string::npos);
        EXPECT_NE(svg.str().find("</svg>"), std::string::npos);
        if (event.event_type == structured::NO_EVENT) {
            decoder.reset();
            return;
        }
        decoder.process_event(event);
    }
    FAIL() << "Grid diagram decode did not terminate";
}

TEST(StaticGrid, CollisionStateTracksRegionTransitions) {
    for (bool rotated : {false, true}) {
        auto [dem, spec] = surface_model(rotated, 5);
        auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        user.configure_static_grid(spec);
        for (auto& component : user.get_structured_mwpm().components()) {
            auto& decoder = component.mwpm;
            std::mt19937_64 rng(62);
            for (size_t shot = 0; shot < 50; shot++) {
                for (size_t id = 0; id < component.global_detectors.size(); id++)
                    if (rng() % 5 == 0)
                        decoder.create_detection_event(&decoder.flooder.graph.node(id));
                bool done = false;
                for (size_t step = 0; step < 10000; step++) {
                    auto event = decoder.flooder.run_until_next_mwpm_notification();
                    for (const auto& c : decoder.flooder.graph.grid->components) {
                        for (const auto& node : c->nodes) {
                            const auto& state = c->collision_states[node.grid_cell];
                            const auto* region = node.top_region();
                            EXPECT_EQ(state.top_region_id, region ? region->region_id : 0);
                            EXPECT_EQ(state.wrapped_radius_cached, node.compute_wrapped_radius());
                            EXPECT_EQ(
                                node.local_radius(),
                                decoder.flooder.radius_store->data()[state.top_region_id] +
                                    state.wrapped_radius_cached);
                        }
                        for (size_t cell = 0; cell < c->kinds.size(); cell++)
                            if (c->kinds[cell] != GridNodeKind::detector) {
                                EXPECT_EQ(c->collision_states[cell].top_region_id, 0);
                                EXPECT_EQ(c->collision_states[cell].wrapped_radius_cached, 0);
                            }
                    }
                    if (event.event_type == structured::NO_EVENT) {
                        done = true;
                        break;
                    }
                    decoder.process_event(event);
                }
                ASSERT_TRUE(done);
                decoder.reset();
            }
        }
    }
}

TEST(StaticGrid, MaskedDirectionsAndSignedPackedState) {
    auto [dem, spec] = surface_model(true);
    auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    user.configure_static_grid(spec);
    using Box = StaticGridBox<5, 5, 6>;
    auto& c = *user.get_structured_mwpm().components().at(0).mwpm.flooder.graph.grid->components[0];
    auto& source = c.nodes[0];
    uint16_t circuit_directions = 0;
    for (auto mask : c.present)
        circuit_directions |= mask;
    ASSERT_LE(std::popcount(circuit_directions), 12);
    std::array<VaryingCT, 3> radii{VaryingCT{0}, VaryingCT{1}, VaryingCT{1}};
    auto check = [&](GridNextEvent expected) {
        expect_matching_kernels<Box>(c, source, radii.data(), expected);
    };
    // Invalid lanes must never use their deliberately out-of-range region ids.
    for (auto& state : c.collision_states)
        state = {INT32_MIN, UINT32_MAX};
    c.collision_states[source.grid_cell] = {0, 1};
    c.present[source.grid_cell] = 0;
    check({SIZE_MAX, GRID_NO_TIME});
    for (size_t d = 0; d < GRID_DIRECTIONS; d++) {
        if (!(circuit_directions & (uint16_t{1} << d)))
            continue;
        size_t cell = ptrdiff_t(source.grid_cell) + c.selected_offset(d);
        c.present[source.grid_cell] = uint16_t{1} << d;
        c.weights[source.grid_cell][d] = 20;
        for (int32_t wrapped : {INT32_MIN, -128, 0, 128, INT32_MAX}) {
            c.collision_states[cell] = {wrapped, 2};
            check({d, (int64_t{20} - wrapped) >> 1});
        }
        c.collision_states[cell] = {INT32_MIN, UINT32_MAX};
    }
}

TEST(StaticGrid, MoveWithActiveRegionsPreservesNodeAddresses) {
    for (bool rotated : {false, true}) {
        for (auto simd : {GridSimd::scalar, GridSimd::automatic}) {
            auto [dem, spec] = surface_model(rotated, 5);
            spec.simd = simd;
            auto generic = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
            auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
            user.configure_static_grid(spec);
            auto& component = user.get_structured_mwpm_with_search_graph().components().at(0);
            auto& decoder = component.mwpm;
            auto pointers = decoder.flooder.graph.grid->detector_nodes;
            auto search_pointers = decoder.search_flooder.graph.grid->detector_nodes;
            auto* radii = decoder.flooder.radius_store->data();
            std::vector<uint64_t> hits{0, 1, 3, 5, 8, 11};
            std::vector<uint64_t> global_hits;
            for (auto id : hits)
                global_hits.push_back(component.global_detectors[id]);
            auto expected = decode_detection_events_for_up_to_64_observables(generic, global_hits, false);
            for (auto id : hits)
                decoder.create_detection_event(&decoder.flooder.graph.node(id));
            auto first = decoder.flooder.run_until_next_mwpm_notification();
            ASSERT_NE(first.event_type, structured::NO_EVENT);
            decoder.process_event(first);

            structured::Mwpm moved(std::move(decoder));
            structured::Mwpm assigned;
            assigned = std::move(moved);
            EXPECT_EQ(assigned.flooder.graph.grid->detector_nodes, pointers);
            EXPECT_EQ(assigned.search_flooder.graph.grid->detector_nodes, search_pointers);
            EXPECT_EQ(assigned.flooder.radius_store->data(), radii);
            bool done = false;
            for (size_t step = 0; step < 10000; step++) {
                auto event = assigned.flooder.run_until_next_mwpm_notification();
                if (event.event_type == structured::NO_EVENT) {
                    done = true;
                    break;
                }
                assigned.process_event(event);
            }
            ASSERT_TRUE(done);
            MatchingResult actual;
            for (auto id : hits) {
                auto& node = assigned.flooder.graph.node(id);
                if (node.region_that_arrived)
                    actual += assigned.shatter_blossom_and_extract_matches(node.top_region());
            }
            EXPECT_EQ(actual, expected);
            assigned.reset();
            for (const auto* node : pointers)
                EXPECT_EQ(&node->grid_component->node_at_cell(node->grid_cell), node);
            EXPECT_EQ(decode_detection_events_for_up_to_64_observables(assigned, hits, false), expected);
        }
    }
}

TEST(StaticGrid, MovePreservesReweightedEdgeAddresses) {
    auto [dem, spec] = surface_model(true);
    auto user = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm().components().at(0).mwpm;
    auto& source = decoder.flooder.graph.node(0);
    for (size_t d = 0; d < GRID_DIRECTIONS; d++) {
        if (!source.has_edge(d) || !source.neighbor(d))
            continue;
        auto* other = source.neighbor(d);
        auto* forward = &source.edge_weight(d);
        auto* backward = &other->edge_weight(d ^ 1);
        weight_int original = *forward;
        ASSERT_GT(original, 0);
        std::vector<ImpliedWeight> rules{{forward, backward, original / 2}};
        decoder.flooder.graph.reweight(rules);
        structured::Mwpm moved(std::move(decoder));
        EXPECT_EQ(&moved.flooder.graph.node(0).edge_weight(d), forward);
        EXPECT_EQ(&other->edge_weight(d ^ 1), backward);
        EXPECT_EQ(*forward, original / 2);
        EXPECT_EQ(*backward, original / 2);
        moved.flooder.graph.undo_reweights();
        EXPECT_EQ(*forward, original);
        EXPECT_EQ(*backward, original);
        return;
    }
    FAIL() << "Expected an ordinary edge from the first detector";
}

TEST(StaticGrid, IndependentDecodersUseGlobalIdsAndIgnoreSharedBoundary) {
    BackendUserGraph user(5, 2);
    user.add_or_merge_edge(0, 2, {0}, 2, -1);
    user.add_or_merge_edge(1, 3, {1}, 4, -1);
    user.add_or_merge_edge(2, 4, {0}, 6, -1);
    user.add_or_merge_edge(3, 4, {1}, 8, -1);
    user.set_boundary({4});
    StaticGridSpec spec{
        3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}, {{4, 0, 0}}}, {}, GridSimd::scalar};
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm_with_search_graph();
    ASSERT_EQ(decoder.components().size(), 2);
    EXPECT_EQ(decoder.components()[0].global_detectors, (std::vector<size_t>{0, 2}));
    EXPECT_EQ(decoder.components()[1].global_detectors, (std::vector<size_t>{1, 3}));
    for (size_t bits = 0; bits < 32; bits++) {
        std::vector<uint64_t> hits;
        for (size_t i = 0; i < 5; i++)
            if ((bits >> i) & 1)
                hits.push_back(i);
        EXPECT_EQ(decoder.decode(hits), decode_detection_events_for_up_to_64_observables(user.get_mwpm(), hits, false));
        std::vector<int64_t> expected, actual;
        decode_detection_events_to_edges(user.get_mwpm_with_search_graph(), hits, expected);
        decode_detection_events_to_edges(decoder, hits, actual);
        EXPECT_EQ(canonical_edges(actual), canonical_edges(expected));
    }
}

TEST(StaticGrid, IndependentDecodersKeepGlobalQuantizationAndNegativeWeights) {
    BackendUserGraph user(4, 3);
    user.add_or_merge_edge(0, 2, {0}, -0.713, -1);
    user.add_or_merge_edge(1, 3, {1}, 190.123, -1);
    user.add_or_merge_boundary_edge(0, {2}, 1.029, -1);
    user.add_or_merge_boundary_edge(1, {}, -212.983, -1);
    user.add_or_merge_edge(3, 3, {2}, -3.71, -1);
    StaticGridSpec spec{3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}}, {}, GridSimd::scalar};
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm();
    EXPECT_EQ(decoder.normalising_constant(), user.get_mwpm().flooder.graph.normalising_constant);
    for (const auto& c : decoder.components())
        EXPECT_EQ(c.mwpm.flooder.graph.normalising_constant, decoder.normalising_constant());
    for (size_t repeat = 0; repeat < 3; repeat++)
        for (size_t bits = 0; bits < 16; bits++) {
            std::vector<uint64_t> hits;
            for (size_t i = 0; i < 4; i++)
                if ((bits >> i) & 1)
                    hits.push_back(i);
            auto expected_result = decode_detection_events_for_up_to_64_observables(user.get_mwpm(), hits, false);
            EXPECT_EQ(decoder.decode(hits), expected_result);
            EXPECT_EQ(decoder.decode_partitioned(decoder.partition_detection_events(hits)), expected_result);
        }
}

TEST(StaticGrid, EqualExplicitLabelsDoNotMergeDisconnectedComponents) {
    BackendUserGraph user(4, 2);
    user.add_or_merge_edge(0, 2, {0}, 2, -1);
    user.add_or_merge_edge(1, 3, {1}, 3, -1);
    StaticGridSpec spec{
        3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}}, {7, 7, 7, 7}, GridSimd::scalar};
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm();
    ASSERT_EQ(decoder.components().size(), 2);
    for (const auto& component : decoder.components()) {
        EXPECT_EQ(component.global_detectors.size(), 2);
        EXPECT_EQ(component.mwpm.flooder.graph.grid->components.at(0)->group, 7);
    }
    EXPECT_EQ(
        decoder.decode({0, 1, 2, 3}),
        decode_detection_events_for_up_to_64_observables(user.get_mwpm(), {0, 1, 2, 3}, false));
}

TEST(StaticGrid, IndependentDecodersMergeManyObservablesAdditively) {
    BackendUserGraph user(4, 80);
    user.add_or_merge_edge(0, 2, {0, 79}, -2, -1);
    user.add_or_merge_edge(1, 3, {65, 79}, 3, -1);
    user.add_or_merge_boundary_edge(0, {79}, 7, -1);
    user.add_or_merge_boundary_edge(1, {65}, 11, -1);
    StaticGridSpec spec{3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}}, {}, GridSimd::scalar};
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm();
    ASSERT_TRUE(decoder.has_search_graph());
    for (size_t bits = 0; bits < 16; bits++) {
        std::vector<uint64_t> hits;
        for (size_t i = 0; i < 4; i++)
            if ((bits >> i) & 1)
                hits.push_back(i);
        std::vector<uint8_t> expected(80), actual(80);
        total_weight_int expected_weight = 0, actual_weight = 0;
        decode_detection_events(user.get_mwpm(), hits, expected.data(), expected_weight, false);
        decoder.decode(hits, actual.data(), actual_weight);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(actual_weight, expected_weight);
        decoder.decode(hits, actual.data(), actual_weight);
        EXPECT_EQ(actual, std::vector<uint8_t>(80));
        EXPECT_EQ(actual_weight, 2 * expected_weight);
    }
}

TEST(StaticGrid, ComponentFailureDoesNotContaminateOtherDecoder) {
    BackendUserGraph user(4, 2);
    user.add_or_merge_edge(0, 2, {0}, 2, -1);
    user.add_or_merge_edge(1, 3, {1}, 3, -1);
    StaticGridSpec spec{3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}}, {}, GridSimd::scalar};
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm();
    for (size_t repeat = 0; repeat < 3; repeat++) {
        EXPECT_THROW(decoder.decode({0, 2, 1}), std::invalid_argument);
        EXPECT_THROW(decoder.decode({0, 2, UINT64_MAX}), std::invalid_argument);
        EXPECT_EQ(
            decoder.decode({0, 1, 2, 3}),
            decode_detection_events_for_up_to_64_observables(user.get_mwpm(), {0, 1, 2, 3}, false));
        EXPECT_EQ(decoder.decode({}), MatchingResult());
    }
}

TEST(StaticGrid, CorrelatedDecodeIsRejectedAtStructuredEntry) {
    auto [dem, spec] = surface_model(true);
    auto user = detector_error_model_to_backend_user_graph(dem, true, NUM_DISTINCT_WEIGHTS);
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm_with_search_graph();
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, true), std::invalid_argument);
    std::vector<int64_t> edges;
    EXPECT_THROW(
        decode_detection_events_to_edges_with_edge_correlations(decoder, {0, 1}, edges), std::invalid_argument);
    EXPECT_EQ(decoder.decode({0, 1}), decode_detection_events_for_up_to_64_observables(user.get_mwpm(), {0, 1}, false));
    EXPECT_NO_THROW(decode_detection_events_for_up_to_64_observables(user.get_mwpm_with_search_graph(), {0, 1}, true));
}

TEST(StaticGrid, ShortestPathsUseGlobalIdsAndRejectUnreachableBoundary) {
    BackendUserGraph user(4, 2);
    user.add_or_merge_edge(0, 2, {0}, 2, -1);
    user.add_or_merge_edge(1, 3, {1}, 3, -1);
    StaticGridSpec spec{3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}}, {}, GridSimd::scalar};
    user.configure_static_grid(spec);
    auto& decoder = user.get_structured_mwpm_with_search_graph();
    std::vector<size_t> path;
    decoder.get_nodes_on_shortest_path_from_source(2, 2, path);
    EXPECT_EQ(path, (std::vector<size_t>{2}));
    path.clear();
    decoder.get_nodes_on_shortest_path_from_source(1, 3, path);
    EXPECT_EQ(path, (std::vector<size_t>{1, 3}));
    path.clear();
    EXPECT_THROW(decoder.get_nodes_on_shortest_path_from_source(0, SIZE_MAX, path), std::invalid_argument);
    EXPECT_THROW(decoder.get_nodes_on_shortest_path_from_source(SIZE_MAX, 0, path), std::invalid_argument);
    EXPECT_THROW(decoder.get_nodes_on_shortest_path_from_source(0, 1, path), std::invalid_argument);
    EXPECT_TRUE(path.empty());
    EXPECT_EQ(
        decoder.decode({0, 1, 2, 3}),
        decode_detection_events_for_up_to_64_observables(user.get_mwpm(), {0, 1, 2, 3}, false));
}

TEST(StaticGrid, LargeDistanceRegistryIncludesThreeDistanceRounds) {
    std::vector<GridSimd> kernels{GridSimd::scalar};
    if (grid_avx512vl_available())
        kernels.push_back(GridSimd::avx512vl);
    for (size_t distance : {21, 25, 29, 33})
        for (size_t rounds : {distance, 3 * distance})
            for (auto simd : kernels) {
                SCOPED_TRACE(::testing::Message() << "distance=" << distance << " rounds=" << rounds
                                                << " simd=" << static_cast<int>(simd));
                BackendUserGraph user(2, 1);
                user.add_or_merge_edge(0, 1, {0}, 2, -1);
                user.add_or_merge_boundary_edge(0, {}, 3, -1);
                StaticGridSpec spec{distance, rounds, false, {{{0, 0, 0}}, {{2, 0, 0}}}, {}, simd};
                user.configure_static_grid(spec);
                auto& decoder = user.get_structured_mwpm_with_search_graph();
                EXPECT_EQ(decoder.components().size(), 1);
                const auto& grid = *decoder.components().front().mwpm.flooder.graph.grid->components.front();
                EXPECT_EQ(grid.nx, distance + 2);
                EXPECT_EQ(grid.nt, rounds + 3);
                EXPECT_EQ(grid.selected_simd, simd);
                for (const std::vector<uint64_t>& hits :
                     {std::vector<uint64_t>{0, 1}, {0}, {}, {1}, {0, 1}})
                    EXPECT_EQ(
                        decoder.decode(hits),
                        decode_detection_events_for_up_to_64_observables(user.get_mwpm(), hits, false));
                std::vector<size_t> path;
                decoder.get_nodes_on_shortest_path_from_source(0, 1, path);
                EXPECT_EQ(path, (std::vector<size_t>{0, 1}));
            }
}

TEST(StaticGrid, ConstructedCellAccessKeepsInactiveCellsAndFirstMinimumMasked) {
    using Box = StaticGridBox<5, 5, 5>;
    std::mt19937_64 rng(20261003);
    for (auto kind : {structured::CircuitStencilKind::x, structured::CircuitStencilKind::z,
                      structured::CircuitStencilKind::unrotated}) {
        for_test_stencil(kind, [&]<typename Stencil>() {
            structured::MatchingGridComponent c;
            c.stencil_kind = kind;
            c.collision_states.resize(Box::volume);
            c.present.resize(Box::volume);
            c.weights.resize(Box::volume);
            structured::DetectorNode source;
            source.grid_component = &c;
            std::array<VaryingCT, 5> radii{};
            const auto slots = structured::CircuitStorage<Stencil>::slots;
            const uint16_t mask = structured::CircuitStorage<Stencil>::mask;
            const auto bit = [](size_t slot) { return uint16_t(uint16_t{1} << slot); };
            const std::array<uint16_t, 8> tie_masks{
                0, mask, uint16_t(bit(slots[0]) | bit(slots[11])),
                uint16_t(bit(slots[1]) | bit(slots[8])), uint16_t(bit(slots[4]) | bit(slots[11])),
                uint16_t(bit(slots[8]) | bit(slots[11])), bit(slots[11]), bit(slots[0])};
            for (size_t cell : {size_t{31}, size_t{62}, size_t{93}}) {
                source.grid_cell = cell;
                for (size_t iteration = 0; iteration < 96; iteration++) {
                    // This fixture models the constructor's stronger contract:
                    // every cell, including inactive cells, has an in-range ID.
                    // Poisoned inactive-cell and cell-zero tests above still
                    // exercise the default masked specialization unchanged.
                    for (auto& state : c.collision_states)
                        state = {std::bit_cast<int32_t>(uint32_t(rng())), uint32_t(rng() % radii.size())};
                    radii[0] = VaryingCT{0};
                    for (size_t id = 1; id < radii.size(); id++)
                        radii[id] = VaryingCT{std::bit_cast<int64_t>((rng() & ~uint64_t{3}) | (rng() % 3))};
                    c.present[cell] = uint16_t(rng()) & mask;
                    for (size_t slot : slots)
                        c.weights[cell][slot] = uint32_t(rng());
                    if (iteration < tie_masks.size()) {
                        // Equal candidates spanning batches must retain the
                        // original first-slot winner; an empty mask has no event.
                        for (auto& radius : radii)
                            radius = VaryingCT{0};
                        radii[1] = VaryingCT{1};
                        c.collision_states[cell] = {0, 1};
                        c.present[cell] = tie_masks[iteration];
                        for (size_t d : structured::CircuitStorage<Stencil>::directions) {
                            const auto offset = SURFACE_OFFSETS[d];
                            const ptrdiff_t other = ptrdiff_t(cell) + offset[0] + 5 * offset[1] + 25 * offset[2];
                            c.collision_states[other] = {0, 2};
                            c.weights[cell][structured::CircuitStorage<Stencil>::global_to_slot[d]] = 42;
                        }
                    }
                    const auto expected = reference_matching_event<Box>(c, source, radii.data());
                    expect_matching_kernels<Box>(c, source, radii.data(), expected);
#if PM_GRID_AVX2
                    if (grid_avx2_available())
                        EXPECT_EQ((structured::grid_matching_avx2<Box, Stencil, structured::GridCellAccess::constructed>(
                                      c, source, radii.data())), expected);
#endif
#if PM_GRID_AVX512VL
                    if (grid_avx512vl_available())
                        EXPECT_EQ((structured::grid_matching_avx512vl<Box, Stencil, structured::GridCellAccess::constructed>(
                                      c, source, radii.data())), expected);
#endif
#if PM_GRID_AVX512
                    if (grid_avx512_available())
                        EXPECT_EQ((structured::grid_matching_avx512<Box, Stencil, structured::GridCellAccess::constructed>(
                                      c, source, radii.data())), expected);
#endif
                }
            }
        });
    }
}

TEST(StaticGrid, OneShotSeedOwnerPreservesComponentOrderAndMoveLifetime) {
    pm::BackendUserGraph graph(4, 1);
    graph.add_or_merge_edge(0, 2, {0}, 1, 0.1);
    graph.add_or_merge_edge(1, 3, {}, 1, 0.1);
    pm::StaticGridSpec spec{3, 3, false, {}, {}, pm::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}};
    spec.groups = {0, 1, 0, 1};
    graph.configure_static_grid(spec);
    auto& decoder = graph.get_structured_mwpm();
    decoder.prepare_shot({3, 0, 2, 1});
    ASSERT_EQ(decoder.shot_seeds.size(), 4);
    const std::array<uint32_t, 4> expected_local{0, 1, 1, 0};
    for (size_t k = 0; k < 4; k++)
        EXPECT_EQ(decoder.shot_seeds[k].detector_id, expected_local[k]);
    for (const auto& component : decoder.components()) {
        const auto& predecode = component.mwpm.predecoder;
        EXPECT_EQ(predecode.owned_seed_capacity(), 0);
        EXPECT_EQ(predecode.seed_view.storage.data(), decoder.shot_seeds.data() + component.seed_begin);
        for (size_t k = 0; k < predecode.seed_view.storage.size(); k++) {
            const auto& seed = predecode.seed_view.storage[k];
            EXPECT_EQ(seed.grid->predecode.active[seed.cell], k + 1);
        }
    }
    auto moved = std::move(decoder);
    pm::MatchingResult result;
    for (auto& component : moved.components())
        result += pm::structured::decode_prepared_detection_events_for_up_to_64_observables(component.mwpm);
    EXPECT_EQ(result.weight, 4);
    EXPECT_EQ(result.obs_mask, 1);
    EXPECT_THROW(moved.prepare_shot({0, 0}), std::invalid_argument);
    EXPECT_THROW(moved.prepare_shot({0, UINT64_MAX}), std::invalid_argument);
    for (const auto& invalid : std::vector<std::vector<uint64_t>>{{3, 3, 0}, {3, 0, 1, 0}, {0, 0, 0, 0, 0}}) {
        EXPECT_THROW(moved.prepare_shot(invalid), std::invalid_argument);
        EXPECT_TRUE(moved.shot_seeds.empty());
        for (const auto& component : moved.components()) {
            EXPECT_TRUE(component.mwpm.predecoder.seed_view.storage.empty());
            for (const auto& grid : component.mwpm.flooder.graph.grid->components) {
                EXPECT_TRUE(std::all_of(grid->predecode.active.begin(), grid->predecode.active.end(), [](auto x) { return x == 0; }));
                EXPECT_TRUE(std::all_of(grid->predecode.radii.begin(), grid->predecode.radii.end(), [](auto x) { return x == 0; }));
            }
        }
    }
    moved.prepare_shot({3, 0, 2, 1}, false);
    for (size_t k = 0; k < moved.shot_seeds.size(); ++k) {
        const auto& seed = moved.shot_seeds[k];
        EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0);
    }
    for (auto& component : moved.components())
        component.mwpm.predecoder.rebuild_cell_index();
    EXPECT_EQ(moved.decode({0, 2}).weight, 2);
    EXPECT_EQ(moved.decode({}).weight, 0);
    for (const auto& component : moved.components())
        EXPECT_EQ(component.mwpm.predecoder.owned_seed_capacity(), 0);
}

TEST(StaticGrid, ShotSeedNegativeParityAndDeferredIndexRebuild) {
    pm::BackendUserGraph graph(4, 1);
    graph.add_or_merge_edge(0, 2, {0}, -1, 0.1);
    graph.add_or_merge_edge(1, 3, {}, 1, 0.1);
    pm::StaticGridSpec spec{3, 3, false, {}, {}, pm::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}};
    spec.groups = {0, 1, 0, 1};
    graph.configure_static_grid(spec);
    auto& decoder = graph.get_structured_mwpm();
    EXPECT_EQ(decoder.decode({0, 2}).weight, -2);
    EXPECT_TRUE(decoder.shot_seeds.empty());
    decoder.prepare_shot({3, 1}, false);
    ASSERT_EQ(decoder.shot_seeds.size(), 4);
    for (auto& component : decoder.components()) {
        auto& predecode = component.mwpm.predecoder;
        std::reverse(predecode.seed_view.storage.begin(), predecode.seed_view.storage.end());
        predecode.rebuild_cell_index();
        for (size_t k = 0; k < predecode.seed_view.storage.size(); k++)
            EXPECT_EQ(predecode.seed_view.storage[k].grid->predecode.active[predecode.seed_view.storage[k].cell], k + 1);
    }
    pm::MatchingResult result;
    for (auto& component : decoder.components())
        result += pm::structured::decode_prepared_detection_events_for_up_to_64_observables(component.mwpm);
    EXPECT_EQ(result.weight, 2);
    EXPECT_EQ(result.obs_mask, 0);
    EXPECT_EQ(decoder.decode({}).weight, 0);
    auto moved = std::move(decoder);
    pm::structured::ComponentDecoder assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.decode({0, 2}).weight, -2);
    EXPECT_TRUE(assigned.shot_seeds.empty());
}

TEST(StaticGrid, ShotAddressEpochWrapResetAndFailedAttemptReuse) {
    pm::BackendUserGraph graph(5, 1);
    graph.add_or_merge_edge(0, 2, {0}, 1, 0.1);
    graph.add_or_merge_edge(1, 3, {}, 1, 0.1);
    graph.set_boundary({4});
    pm::StaticGridSpec spec{3, 3, false, {}, {}, pm::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}, {{0, 0, 0}}};
    spec.groups = {0, 1, 0, 1, 2};
    graph.configure_static_grid(spec);
    auto& decoder = graph.get_structured_mwpm();
    using Access = pm::structured::StaticGridComponentTestAccess;
    Access::seed_epoch(decoder, UINT32_MAX, 1);
    decoder.prepare_shot({4, 3, 0, 4, 2, 1}, false);
    EXPECT_EQ(Access::epoch(decoder), 1);
    ASSERT_EQ(decoder.shot_seeds.size(), 4);
    for (const auto& seed : decoder.shot_seeds)
        EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0);
    auto moved = std::move(decoder);
    pm::structured::ComponentDecoder assigned;
    assigned = std::move(moved);
    EXPECT_EQ(Access::epoch(assigned), 1);
    for (size_t repeat = 0; repeat < 3; ++repeat) {
        EXPECT_EQ(assigned.decode({3, 0, 2, 1}).weight, 4);
        assigned.reset();
        EXPECT_EQ(Access::epoch(assigned), repeat + 2);
    }
    try {
        assigned.prepare_shot({0, 0, UINT64_MAX});
        FAIL() << "Mixed duplicate/range input must throw.";
    } catch (const std::invalid_argument& ex) {
        EXPECT_NE(std::string(ex.what()).find("not in the graph"), std::string::npos);
    }
    EXPECT_THROW(assigned.prepare_shot({3, 3, 0}), std::invalid_argument);
    EXPECT_TRUE(assigned.shot_seeds.empty());
    EXPECT_EQ(assigned.decode({0, 2}).weight, 2);
    EXPECT_EQ(assigned.decode({4, 4}).weight, 0);
}

TEST(StaticGrid, ShotStaticAddressesAvoidMutableDetectorMetadata) {
    pm::BackendUserGraph graph(2, 1);
    graph.add_or_merge_edge(0, 1, {0}, 1, 0.1);
    pm::StaticGridSpec spec{3, 3, false, {{{0, 0, 0}}, {{2, 0, 0}}}, {}, pm::GridSimd::scalar};
    graph.configure_static_grid(spec);
    auto& decoder = graph.get_structured_mwpm();
    auto& component = decoder.components().front();
    auto& node = component.mwpm.flooder.graph.node(0);
    const auto cell = node.grid_cell;
    auto* grid = node.grid_component;
    node.detector_id = SIZE_MAX;
    node.grid_cell = SIZE_MAX;
    node.grid_component = nullptr;
    // Restore before any decoding; only scatter must use the immutable address.
    EXPECT_NO_THROW(decoder.prepare_shot({0}, false));
    node.detector_id = 0;
    node.grid_cell = cell;
    node.grid_component = grid;
    ASSERT_EQ(decoder.shot_seeds.size(), 1);
    EXPECT_EQ(decoder.shot_seeds[0].detector_id, 0);
    EXPECT_EQ(decoder.shot_seeds[0].cell, cell);
    EXPECT_EQ(decoder.shot_seeds[0].grid, grid);
    EXPECT_EQ(grid->predecode.active[cell], 0);
    component.mwpm.predecoder.rebuild_cell_index();
    EXPECT_EQ(grid->predecode.active[cell], 1);
    EXPECT_EQ(decoder.decode({0, 1}).weight, 2);
}

TEST(StaticGrid, ShotAddressEpochKeepsNegativeCycleCorrections) {
    pm::BackendUserGraph graph(4, 1);
    graph.add_or_merge_edge(0, 1, {0}, -1, 0.1);
    graph.add_or_merge_edge(1, 2, {}, -1, 0.1);
    graph.add_or_merge_edge(2, 3, {}, -1, 0.1);
    graph.add_or_merge_edge(3, 0, {}, -1, 0.1);
    pm::StaticGridSpec spec{
        3, 3, false, {{{0, 0, 0}}, {{2, 0, 0}}, {{2, 2, 0}}, {{0, 2, 0}}}, {}, pm::GridSimd::scalar};
    graph.configure_static_grid(spec);
    auto& decoder = graph.get_structured_mwpm();
    EXPECT_TRUE(decoder.components().front().mwpm.flooder.negative_weight_detection_events.empty());
    const auto empty = decoder.decode({});
    EXPECT_EQ(empty.weight, -8);
    EXPECT_EQ(empty.obs_mask, 1);
    EXPECT_EQ(pm::structured::StaticGridComponentTestAccess::epoch(decoder), 1);
    const auto pair = decoder.decode({0, 1});
    EXPECT_EQ(pair.weight, -6);
    EXPECT_EQ(pair.obs_mask, 0);
}
