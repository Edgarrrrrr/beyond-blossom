// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include "pymatching/sparse_blossom/driver/decoder_strategy.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#if defined(__linux__)
#include <sched.h>
#endif
#include "gtest/gtest.h"
#include "pymatching/sparse_blossom/driver/backend_user_graph.h"

namespace {
pm::StaticGridSpec strategy_test_spec() {
    return {3, 3, false, {{{0, 0, 0}}, {{0, 0, 0}}, {{2, 0, 0}}, {{2, 0, 0}}},
            {}, pm::GridSimd::scalar};
}
pm::BackendUserGraph strategy_test_graph(size_t observables = 2, bool negative = false) {
    pm::BackendUserGraph graph(4, observables);
    graph.add_or_merge_edge(0, 2, {0}, negative ? -2 : 2, -1);
    graph.add_or_merge_edge(1, 3, {observables - 1}, 3, -1);
    graph.add_or_merge_boundary_edge(0, {0}, 7, -1);
    graph.add_or_merge_boundary_edge(1, {observables - 1}, 11, -1);
    return graph;
}
std::vector<int> strategy_test_cpus() {
    std::vector<int> result;
#if defined(__linux__)
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0)
        for (int cpu = 0; cpu < CPU_SETSIZE && result.size() < 8; ++cpu)
            if (CPU_ISSET(cpu, &affinity)) result.push_back(cpu);
#elif defined(__APPLE__)
    for (int slot = 0; slot < 8; ++slot) result.push_back(slot);
#endif
    return result;
}

std::vector<std::pair<int64_t, int64_t>> canonical_strategy_edges(const std::vector<int64_t>& edges) {
    std::vector<std::pair<int64_t, int64_t>> result;
    for (size_t k = 0; k + 1 < edges.size(); k += 2)
        result.push_back(std::minmax(edges[k], edges[k + 1]));
    std::sort(result.begin(), result.end());
    return result;
}

void expect_strategy_correction(
    pm::BackendUserGraph& graph, const std::vector<uint64_t>& hits,
    const std::vector<int64_t>& edges, const pm::ExtendedMatchingResult& expected) {
    struct EdgeData {
        pm::total_weight_int weight;
        std::vector<size_t> observables;
    };
    std::map<std::pair<int64_t, int64_t>, EdgeData> original;
    graph.to_matching_or_search_graph_helper(pm::NUM_DISTINCT_WEIGHTS,
        [&](size_t u, size_t v, pm::signed_weight_int weight, const auto& observables, const auto&) {
            original[std::minmax(int64_t(u), int64_t(v))] = {weight, observables};
        },
        [&](size_t u, pm::signed_weight_int weight, const auto& observables, const auto&) {
            original[{int64_t{-1}, int64_t(u)}] = {weight, observables};
        });
    ASSERT_EQ(edges.size() % 2, 0U);
    std::vector<uint8_t> parity(graph.get_num_nodes()), wanted_parity(graph.get_num_nodes());
    for (const auto hit : hits) wanted_parity.at(hit) ^= 1;
    std::vector<uint8_t> observables(graph.get_num_observables());
    std::set<std::pair<int64_t, int64_t>> seen;
    pm::total_weight_int weight = 0;
    for (size_t k = 0; k < edges.size(); k += 2) {
        const auto u = edges[k], v = edges[k + 1];
        // The public boundary convention is independent of edge ordering.
        ASSERT_GE(u, 0);
        ASSERT_LT(size_t(u), parity.size());
        ASSERT_GE(v, -1);
        ASSERT_TRUE(v == -1 || size_t(v) < parity.size());
        const auto edge = std::minmax(u, v);
        ASSERT_TRUE(seen.insert(edge).second);
        const auto found = original.find(edge);
        ASSERT_NE(found, original.end());
        weight += found->second.weight;
        parity[u] ^= 1;
        if (v != -1) parity[v] ^= 1;
        for (auto observable : found->second.observables) observables.at(observable) ^= 1;
    }
    EXPECT_EQ(parity, wanted_parity);
    EXPECT_EQ(weight, expected.weight);
    EXPECT_EQ(observables, expected.obs_crossed);
}

TEST(DecoderStrategy, SerialMethodsRetainLargeObservableNegativeWeightAndEdgeContracts) {
    auto graph = strategy_test_graph(75, true);
    auto reference = graph.to_mwpm(pm::NUM_DISTINCT_WEIGHTS, true);
    const std::vector<std::vector<uint64_t>> shots{{}, {0}, {0, 1, 2, 3}, {3, 2}};
    std::vector<pm::ExtendedMatchingResult> expected;
    for (const auto& shot : shots) {
        pm::ExtendedMatchingResult value(75);
        pm::decode_detection_events(reference, shot, value.obs_crossed.data(), value.weight, false);
        expected.push_back(std::move(value));
    }
    for (auto strategy : {pm::DecoderStrategy::Upstream, pm::DecoderStrategy::GridSIMD, pm::DecoderStrategy::Predecode}) {
        pm::DecoderOptions options;
        options.strategy = strategy;
        // >64 observables include a search graph even with the default false
        // request. Metadata and edge capability must reflect the actual graph.
        if (strategy != pm::DecoderStrategy::Upstream) options.grid = strategy_test_spec();
        pm::StrategyDecoder decoder(graph, options);
        EXPECT_EQ(decoder.metadata().predecode_enabled, strategy == pm::DecoderStrategy::Predecode);
        EXPECT_TRUE(decoder.metadata().supports_extended_observables);
        EXPECT_TRUE(decoder.metadata().search_graph_included);
        EXPECT_THROW(decoder.decode(shots[0]), std::invalid_argument);
        decoder.run_with_team([&](pm::StrategyDecoder& active) {
            for (size_t k = 0; k < shots.size(); ++k) {
                pm::ExtendedMatchingResult value(75);
                active.decode(shots[k], value.obs_crossed.data(), value.weight);
                EXPECT_EQ(value, expected[k]);
                std::vector<int64_t> actual_edges, reference_edges;
                active.decode_to_edges(shots[k], actual_edges);
                pm::decode_detection_events_to_edges(reference, shots[k], reference_edges);
                // Physical edges are undirected; cross-backend emission order
                // follows different component and extraction traversals.
                EXPECT_EQ(canonical_strategy_edges(actual_edges), canonical_strategy_edges(reference_edges));
                expect_strategy_correction(graph, shots[k], actual_edges, expected[k]);
                expect_strategy_correction(graph, shots[k], reference_edges, expected[k]);
            }
        });
        EXPECT_EQ(decoder.metadata().actual_threads, 1U);
    }
}

TEST(DecoderStrategy, ExplicitConfigurationAndMoveValidation) {
    const stim::DetectorErrorModel dem("error(0.1) D0 L0");
    pm::StrategyDecoder generic(dem);
    const auto expected = generic.decode({0});
    auto moved = std::move(generic);
    EXPECT_EQ(moved.decode({0}), expected);
    EXPECT_THROW(generic.metadata(), std::logic_error);
    moved.run_with_team([&](pm::StrategyDecoder& active) {
        EXPECT_THROW(pm::StrategyDecoder(std::move(active)), std::logic_error);
        EXPECT_THROW(active.run_with_team([](auto&) {}), std::logic_error);
    });
    pm::DecoderOptions missing_grid;
    missing_grid.strategy = pm::DecoderStrategy::Predecode;
    EXPECT_THROW(pm::StrategyDecoder(dem, missing_grid), std::invalid_argument);
    auto graph = strategy_test_graph();
    missing_grid.grid = strategy_test_spec();
    missing_grid.enable_correlations = true;
    EXPECT_THROW(pm::StrategyDecoder(graph, missing_grid), std::invalid_argument);
    missing_grid.enable_correlations = false;
    missing_grid.strategy = pm::DecoderStrategy::Component;
    EXPECT_THROW(pm::StrategyDecoder(graph, missing_grid), std::invalid_argument);
}

TEST(DecoderStrategy, ArrayOutputOverwritesWeightAndXorsObservablesAtBothObservableWidths) {
    const std::vector<std::vector<uint64_t>> shots{{}, {0}, {0, 1, 2, 3}, {3, 2}};
    for (size_t observable_count : {size_t{2}, size_t{75}}) {
        auto graph = strategy_test_graph(observable_count, true);
        auto reference = graph.to_mwpm(pm::NUM_DISTINCT_WEIGHTS, true);
        std::vector<pm::ExtendedMatchingResult> expected;
        for (const auto& shot : shots) {
            pm::ExtendedMatchingResult value(observable_count);
            pm::decode_detection_events(reference, shot, value.obs_crossed.data(), value.weight, false);
            expected.push_back(std::move(value));
        }
        for (auto strategy : {pm::DecoderStrategy::Upstream, pm::DecoderStrategy::GridSIMD, pm::DecoderStrategy::Predecode}) {
            SCOPED_TRACE(::testing::Message() << pm::decoder_strategy_name(strategy) << " observables=" << observable_count);
            pm::DecoderOptions options;
            options.strategy = strategy;
            if (strategy != pm::DecoderStrategy::Upstream) options.grid = strategy_test_spec();
            pm::StrategyDecoder decoder(graph, options);
            for (size_t k = 0; k < shots.size(); ++k) {
                std::vector<uint8_t> observables(observable_count, 1);
                pm::total_weight_int weight = -1234567;
                decoder.decode(shots[k], observables.data(), weight);
                EXPECT_EQ(weight, expected[k].weight);
                for (size_t i = 0; i < observable_count; ++i)
                    EXPECT_EQ(observables[i], (expected[k].obs_crossed[i] ^ 1));
            }
        }
    }
}

TEST(DecoderStrategy, UpstreamCorrelationPolicyIsBoundAtConstruction) {
    const stim::DetectorErrorModel dem(
        "error(0.1) D0 L0 ^ D1 L1\nerror(0.05) D0 D1 L2\n");
    pm::DecoderOptions options;
    options.enable_correlations = true;
    pm::StrategyDecoder decoder(dem, options);
    auto reference = pm::detector_error_model_to_mwpm(dem, pm::NUM_DISTINCT_WEIGHTS, true, true);
    const std::vector<std::vector<uint64_t>> shots{{}, {0}, {1}, {0, 1}};
    const auto actual = decoder.decode_batch(shots);
    for (size_t k = 0; k < shots.size(); ++k)
        EXPECT_EQ(actual[k], pm::decode_detection_events_for_up_to_64_observables(reference, shots[k], true));
    for (size_t k = 0; k < shots.size(); ++k) {
        std::vector<uint8_t> observables(3, 1);
        pm::total_weight_int weight = 1234567;
        decoder.decode(shots[k], observables.data(), weight);
        EXPECT_EQ(weight, actual[k].weight);
        for (size_t i = 0; i < observables.size(); ++i)
            EXPECT_EQ(observables[i], ((actual[k].obs_mask >> i) & 1) ^ 1);
    }
    EXPECT_TRUE(decoder.metadata().correlations_enabled);
    EXPECT_TRUE(decoder.metadata().search_graph_included);
    EXPECT_FALSE(decoder.metadata().supports_match_edges);
    std::vector<int64_t> matches;
    EXPECT_THROW(decoder.decode_to_match_edges(shots[1], matches), std::invalid_argument);
}

TEST(DecoderStrategy, ParallelSessionEmptyOddExceptionalStreamsAndCapabilityBoundaries) {
#if !defined(_OPENMP)
    GTEST_SKIP() << "Parallel strategies require OpenMP";
#else
    const auto cpus = strategy_test_cpus();
    if (cpus.size() < 8) GTEST_SKIP() << "Eight allowed CPU IDs are needed for all parallel profiles";
    auto graph = strategy_test_graph();
    pm::StrategyDecoder generic(graph, {});
    const std::vector<std::vector<uint64_t>> shots{{0, 2}, {}, {1, 3}};
    const auto expected = generic.decode_batch(shots);
    for (auto strategy : {pm::DecoderStrategy::Component, pm::DecoderStrategy::TailReset, pm::DecoderStrategy::Spatial}) {
        pm::DecoderOptions options;
        options.strategy = strategy;
        options.grid = strategy_test_spec();
        const size_t threads = strategy == pm::DecoderStrategy::Component ? 2 : strategy == pm::DecoderStrategy::TailReset ? 4 : 8;
        options.cpus.assign(cpus.begin(), cpus.begin() + threads);
        pm::StrategyDecoder decoder(graph, options);
        EXPECT_THROW(decoder.decode(shots[0]), std::logic_error);
        EXPECT_TRUE(decoder.decode_batch({}).empty());
        EXPECT_EQ(decoder.decode_batch(shots), expected);
        EXPECT_EQ(decoder.metadata().actual_threads, threads);
        EXPECT_EQ(decoder.metadata().threads.size(), threads);
        std::set<uint64_t> thread_ids;
        for (const auto& thread : decoder.metadata().threads) {
            EXPECT_NE(thread.thread_id, 0U);
            EXPECT_TRUE(thread_ids.insert(thread.thread_id).second);
#if defined(__APPLE__)
            EXPECT_EQ(thread.requested_cpu, -1);
            EXPECT_TRUE(thread.affinity.empty());
            EXPECT_EQ(thread.qos_class, 25);  // QOS_CLASS_USER_INITIATED
            EXPECT_EQ(thread.qos_relative_priority, 0);
#else
            EXPECT_EQ(thread.affinity, std::vector<int>{thread.requested_cpu});
#endif
        }
        decoder.run_with_team([&](pm::StrategyDecoder& active) {
            for (size_t k = 0; k < shots.size(); ++k) {
                std::vector<uint8_t> observables(2, 1);
                pm::total_weight_int weight = -1234567;
                active.decode(shots[k], observables.data(), weight);
                EXPECT_EQ(weight, expected[k].weight);
                for (size_t i = 0; i < observables.size(); ++i)
                    EXPECT_EQ(observables[i], ((expected[k].obs_mask >> i) & 1) ^ 1);
            }
            active.drain();
        });
        EXPECT_THROW(decoder.run_with_team([&](pm::StrategyDecoder& active) {
            EXPECT_EQ(active.decode(shots[0]), expected[0]);
            throw std::runtime_error("caller exception after one pipeline shot");
        }), std::runtime_error);
        EXPECT_EQ(decoder.decode_batch(shots), expected);
        std::vector<int64_t> edges;
        EXPECT_THROW(decoder.decode_to_edges(shots[0], edges), std::invalid_argument);
        if (strategy != pm::DecoderStrategy::Component)
            EXPECT_THROW(decoder.decode_to_match_edges(shots[0], edges), std::invalid_argument);
        auto large_graph = strategy_test_graph(75);
        EXPECT_THROW(pm::StrategyDecoder(large_graph, options), std::invalid_argument);
    }
#endif
}

TEST(DecoderStrategy, SingleThreadControlPreservesParallelAlgorithmAndPublicLifecycle) {
#if !defined(_OPENMP)
    GTEST_SKIP() << "Parallel strategies require OpenMP";
#else
    const auto cpus = strategy_test_cpus();
    if (cpus.empty()) GTEST_SKIP() << "One allowed CPU/slot is needed";
    auto graph = strategy_test_graph();
    pm::StrategyDecoder generic(graph, {});
    const std::vector<std::vector<uint64_t>> shots{{0, 2}, {}, {1, 3}, {0, 1, 2, 3}, {0}};
    const auto expected = generic.decode_batch(shots);
    for (auto strategy : {pm::DecoderStrategy::Component, pm::DecoderStrategy::TailReset, pm::DecoderStrategy::Spatial}) {
        SCOPED_TRACE(pm::decoder_strategy_name(strategy));
        pm::DecoderOptions options;
        options.strategy = strategy;
        options.grid = strategy_test_spec();
        options.same_algorithm_single_thread = true;
        EXPECT_THROW(pm::StrategyDecoder(graph, options), std::invalid_argument);
        options.cpus = {cpus.front()};
        pm::StrategyDecoder decoder(graph, options);
        const auto& info = decoder.metadata();
        EXPECT_EQ(info.strategy, strategy);
        EXPECT_TRUE(info.same_algorithm_single_thread);
        EXPECT_EQ(info.requested_threads, 1U);
        EXPECT_EQ(info.execution, strategy == pm::DecoderStrategy::Component
            ? pm::DecoderExecution::component_team : pm::DecoderExecution::double_buffer);
        EXPECT_STREQ(info.pipeline_schedule, strategy == pm::DecoderStrategy::Component ? "none" : "single_thread");
        EXPECT_EQ(info.partitions, strategy == pm::DecoderStrategy::Spatial ? 2U : 1U);
#if defined(__APPLE__) && defined(__aarch64__)
        EXPECT_EQ(info.component_threshold, strategy == pm::DecoderStrategy::Spatial
            ? size_t(PM_M4_SPATIAL_COMPONENT_THRESHOLD) : size_t{64});
#else
        EXPECT_EQ(info.component_threshold, strategy == pm::DecoderStrategy::Spatial ? 0U : 64U);
#endif
        EXPECT_EQ(info.shot_threshold, 32U);
        EXPECT_STREQ(info.fixed_seed_input, strategy == pm::DecoderStrategy::Spatial ? "indexed" : "scan");
        EXPECT_STREQ(info.import_preparation, strategy == pm::DecoderStrategy::Spatial ? "captained_serial" : "serial");
        EXPECT_FALSE(info.supports_extended_observables);
        EXPECT_THROW(decoder.decode(shots.front()), std::logic_error);
        EXPECT_TRUE(decoder.decode_batch({}).empty());
        EXPECT_EQ(decoder.decode_batch(shots), expected);
        EXPECT_EQ(info.actual_threads, 1U);
        EXPECT_EQ(info.threads.size(), strategy == pm::DecoderStrategy::Component ? 1U : 2U);
        std::set<uint64_t> tids;
        for (const auto& thread : info.threads) {
            EXPECT_NE(thread.thread_id, 0U);
            tids.insert(thread.thread_id);
        }
        EXPECT_EQ(tids.size(), 1U);
        decoder.run_with_team([&](pm::StrategyDecoder& active) {
            for (size_t k = 0; k < shots.size(); ++k) {
                EXPECT_EQ(active.decode(shots[k]), expected[k]);
                // Drain at an odd boundary and continue the same session.
                if (k == 0) active.drain();
            }
            active.drain();
        });
        EXPECT_THROW(decoder.run_with_team([&](pm::StrategyDecoder& active) {
            EXPECT_EQ(active.decode(shots.front()), expected.front());
            throw std::runtime_error("single-thread control caller exception");
        }), std::runtime_error);
        EXPECT_EQ(decoder.decode_batch(shots), expected);
        auto large_graph = strategy_test_graph(75);
        EXPECT_THROW(pm::StrategyDecoder(large_graph, options), std::invalid_argument);
    }
    for (auto strategy : {pm::DecoderStrategy::Upstream, pm::DecoderStrategy::GridSIMD, pm::DecoderStrategy::Predecode}) {
        pm::DecoderOptions options;
        options.strategy = strategy;
        options.same_algorithm_single_thread = true;
        if (strategy != pm::DecoderStrategy::Upstream) options.grid = strategy_test_spec();
        EXPECT_THROW(pm::StrategyDecoder(graph, options), std::invalid_argument);
    }
#endif
}
}  // namespace
