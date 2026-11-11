// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pymatching/sparse_blossom/driver/backend_user_graph.h"

namespace pm::structured {
struct SerialSeedStorageTestAccess {
    static std::array<std::pair<uintptr_t, uintptr_t>, 3> allocations(const OneHopPredecoder& pre) {
        const auto bounds = [](const auto& storage) {
            return std::pair{reinterpret_cast<uintptr_t>(storage.data()),
                             reinterpret_cast<uintptr_t>(storage.data() + storage.size())};
        };
        return {bounds(pre.candidate_masks), bounds(pre.degrees), bounds(pre.order)};
    }
};
}  // namespace pm::structured

namespace {
using pm::structured::ComponentDecoder;
using pm::structured::OneHopPredecoder;
using pm::structured::PredecodeSeed;

pm::StaticGridSpec serial_spec(bool ignored = false) {
    pm::StaticGridSpec spec{3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}},
                            {}, pm::GridSimd::scalar};
    if (ignored) spec.coordinates.push_back({0, 0, 2});
    return spec;
}

pm::BackendUserGraph serial_graph(bool negative = false, bool ignored = false) {
    pm::BackendUserGraph graph(ignored ? 5 : 4, 2);
    graph.add_or_merge_edge(0, 2, {0}, negative ? -2 : 2, -1);
    graph.add_or_merge_edge(1, 3, {1}, 3, -1);
    graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
    graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
    if (ignored) graph.set_boundary({4});
    return graph;
}

std::array<uintptr_t, 3> scratch_addresses(const OneHopPredecoder& pre) {
    const auto layouts = pre.scratch_layout();
    EXPECT_EQ(layouts.size(), 1U);
    if (layouts.empty()) return {};
    return {layouts[0].mask_address, layouts[0].degree_address, layouts[0].order_address};
}

void expect_clean_cells(const ComponentDecoder& decoder) {
    for (const auto& component : decoder.components()) {
        for (auto value : component.seed_grid->predecode.active) EXPECT_EQ(value, 0U);
        for (auto value : component.seed_grid->predecode.radii) EXPECT_EQ(value, 0U);
        EXPECT_TRUE(component.mwpm.predecoder.seed_view.empty());
    }
}

void expect_same_predecode(ComponentDecoder& reference, ComponentDecoder& fixed,
                           const std::vector<uint64_t>& events) {
    reference.prepare_shot(events);
    fixed.prepare_shot(events);
    for (size_t c = 0; c < reference.components().size(); ++c) {
        auto& expected = reference.components()[c].mwpm.predecoder;
        auto& actual = fixed.components()[c].mwpm.predecoder;
        expected.run();
        actual.run();
        ASSERT_EQ(actual.seed_view.storage.size(), expected.seed_view.storage.size());
        EXPECT_EQ(actual.result, expected.result);
        EXPECT_EQ(actual.complete, expected.complete);
        for (size_t k = 0; k < actual.seed_view.storage.size(); ++k) {
            const auto& a = actual.seed_view.storage[k];
            const auto& e = expected.seed_view.storage[k];
            EXPECT_EQ(a.detector_id, e.detector_id);
            EXPECT_EQ(a.cell, e.cell);
            EXPECT_EQ(a.mate, e.mate);
            EXPECT_EQ(a.radius, e.radius);
            EXPECT_EQ(a.match_slot, e.match_slot);
            EXPECT_EQ(a.grid->predecode.active[a.cell], k + 1);
        }
    }
    reference.reset_after_shot();
    fixed.reset_after_shot();
}
}  // namespace

TEST(SerialSeedStorage, FixedComponentPrefixesAndAlignedScratchSurviveMoves) {
    auto graph = serial_graph();
    ComponentDecoder decoder(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
    ASSERT_TRUE(decoder.initialize_serial_seed_storage());
    EXPECT_TRUE(decoder.initialize_serial_seed_storage());
    EXPECT_EQ(decoder.serial_seed_storage_capacity(), 4U);
    EXPECT_TRUE(decoder.shot_seeds.empty());
    std::array<std::array<uintptr_t, 3>, 2> scratch{};
    for (size_t c = 0; c < 2; ++c) {
        scratch[c] = scratch_addresses(decoder.components()[c].mwpm.predecoder);
        for (auto address : scratch[c]) EXPECT_EQ(address % 64, 0U);
    }
    decoder.prepare_shot({0, 1, 2, 3});
    std::array<const PredecodeSeed*, 2> bases{};
    for (size_t c = 0; c < 2; ++c) bases[c] = decoder.components()[c].mwpm.predecoder.seed_view.storage.data();
    EXPECT_EQ(bases[1] - bases[0], 2);
    decoder.reset_after_shot();
    expect_clean_cells(decoder);
    decoder.prepare_shot({1, 3});
    ComponentDecoder moved(std::move(decoder));
    for (size_t c = 0; c < 2; ++c) {
        auto& pre = moved.components()[c].mwpm.predecoder;
        EXPECT_EQ(pre.seed_view.storage.data(), bases[c]);
        EXPECT_EQ(pre.seed_view.storage.size(), c ? 2U : 0U);
        pre.prepare_scratch();
        EXPECT_EQ(scratch_addresses(pre), scratch[c]);
    }
    moved.reset_after_shot();
    expect_clean_cells(moved);
    for (const auto& events : std::vector<std::vector<uint64_t>>{{0}, {}, {0, 1, 2, 3}, {3}}) {
        moved.prepare_shot(events);
        for (size_t c = 0; c < 2; ++c) {
            auto& pre = moved.components()[c].mwpm.predecoder;
            EXPECT_EQ(pre.seed_view.storage.data(), bases[c]);
            pre.prepare_scratch();
            EXPECT_EQ(scratch_addresses(pre), scratch[c]);
        }
        moved.reset_after_shot();
        expect_clean_cells(moved);
    }
}

TEST(SerialSeedStorage, UnsortedValidationAndRecovery) {
    auto graph = serial_graph(false, true);
    ComponentDecoder reference(graph, serial_spec(true), pm::NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder fixed(graph, serial_spec(true), pm::NUM_DISTINCT_WEIGHTS, false);
    ASSERT_TRUE(fixed.initialize_serial_seed_storage());
    expect_same_predecode(reference, fixed, {4, 3, 2, 1, 0, 4});
    for (const auto& events : std::vector<std::vector<uint64_t>>{{2, 0, 2, 99, 98}, {0, 0}, {4, 1, 1}}) {
        std::string expected_error;
        try {
            reference.prepare_shot(events);
            FAIL() << "Expected invalid input";
        } catch (const std::invalid_argument& e) { expected_error = e.what(); }
        try {
            fixed.prepare_shot(events);
            FAIL() << "Expected invalid input";
        } catch (const std::invalid_argument& e) { EXPECT_EQ(e.what(), expected_error); }
        expect_clean_cells(fixed);
        expect_same_predecode(reference, fixed, {3, 0, 1, 2});
    }
    expect_same_predecode(reference, fixed, {4, 4});
    expect_same_predecode(reference, fixed, {});
}

TEST(SerialSeedStorage, NegativeNormalizationFallsBack) {
    auto graph = serial_graph(true);
    ComponentDecoder reference(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder fallback(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
    EXPECT_FALSE(fallback.initialize_serial_seed_storage());
    EXPECT_EQ(fallback.serial_seed_storage_capacity(), 0U);
    for (size_t mask = 0; mask < 16; ++mask) {
        std::vector<uint64_t> events;
        for (size_t k = 0; k < 4; ++k) if (mask & (1U << k)) events.push_back(k);
        EXPECT_EQ(fallback.decode(events), reference.decode(events));
    }
}

TEST(SerialSeedStorage, DecodeMatchesDynamicStorage) {
    auto graph = serial_graph();
    ComponentDecoder reference(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, true);
    ComponentDecoder fixed(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, true);
    ASSERT_TRUE(fixed.initialize_serial_seed_storage());
    for (size_t repetition = 0; repetition < 3; ++repetition) {
        for (size_t mask = 0; mask < 16; ++mask) {
            std::vector<uint64_t> events;
            for (size_t k = 0; k < 4; ++k) if (mask & (1U << k)) events.push_back(k);
            if (repetition & 1) std::reverse(events.begin(), events.end());
            expect_same_predecode(reference, fixed, events);
            EXPECT_EQ(fixed.decode(events), reference.decode(events));
            reference.decode_to_match_edges(events);
            fixed.decode_to_match_edges(events);
            EXPECT_EQ(fixed.match_edges, reference.match_edges);
            reference.reset_after_shot();
            fixed.reset_after_shot();
        }
    }
    EXPECT_EQ(fixed.decode_partitioned({{0, 1}, {0, 1}}), reference.decode_partitioned({{0, 1}, {0, 1}}));
    EXPECT_EQ(fixed.decode({1, 3}), reference.decode({1, 3}));
}

TEST(SerialSeedStorage, BuildIndexCanBeDeferred) {
    auto graph = serial_graph();
    ComponentDecoder decoder(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
    ASSERT_TRUE(decoder.initialize_serial_seed_storage());
    decoder.prepare_shot({0, 1, 2, 3}, false);
    for (auto& component : decoder.components()) {
        auto& pre = component.mwpm.predecoder;
        for (const auto& seed : pre.seed_view.storage) EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0U);
        pre.rebuild_cell_index();
        for (size_t k = 0; k < pre.seed_view.storage.size(); ++k) {
            const auto& seed = pre.seed_view.storage[k];
            EXPECT_EQ(seed.grid->predecode.active[seed.cell], k + 1);
        }
    }
    decoder.reset_after_shot();
    expect_clean_cells(decoder);
}

TEST(SerialSeedStorage, LateInitializationLeavesDynamicDecoderUsable) {
    auto graph = serial_graph();
    ComponentDecoder decoder(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder reference(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
    decoder.prepare_shot({0, 2});
    EXPECT_THROW(decoder.initialize_serial_seed_storage(), std::logic_error);
    EXPECT_EQ(decoder.serial_seed_storage_capacity(), 0U);
    EXPECT_EQ(decoder.decode({0, 1, 2, 3}), reference.decode({0, 1, 2, 3}));
}

TEST(SerialSeedStorage, FirstEmptyComponentPublishesZeroLengthScratch) {
    for (const auto& first_events : std::vector<std::vector<uint64_t>>{{}, {0}, {1}}) {
        auto graph = serial_graph();
        ComponentDecoder decoder(graph, serial_spec(), pm::NUM_DISTINCT_WEIGHTS, false);
        ASSERT_TRUE(decoder.initialize_serial_seed_storage());
        std::array<std::array<uintptr_t, 3>, 2> addresses{};
        for (size_t c = 0; c < 2; ++c)
            addresses[c] = scratch_addresses(decoder.components()[c].mwpm.predecoder);
        for (const auto& events : std::vector<std::vector<uint64_t>>{first_events, {0, 1, 2, 3}, {}}) {
            decoder.prepare_for_mwpm(events);
            for (size_t c = 0; c < 2; ++c) {
                const auto& pre = decoder.components()[c].mwpm.predecoder;
                const auto layout = pre.scratch_layout();
                ASSERT_EQ(layout.size(), 1U);
                EXPECT_EQ(layout[0].seed_begin, 0U);
                EXPECT_EQ(layout[0].seed_end, pre.seed_view.storage.size());
                EXPECT_EQ(scratch_addresses(pre), addresses[c]);
                if (pre.seed_view.empty()) EXPECT_EQ(layout[0].seed_end, 0U);
            }
            decoder.reset_after_shot();
        }
    }
}

TEST(SerialSeedStorage, PartitionScratchFitsFixedAllocationThroughLastBucket) {
    constexpr size_t capacity = 257;
    OneHopPredecoder::Grid grid;
    grid.predecode.active.resize(capacity);
    grid.predecode.radii.resize(capacity);
    grid.boundary_mask.resize(capacity);
    const auto degree = [](size_t cell) { return (cell * 7 + 3) % 13; };
    grid.predecode.neighborhood = [](const OneHopPredecoder::Grid&, size_t cell, uint16_t) {
        const uint32_t degree = (cell * 7 + 3) % 13;
        return (degree << 16) | ((uint32_t{1} << degree) - 1);
    };
    std::vector<PredecodeSeed> seeds(capacity);
    for (size_t k = 0; k < seeds.size(); ++k) {
        seeds[k] = {uint32_t(k), uint32_t(k)};
        seeds[k].grid = &grid;
    }
    OneHopPredecoder pre;
    pre.bind_serial_scratch(capacity);
    const auto allocations = pm::structured::SerialSeedStorageTestAccess::allocations(pre);
    const auto dense_addresses = scratch_addresses(pre);
    for (const auto& offsets : std::vector<std::vector<size_t>>{{0, 1, 128, capacity},
                                                               {0, 1, 64, 129, 256, capacity}}) {
        pre.bind_seeds(seeds, false);
        pre.prepare_partition_scratch(offsets);
        const auto layouts = pre.scratch_layout();
        ASSERT_EQ(layouts.size(), offsets.size() - 1);
        for (size_t b = 0; b < layouts.size(); ++b) {
            const auto& layout = layouts[b];
            const std::array<uintptr_t, 3> addresses{layout.mask_address, layout.degree_address, layout.order_address};
            const std::array<size_t, 3> widths{sizeof(uint16_t), sizeof(uint8_t), sizeof(uint32_t)};
            for (size_t array = 0; array < addresses.size(); ++array) {
                ASSERT_GE(addresses[array], allocations[array].first);
                ASSERT_LE(addresses[array] + (offsets[b + 1] - offsets[b]) * widths[array],
                          allocations[array].second);
                EXPECT_EQ(addresses[array] % 64, 0U);
            }
            OneHopPredecoder::PartialResult partial;
            const auto actual = pre.prepare_partition_order(b, partial);
            std::vector<uint32_t> expected;
            for (size_t k = offsets[b]; k < offsets[b + 1]; ++k) expected.push_back(uint32_t(k));
            std::stable_sort(expected.begin(), expected.end(), [&](uint32_t a, uint32_t b) {
                return degree(a) < degree(b);
            });
            EXPECT_EQ(std::vector<uint32_t>(actual.sources.begin(), actual.sources.end()), expected);
            for (auto k : actual.sources)
                EXPECT_EQ(actual.masks_by_seed[k], (uint32_t{1} << degree(k)) - 1);
        }
        pre.prepare_scratch();
        EXPECT_EQ(scratch_addresses(pre), dense_addresses);
        EXPECT_EQ(pm::structured::SerialSeedStorageTestAccess::allocations(pre), allocations);
        pre.reset();
    }
}
