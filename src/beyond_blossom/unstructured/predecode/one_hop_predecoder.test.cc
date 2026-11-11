// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/driver/mwpm_decoding.h"
#include "beyond_blossom/unstructured/predecode/one_hop_predecoder.h"

#include <gtest/gtest.h>
#include <limits>
#include <random>

using namespace beyond_blossom;

namespace {
struct TestEdge {
    size_t u;
    size_t v;
    signed_weight_int weight;
};

Mwpm make_decoder(size_t count, const std::vector<TestEdge>& edges, size_t observable_offset = 0) {
    MatchingGraph graph(count, observable_offset + edges.size());
    SearchGraph search(count);
    for (size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        const std::vector<size_t> obs{index + observable_offset};
        if (edge.v == SIZE_MAX) {
            graph.add_boundary_edge(edge.u, edge.weight, obs);
            search.add_boundary_edge(edge.u, edge.weight, obs);
        } else {
            graph.add_edge(edge.u, edge.v, edge.weight, obs);
            search.add_edge(edge.u, edge.v, edge.weight, obs);
        }
    }
    Mwpm result{GraphFlooder(std::move(graph)), SearchFlooder(std::move(search))};
    result.flooder.sync_negative_weight_observables_and_detection_events();
    return result;
}

std::pair<uint64_t, total_weight_int> evaluate(uint64_t mask, const std::vector<TestEdge>& edges) {
    uint64_t syndrome = 0;
    total_weight_int weight = 0;
    for (size_t index = 0; index < edges.size(); ++index) {
        if (!(mask & (uint64_t{1} << index)))
            continue;
        syndrome ^= uint64_t{1} << edges[index].u;
        if (edges[index].v != SIZE_MAX)
            syndrome ^= uint64_t{1} << edges[index].v;
        weight += edges[index].weight;
    }
    return {syndrome, weight};
}
}  // namespace

TEST(UnstructuredPredecode, ExhaustiveTJoinOracleWithNegativeWeightsAndBoundaries) {
    std::mt19937 rng(728191);
    size_t complete_shots = 0, partial_shots = 0;
    constexpr size_t count = 6;
    for (size_t trial = 0; trial < 40; ++trial) {
        std::vector<TestEdge> edges;
        bool connected[count][count]{};
        const auto next_weight = [&] { return signed_weight_int(2 * (int(rng() % 9) - 3)); };
        for (size_t u = 0; u + 1 < count; ++u) {
            edges.push_back({u, u + 1, next_weight()});
            connected[u][u + 1] = true;
        }
        for (size_t attempt = 0; attempt < 6; ++attempt) {
            size_t u = rng() % count, v = rng() % count;
            if (u > v)
                std::swap(u, v);
            if (u == v || connected[u][v])
                continue;
            connected[u][v] = true;
            edges.push_back({u, v, next_weight()});
        }
        if (trial % 2) {
            edges.push_back({0, SIZE_MAX, next_weight()});
            edges.push_back({count - 1, SIZE_MAX, next_weight()});
        }
        edges.push_back({trial % count, trial % count, -2});
        std::vector<total_weight_int> optimum(size_t{1} << count, std::numeric_limits<total_weight_int>::max());
        for (uint64_t mask = 0; mask < (uint64_t{1} << edges.size()); ++mask) {
            auto [syndrome, weight] = evaluate(mask, edges);
            optimum[syndrome] = std::min(optimum[syndrome], weight);
        }
        auto decoder = make_decoder(count, edges);
        auto reference = make_decoder(count, edges);
        reference.predecode_enabled = false;
        for (uint64_t syndrome = 0; syndrome < optimum.size(); ++syndrome) {
            std::vector<uint64_t> events;
            for (size_t node = 0; node < count; ++node)
                if (syndrome & (uint64_t{1} << node))
                    events.push_back(node);
            std::shuffle(events.begin(), events.end(), rng);
            if (optimum[syndrome] == std::numeric_limits<total_weight_int>::max()) {
                EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, events, false), std::invalid_argument);
                continue;
            }
            const auto result = decode_detection_events_for_up_to_64_observables(decoder, events, false);
            const auto baseline = decode_detection_events_for_up_to_64_observables(reference, events, false);
            ASSERT_EQ(result.weight, optimum[syndrome]) << "trial=" << trial << " syndrome=" << syndrome;
            ASSERT_EQ(result.weight, baseline.weight);
            const auto [actual_syndrome, actual_weight] = evaluate(result.obs_mask, edges);
            ASSERT_EQ(actual_syndrome, syndrome);
            ASSERT_EQ(actual_weight, optimum[syndrome]);
            complete_shots += decoder.predecoder.complete;
            partial_shots += !decoder.predecoder.complete;
            ASSERT_TRUE(decoder.flooder.queue.empty());
            ASSERT_EQ(decoder.node_arena.allocated.size(), decoder.node_arena.available.size());
            ASSERT_EQ(decoder.flooder.region_arena.allocated.size(), decoder.flooder.region_arena.available.size());
        }
    }
    EXPECT_GT(complete_shots, 0);
    EXPECT_GT(partial_shots, 0);
}

TEST(UnstructuredPredecode, ProvisionalPairIsRevisedByFullMatching) {
    auto decoder = make_decoder(4, {{1, 2, 2}, {0, 1, 4}, {2, 3, 4}, {0, SIZE_MAX, 100}, {3, SIZE_MAX, 100}});
    const auto result = decode_detection_events_for_up_to_64_observables(decoder, {1, 2, 0, 3}, false);
    EXPECT_FALSE(decoder.predecoder.complete);
    EXPECT_EQ(decoder.predecoder.weight, 2);
    EXPECT_EQ(result.weight, 8);
    EXPECT_EQ(result.obs_mask, 6);
}

TEST(UnstructuredPredecode, CompleteShotAvoidsArenasAndSupportsReverseSeedOrder) {
    auto decoder = make_decoder(4, {{0, 1, 2}, {2, 3, 6}, {0, 2, 100}});
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {3, 2, 1, 0}, false), MatchingResult(3, 8));
    EXPECT_TRUE(decoder.predecoder.complete);
    EXPECT_TRUE(decoder.flooder.region_arena.allocated.empty());
    EXPECT_TRUE(decoder.node_arena.allocated.empty());
    decode_detection_events_to_match_edges(decoder, {3, 2, 1, 0});
    ASSERT_EQ(decoder.flooder.match_edges.size(), 2);
    EXPECT_EQ(decoder.flooder.match_edges[0].obs_mask ^ decoder.flooder.match_edges[1].obs_mask, 3);
}

TEST(UnstructuredPredecode, WideObservablesNegativeEdgesAndEdgeExtraction) {
    auto decoder = make_decoder(3, {{0, 1, -2}, {1, 2, 4}, {2, SIZE_MAX, 6}}, 65);
    std::vector<uint8_t> observables(68);
    total_weight_int weight = 0;
    decode_detection_events(decoder, {0, 2}, observables.data(), weight, false);
    EXPECT_EQ(weight, 2);
    EXPECT_EQ(observables[65], 1);
    EXPECT_EQ(observables[66], 1);
    EXPECT_EQ(observables[67], 0);
    std::vector<int64_t> edges;
    decode_detection_events_to_edges(decoder, {0, 2}, edges);
    EXPECT_EQ(edges.size(), 4);
    uint64_t syndrome = 0;
    for (auto endpoint : edges)
        if (endpoint >= 0)
            syndrome ^= uint64_t{1} << endpoint;
    EXPECT_EQ(syndrome, 5);
}

TEST(UnstructuredPredecode, BoundaryNodesErrorsMovesAndRepeatedShots) {
    auto decoder = make_decoder(3, {{0, 1, 2}});
    decoder.flooder.graph.is_user_graph_boundary_node = {false, false, true};
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0, 9}, false), std::invalid_argument);
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0, 0}, false), std::invalid_argument);
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0}, false), std::invalid_argument);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {0, 1, 2}, false), MatchingResult(1, 2));
    auto moved = std::move(decoder);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(moved, {1, 0}, false), MatchingResult(1, 2));
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(moved, {}, false), MatchingResult());
}

TEST(UnstructuredPredecode, ArbitraryHighDegreeGraph) {
    const auto create = [] {
        MatchingGraph graph(81, 1);
        for (size_t node = 1; node <= 80; ++node) {
            graph.add_edge(0, node, 2, {});
            graph.add_boundary_edge(node, 2, {});
        }
        return Mwpm{GraphFlooder(std::move(graph))};
    };
    auto decoder = create();
    auto reference = create();
    reference.predecode_enabled = false;
    std::vector<uint64_t> events;
    for (size_t node = 0; node <= 80; ++node)
        events.push_back(node);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, events, false).weight, 160);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(reference, events, false).weight, 160);
    // The central seed saturates the leaf constraints; unresolved leaves must
    // continue through the full solver despite the graph's high degree.
    EXPECT_FALSE(decoder.predecoder.complete);
}

TEST(UnstructuredPredecode, CorrelationPassesBypassAndRestorePredecoder) {
    auto decoder = make_decoder(2, {{0, 1, 4}, {0, SIZE_MAX, 8}, {1, SIZE_MAX, 8}});
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, true), MatchingResult(1, 4));
    EXPECT_TRUE(decoder.predecode_enabled);
    EXPECT_TRUE(decoder.predecoder.seeds.empty());
    EXPECT_FALSE(decoder.flooder.region_arena.allocated.empty());
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, false), MatchingResult(1, 4));
    EXPECT_TRUE(decoder.predecoder.complete);
}

TEST(UnstructuredPredecode, OddLowLevelWeightsKeepDualFeasibleCaps) {
    MatchingGraph graph(3, 0);
    graph.add_edge(0, 1, 3, {});
    graph.add_edge(1, 2, 5, {});
    OneHopPredecoder predecoder;
    predecoder.prepare(graph, {0, 1, 2}, {});
    predecoder.run();
    EXPECT_FALSE(predecoder.complete);
    EXPECT_LE(predecoder.seeds[0].radius + predecoder.seeds[1].radius, 3);
    EXPECT_LE(predecoder.seeds[1].radius + predecoder.seeds[2].radius, 5);
    for (const auto& seed : predecoder.seeds)
        EXPECT_EQ(seed.radius & 1, 0);
}

TEST(UnstructuredPredecode, RebuildsCapsAfterPublicGraphWeightChanges) {
    auto decoder = make_decoder(2, {{0, 1, 4}});
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, false).weight, 4);
    decoder.flooder.graph.nodes[0].neighbor_weights[0] = 10;
    decoder.flooder.graph.nodes[1].neighbor_weights[0] = 10;
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, false).weight, 10);
    EXPECT_TRUE(decoder.predecoder.complete);
}

TEST(UnstructuredPredecode, CorrelationInputFailuresLeaveDecoderReusable) {
    auto decoder = make_decoder(2, {{0, 1, 4}, {0, SIZE_MAX, 8}, {1, SIZE_MAX, 8}});
    for (const std::vector<uint64_t> events : {std::vector<uint64_t>{0, 99}, std::vector<uint64_t>{0, 0}}) {
        EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, events, true), std::invalid_argument);
        EXPECT_TRUE(decoder.flooder.queue.empty());
        EXPECT_TRUE(decoder.predecode_enabled);
        EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, true), MatchingResult(1, 4));
    }
    auto negative = make_decoder(2, {{0, 1, -4}});
    negative.predecode_enabled = false;
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(negative, {0, 99}, false), std::invalid_argument);
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(negative, {0, 0}, false), std::invalid_argument);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(negative, {0, 1}, false), MatchingResult(1, -4));
}

TEST(UnstructuredPredecode, SparseHighDegreeFallbackPreservesDenseComponentsAndGraphChanges) {
    const auto create = [] {
        // Thousands of irrelevant vertices must not turn the dense shot into
        // a sparse one. Dispatch observes the active neighborhood instead.
        MatchingGraph graph(4096, 0);
        for (size_t u = 0; u < 128; ++u)
            for (size_t v = u + 1; v < 128; ++v)
                graph.add_edge(u, v, 2, {});
        return Mwpm{GraphFlooder(std::move(graph))};
    };
    auto decoder = create();
    auto reference = create();
    reference.predecode_enabled = false;
    const std::vector<uint64_t> sparse{0, 1, 2, 3, 4, 5, 6, 7};
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, sparse, false).weight, 8);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(reference, sparse, false).weight, 8);
    ASSERT_TRUE(decoder.predecoder.skipped);
    for (const auto& seed : decoder.predecoder.seeds) {
        EXPECT_EQ(seed.radius, 0);
        EXPECT_EQ(seed.mate, OneHopPredecoder::UNMATCHED);
    }
    EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0, 1, 2}, false), std::invalid_argument);
    EXPECT_TRUE(decoder.flooder.queue.empty());
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, sparse, false).weight, 8);
    std::vector<uint64_t> dense;
    for (size_t node = 0; node < 96; ++node)
        dense.push_back(node);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, dense, false).weight, 96);
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(reference, dense, false).weight, 96);
    EXPECT_FALSE(decoder.predecoder.skipped);
    EXPECT_GT(decoder.predecoder.weight, 0);
    // Public adjacency/weight changes are observed on the next shot, without
    // a stale topology or weight cache suppressing useful boundary matches.
    for (auto node : sparse)
        decoder.flooder.graph.add_boundary_edge(node, 0, {});
    EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, sparse, false).weight, 0);
    EXPECT_FALSE(decoder.predecoder.skipped);
    EXPECT_TRUE(decoder.predecoder.complete);
}

TEST(UnstructuredPredecode, SparseFallbackNormalizesNegativeWeightsAndRecoversFromErrors) {
    const auto create = [] {
        MatchingGraph graph(128, 70);
        SearchGraph search(128);
        for (size_t u = 0; u < 128; ++u)
            for (size_t v = u + 1; v < 128; ++v) {
                const signed_weight_int weight = u == 0 && v == 127 ? -2 : 2;
                const std::vector<size_t> observables{weight < 0 ? size_t{65} : size_t{66}};
                graph.add_edge(u, v, weight, observables);
                search.add_edge(u, v, weight, observables);
            }
        Mwpm result{GraphFlooder(std::move(graph)), SearchFlooder(std::move(search))};
        result.flooder.sync_negative_weight_observables_and_detection_events();
        return result;
    };
    auto decoder = create();
    auto reference = create();
    reference.predecode_enabled = false;
    const std::vector<uint64_t> events{0, 1, 2, 3, 4, 5};
    for (bool correlated : {false, true}) {
        EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0, 999}, correlated), std::invalid_argument);
        EXPECT_THROW(decode_detection_events_for_up_to_64_observables(decoder, {0, 0}, correlated), std::invalid_argument);
        std::vector<uint8_t> observed(70), expected(70);
        total_weight_int weight = 0, reference_weight = 0;
        decode_detection_events(decoder, events, observed.data(), weight, correlated);
        decode_detection_events(reference, events, expected.data(), reference_weight, correlated);
        EXPECT_EQ(weight, 4);
        EXPECT_EQ(weight, reference_weight);
        EXPECT_EQ(observed, expected);
        EXPECT_EQ(observed[65], 1);
        EXPECT_TRUE(decoder.predecode_enabled);
        if (!correlated)
            EXPECT_TRUE(decoder.predecoder.skipped);
    }
}
