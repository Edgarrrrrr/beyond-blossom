// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include <cstdlib>
#include <optional>
#include <string>
#include "gtest/gtest.h"
#include "beyond_blossom/driver/backend_user_graph.h"
#include "beyond_blossom/structured/driver/shared_component_decoder.h"

namespace {
using namespace beyond_blossom::structured;

beyond_blossom::StaticGridSpec binding_spec() {
    return {3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}},
            {}, beyond_blossom::GridSimd::scalar};
}

beyond_blossom::BackendUserGraph binding_graph(size_t observables = 2, bool negative = false) {
    beyond_blossom::BackendUserGraph graph(4, observables);
    graph.add_or_merge_edge(0, 2, {0}, negative ? -2 : 2, -1);
    graph.add_or_merge_edge(1, 3, {observables - 1}, 3, -1);
    graph.add_or_merge_boundary_edge(0, {0}, 7, -1);
    graph.add_or_merge_boundary_edge(1, {observables - 1}, 11, -1);
    return graph;
}

TEST(StructuredPredecodeBinding, OppositePoliciesCoexistAndSurviveMoves) {
    auto graph = binding_graph();
    ComponentDecoder enabled(graph, binding_spec(), beyond_blossom::NUM_DISTINCT_WEIGHTS, true, true);
    ComponentDecoder disabled(graph, binding_spec(), beyond_blossom::NUM_DISTINCT_WEIGHTS, true, false);
    ASSERT_TRUE(enabled.initialize_serial_seed_storage());
    ASSERT_TRUE(disabled.initialize_serial_seed_storage());
    const std::vector<uint64_t> pairs{0, 1, 2, 3};
    EXPECT_EQ(enabled.decode(pairs), disabled.decode(pairs));
    for (size_t c = 0; c < 2; ++c) {
        EXPECT_TRUE(enabled.components()[c].mwpm.predecoder.complete);
        EXPECT_FALSE(disabled.components()[c].mwpm.predecoder.complete);
        EXPECT_FALSE(disabled.components()[c].mwpm.predecode_enabled());
    }
    ComponentDecoder moved(std::move(disabled));
    for (const auto& events : std::vector<std::vector<uint64_t>>{{}, {0}, {1, 3}, pairs, {2, 0}}) {
        EXPECT_EQ(enabled.decode(events), moved.decode(events));
        for (auto& c : moved.components()) {
            EXPECT_FALSE(c.mwpm.predecoder.complete);
            EXPECT_TRUE(c.mwpm.flooder.queue.empty());
            EXPECT_EQ(c.mwpm.active_tree_count(), 0U);
        }
    }
    std::vector<int64_t> on_edges, off_edges;
    enabled.decode_to_edges(pairs, on_edges);
    moved.decode_to_edges(pairs, off_edges);
    EXPECT_EQ(on_edges, off_edges);
    EXPECT_THROW(moved.decode({0, 0}), std::invalid_argument);
    EXPECT_EQ(enabled.decode(pairs), moved.decode(pairs));
}

TEST(StructuredPredecodeBinding, LargeObservableAndNegativeNormalizationKeepBothBindings) {
    auto graph = binding_graph(75, true);
    ComponentDecoder enabled(graph, binding_spec(), beyond_blossom::NUM_DISTINCT_WEIGHTS, true, true);
    ComponentDecoder disabled(graph, binding_spec(), beyond_blossom::NUM_DISTINCT_WEIGHTS, true, false);
    EXPECT_FALSE(enabled.initialize_serial_seed_storage());
    EXPECT_FALSE(disabled.initialize_serial_seed_storage());
    for (const auto& events : std::vector<std::vector<uint64_t>>{{}, {0}, {0, 1, 2, 3}, {3, 2}}) {
        std::vector<uint8_t> on(75), off(75);
        beyond_blossom::total_weight_int on_weight = 0, off_weight = 0;
        enabled.decode(events, on.data(), on_weight);
        disabled.decode(events, off.data(), off_weight);
        EXPECT_EQ(on, off);
        EXPECT_EQ(on_weight, off_weight);
    }
}

TEST(StructuredPredecodeBinding, SharedRejectsAnExplicitNoPredecodeCore) {
    auto graph = binding_graph();
    ComponentDecoder decoder(graph, binding_spec(), beyond_blossom::NUM_DISTINCT_WEIGHTS, false, false);
    EXPECT_THROW(decoder.prepare_for_mwpm({0, 2}), std::logic_error);
    EXPECT_THROW((SharedComponentDecoder(std::move(decoder), {-1}, 1, 64, 32, 3, false,
        SharedComponentDecoder::PrefixSchedule::component_sequential,
        SharedComponentDecoder::ContinuationPlacement::component_captain,
        TimePartitionCuts::Geometry::symmetric_width, SharedDecoderConfiguration{})), std::invalid_argument);
}

#if defined(__linux__)
TEST(StructuredPredecodeBinding, ExplicitSharedConfigurationDoesNotReadLegacyEnvironment) {
    const char* old = std::getenv("BB_IMPORT_PREPARATION");
    const std::optional<std::string> saved = old ? std::optional<std::string>(old) : std::nullopt;
    struct Restore {
        const std::optional<std::string>& saved;
        ~Restore() {
            if (saved) setenv("BB_IMPORT_PREPARATION", saved->c_str(), 1);
            else unsetenv("BB_IMPORT_PREPARATION");
        }
    } restore{saved};
    setenv("BB_IMPORT_PREPARATION", "invalid-environment-value", 1);
    auto graph = binding_graph();
    ComponentDecoder decoder(graph, binding_spec(), beyond_blossom::NUM_DISTINCT_WEIGHTS, false, true);
    SharedComponentDecoder explicit_decoder(std::move(decoder), {-1}, 1, 64, 32, 3, false,
        SharedComponentDecoder::PrefixSchedule::component_sequential,
        SharedComponentDecoder::ContinuationPlacement::component_captain,
        TimePartitionCuts::Geometry::symmetric_width, SharedDecoderConfiguration{});
    EXPECT_STREQ(explicit_decoder.import_preparation_name(), "serial");
    EXPECT_THROW(SharedDecoderConfiguration::from_environment(), std::invalid_argument);
}
#endif
}  // namespace
