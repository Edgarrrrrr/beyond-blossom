// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <numeric>

#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"

#if PM_PREDECODE_ENABLED
namespace {
using namespace pm::structured;
struct Edge { size_t u; int v; int weight; };
const std::vector<Edge> blossom_edges{
    {0,1,10}, {1,4,20}, {4,3,20}, {3,2,12}, {0,2,16}, {4,5,50}, {2,6,100}, {5,-1,36}};
const std::vector<std::array<int64_t,3>> blossom_coordinates{
    {0,0,0}, {2,0,0}, {0,0,1}, {0,2,1}, {2,2,0}, {4,2,0}, {0,0,2}};

Mwpm make_decoder(const std::vector<std::array<int64_t,3>>& coordinates, const std::vector<Edge>& edges) {
    MatchingGraph graph(coordinates.size(), edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto& e = edges[i];
        if (e.v < 0) graph.add_boundary_edge(e.u, e.weight, {i});
        else graph.add_edge(e.u, size_t(e.v), e.weight, {i});
    }
    graph.configure_grid({5, 5, false, coordinates, {}, pm::GridSimd::scalar});
    GraphFlooder flooder(std::move(graph));
    flooder.sync_negative_weight_observables_and_detection_events();
    return Mwpm(std::move(flooder));
}

pm::MatchingResult generic_result(size_t n, const std::vector<Edge>& edges, const std::vector<uint64_t>& events) {
    pm::MatchingGraph graph(n, edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto& e = edges[i];
        if (e.v < 0) graph.add_boundary_edge(e.u, e.weight, {i});
        else graph.add_edge(e.u, size_t(e.v), e.weight, {i});
    }
    pm::Mwpm oracle(pm::GraphFlooder(std::move(graph)));
    return pm::decode_detection_events_for_up_to_64_observables(oracle, events, false);
}

void expect_match_edges(const Mwpm& decoder, std::vector<std::array<uint64_t,3>> expected) {
    std::vector<std::array<uint64_t,3>> actual;
    for (const auto& edge : decoder.flooder.match_edges) {
        ASSERT_NE(edge.loc_from, nullptr);
        auto from = uint64_t(edge.loc_from->detector_id);
        auto to = edge.loc_to ? uint64_t(edge.loc_to->detector_id) : UINT64_MAX;
        if (to < from) std::swap(from, to);
        actual.push_back({from, to, edge.obs_mask});
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

void import_captained(Mwpm& decoder) {
    const auto& seeds = decoder.predecoder.seed_view;
    const auto count = seeds.active_count();
    PredecodeImportScratch scratch;
    scratch.roots.reserve(count);
    const auto first = decoder.prepare_predecode_import(count);
    decoder.import_fixed_predecode_range_captained<2>(seeds, first, 0, count, scratch);
    decoder.prepare_predecode_import_events(scratch);
    decoder.publish_predecode_import_events(std::span<const PredecodeImportScratch>(&scratch, 1), count);
}

void expect_zero_seed_node(const DetectorNode& node) {
    EXPECT_EQ(node.region_that_arrived, NO_REGION);
    EXPECT_EQ(node.reached_from_source, nullptr);
    EXPECT_EQ(node.radius_of_arrival, 0);
    EXPECT_EQ(node.observables_crossed_from_source, 0U);
    EXPECT_FALSE(node.node_event_tracker.has_queued_time);
    EXPECT_FALSE(node.node_event_tracker.has_desired_time);
}

void prepare_cold(Mwpm& decoder, const std::vector<uint64_t>& events) {
    decoder.predecoder.prepare(decoder.flooder.graph, events, {});
    for (auto id : events) decoder.create_detection_event(&decoder.flooder.graph.node(id));
    decoder.verify_invariants();
}

void expect_clean(const Mwpm& decoder) {
    EXPECT_EQ(decoder.active_tree_count(), 0);
    EXPECT_TRUE(decoder.flooder.queue.empty());
    EXPECT_EQ(decoder.node_arena.allocated.size(), decoder.node_arena.available.size());
    decoder.verify_invariants();
    for (size_t i = 0; i < decoder.flooder.graph.num_nodes; ++i) {
        const auto& node = decoder.flooder.graph.node(i);
        EXPECT_EQ(node.region_that_arrived, NO_REGION);
        EXPECT_FALSE(node.node_event_tracker.has_queued_time);
        EXPECT_FALSE(node.node_event_tracker.has_desired_time);
    }
}
}  // namespace

TEST(StructuredActiveTrees, BlossomAbsorptionAndShatteringPreserveRootsUntilAugmentation) {
    auto decoder = make_decoder(blossom_coordinates, blossom_edges);
    const std::vector<uint64_t> events{0,1,2,3,4,5,6};
    prepare_cold(decoder, events);
    // Two pairs, two absorptions, a five-cycle contraction, augmentation,
    // absorption of the matched blossom, shattering, then boundary augmentation.
    const std::array<size_t,9> counts{5,3,3,3,3,1,1,1,0};
    const std::array<size_t,9> times{5,6,14,15,17,25,83,91,94};
    bool saw_extra_plus_nodes = false;
    for (size_t i = 0; i < counts.size(); ++i) {
        const auto event = decoder.flooder.run_until_next_mwpm_notification();
        ASSERT_NE(event.event_type, NO_EVENT);
        EXPECT_EQ(decoder.flooder.queue.cur_time, times[i]);
        if (i == 7) EXPECT_EQ(event.event_type, BLOSSOM_SHATTER);
        decoder.process_event(event);
        EXPECT_EQ(decoder.active_tree_count(), counts[i]);
        decoder.verify_invariants();
        saw_extra_plus_nodes |= decoder.node_arena.allocated.size() - decoder.node_arena.available.size() > counts[i];
    }
    EXPECT_TRUE(saw_extra_plus_nodes);
    EXPECT_FALSE(decoder.flooder.queue.empty());
    const auto result = decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true);
    EXPECT_EQ(result, generic_result(7, blossom_edges, events));
    EXPECT_EQ(result.weight, 166);
    EXPECT_EQ(decoder.flooder.queue.cur_time, 94);
    expect_clean(decoder);
}

TEST(StructuredActiveTrees, ImportedBoundaryMatchConsumesOneRoot) {
    const std::vector<Edge> edges{{0,1,4}, {0,-1,2}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}}, edges);
    decoder.predecoder.prepare(decoder.flooder.graph, {0,1}, {});
    auto seeds = decoder.predecoder.seed_view.storage;
    seeds[0].mate = PredecodeSeed::BOUNDARY;
    seeds[0].radius = 2;
    seeds[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(nullptr);
    decoder.import_predecode_state(seeds);
    ASSERT_EQ(decoder.active_tree_count(), 1);
    ASSERT_TRUE(decoder.flooder.regions->is_unchanged_seed(1));
    ASSERT_TRUE(decoder.flooder.regions->is_lazy_seed(1));
    auto event = decoder.flooder.run_until_next_mwpm_notification();
    ASSERT_EQ(event.event_type, REGION_HIT_REGION);
    decoder.process_event(event);
    EXPECT_FALSE(decoder.flooder.regions->is_unchanged_seed(1));
    EXPECT_FALSE(decoder.flooder.regions->is_lazy_seed(1));
    EXPECT_EQ(decoder.active_tree_count(), 0);
    decoder.verify_invariants();
    EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
        generic_result(2, edges, {0,1}));
    expect_clean(decoder);
}

TEST(StructuredActiveTrees, VacatedShellNodeTrackersRetireBeforeTheNextShot) {
    auto coordinates = blossom_coordinates;
    coordinates.push_back({2,0,1});
    coordinates.push_back({4,0,1});
    const std::vector<uint64_t> events{0,1,2,3,4,5,6};
    // The blossom reaches node 7 and schedules a distant event toward node 8.
    // Its later shrinking vacates node 7, leaving a cancelled queued tracker
    // outside every surviving shell when the last root reaches the boundary.
    auto edges = blossom_edges;
    edges.push_back({2,7,12});
    edges.push_back({7,8,100});
    auto decoder = make_decoder(coordinates, edges);
    for (size_t repeat = 0; repeat < 3; ++repeat) {
        prepare_cold(decoder, events);
        for (size_t step = 0; decoder.active_tree_count() != 0 && step < 100; ++step) {
            auto event = decoder.flooder.run_until_next_mwpm_notification();
            ASSERT_NE(event.event_type, NO_EVENT);
            decoder.process_event(event);
            decoder.verify_invariants();
        }
        ASSERT_EQ(decoder.active_tree_count(), 0);
        const auto& vacated = decoder.flooder.graph.node(7);
        ASSERT_EQ(vacated.region_that_arrived, NO_REGION);
        ASSERT_TRUE(vacated.node_event_tracker.has_queued_time);
        EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
            generic_result(coordinates.size(), edges, events));
        expect_clean(decoder);
        EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, {7}, false),
            generic_result(coordinates.size(), edges, {7}));
        expect_clean(decoder);
    }
}

TEST(StructuredActiveTrees, JoinedImportCountsOnlyUnmatchedSeedsAndRollsBackReservations) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,8}, {2,-1,6}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
    for (bool fixed : {false, true}) for (bool matched_boundary : {false, true}) {
        decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
        auto seeds = decoder.predecoder.seed_view.storage;
        seeds[0].mate = 1; seeds[1].mate = 0;
        seeds[0].radius = seeds[1].radius = 2;
        seeds[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(&decoder.flooder.graph.node(1));
        seeds[1].match_slot = decoder.flooder.graph.node(1).index_of_neighbor(&decoder.flooder.graph.node(0));
        if (matched_boundary) {
            seeds[2].mate = PredecodeSeed::BOUNDARY; seeds[2].radius = 6;
            seeds[2].match_slot = decoder.flooder.graph.node(2).index_of_neighbor(nullptr);
        }
        std::array<PredecodeSeed,9> backing{};
        SeedView view;
        const std::array<size_t,4> bases{0,3,6,9};
        view.bind_fixed(backing, bases);
        view.set_counts(std::array<size_t,3>{1,1,1});
        backing[0] = seeds[0]; backing[3] = seeds[1]; backing[6] = seeds[2];
        backing[0].mate = 3; backing[3].mate = 0;
        const auto first = decoder.prepare_predecode_import(3);
        if (fixed) {
            decoder.import_fixed_predecode_range<2>(view, first, 0, 1);
            decoder.import_fixed_predecode_range<2>(view, first, 1, 3);
        } else {
            decoder.import_predecode_range(seeds, first, 0, 1);
            decoder.import_predecode_range(seeds, first, 1, 3);
        }
        EXPECT_EQ(decoder.active_tree_count(), 0);  // Workers construct regions, never roots.
        if (fixed) decoder.finish_fixed_predecode_import(view, first);
        else decoder.finish_predecode_import(seeds, first);
        EXPECT_EQ(decoder.active_tree_count(), matched_boundary ? 0 : 1);
        decoder.verify_invariants();
        EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
            generic_result(3, edges, {0,1,2}));
        expect_clean(decoder);
    }
    decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
    const auto seeds = decoder.predecoder.seed_view.storage;
    const auto first = decoder.prepare_predecode_import(seeds);
    decoder.import_predecode_range(seeds, first, 0, 1);  // Simulate another worker throwing.
    decoder.reset();
    expect_clean(decoder);
    EXPECT_TRUE(decoder.flooder.regions->empty());
}

namespace {
using FinishGrid = StaticGridComponent<DetectorNode, pm::obs_int>;
decltype(FinishGrid::matching_next) finish_original_next = nullptr;
std::vector<uint64_t> finish_event_order;
size_t finish_fail_at = SIZE_MAX;

pm::GridNextEvent observe_import_finish(
        const FinishGrid& grid, const DetectorNode& source, const pm::VaryingCT* radii) {
    // Events must never inspect a partially imported neighbor, even though
    // later roots have not been constructed yet in the fused finish.
    for (const auto& node : grid.nodes) EXPECT_NE(node.region_that_arrived, NO_REGION);
    finish_event_order.push_back(source.detector_id);
    if (finish_event_order.size() == finish_fail_at)
        throw std::runtime_error("failure after a prior root event was queued");
    return finish_original_next(grid, source, radii);
}
}

TEST(StructuredActiveTrees, FusedImportFinishKeepsEventOrderAndRecoversPartialForest) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,8}, {2,-1,6}};
    const std::vector<uint64_t> events{0,1,2};
    for (bool fixed : {false, true}) for (bool legacy_seed : {false, true}) {
        auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
        auto& grid = *decoder.flooder.graph.node(0).grid_component;
        finish_original_next = grid.matching_next;
        for (bool fail : {true, false}) {
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            const auto source_seeds = decoder.predecoder.seed_view.storage;
            std::vector<PredecodeSeed> seeds(source_seeds.begin(), source_seeds.end());
            if (legacy_seed) for (auto& seed : seeds) seed.grid = nullptr;
            std::array<PredecodeSeed,9> backing{};
            SeedView view;
            view.bind_fixed(backing, std::array<size_t,4>{0,3,6,9});
            view.set_counts(std::array<size_t,3>{1,1,1});
            backing[0] = seeds[0]; backing[3] = seeds[1]; backing[6] = seeds[2];
            const auto first = decoder.prepare_predecode_import(3);
            if (fixed) decoder.import_fixed_predecode_range<2>(view, first, 0, 3);
            else decoder.import_predecode_range(seeds, first, 0, 3);
            grid.matching_next = observe_import_finish;
            finish_event_order.clear();
            finish_fail_at = fail ? 2 : SIZE_MAX;
            const auto finish = [&] {
                if (fixed) decoder.finish_fixed_predecode_import(view, first);
                else decoder.finish_predecode_import(seeds, first);
            };
            if (fail) {
                EXPECT_THROW(finish(), std::runtime_error);
                EXPECT_EQ(finish_event_order, (std::vector<uint64_t>{0,1}));
                EXPECT_EQ(decoder.active_tree_count(), 2U);
                EXPECT_FALSE(decoder.flooder.queue.empty());
                grid.matching_next = finish_original_next;
                decoder.reset();
            } else {
                finish();
                EXPECT_EQ(finish_event_order, events);
                EXPECT_EQ(decoder.active_tree_count(), 3U);
                grid.matching_next = finish_original_next;
                decoder.verify_invariants();
                EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                          generic_result(3, edges, events));
            }
            expect_clean(decoder);
        }
    }
}

TEST(StructuredActiveTrees, UnchangedSeedsExtractWithoutRegionTeardownAcrossHolesAndReuse) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,3,100}, {2,-1,6}, {3,-1,8}};
    const std::vector<uint64_t> events{0,1,2,3};
    for (int import_route : {0,1,2}) for (int output : {0,1,2}) {
        const bool fixed = import_route != 0;
        std::array<PredecodeSeed,12> backing{};
        auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, edges);
        auto& pool = *decoder.flooder.regions;
        for (int repeat = 0; repeat < 4; ++repeat) {
            // Two consecutive deferred shots, then an ordinary import, then a
            // deferred import again exercise both persistence and flag clearing.
            const bool captained = import_route == 2 && repeat != 2;
            const bool node_deferred = captained || !fixed;
            SCOPED_TRACE(::testing::Message() << "route=" << import_route << " output=" << output
                << " repeat=" << repeat << " captained=" << captained);
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            const auto dense_view = decoder.predecoder.seed_view;
            auto dense = dense_view.storage;
            dense[0].mate = 1; dense[1].mate = 0;
            dense[0].radius = dense[1].radius = 2;
            dense[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(&decoder.flooder.graph.node(1));
            dense[1].match_slot = dense[0].match_slot ^ 1;
            dense[2].mate = PredecodeSeed::BOUNDARY; dense[2].radius = 6;
            dense[2].match_slot = decoder.flooder.graph.node(2).index_of_neighbor(nullptr);
            dense[3].radius = 2;  // Unmatched predecode growth still owns only this seed node.
            if (fixed) {
                backing[0] = dense[0]; backing[4] = dense[1];
                backing[8] = dense[2]; backing[9] = dense[3];
                backing[0].mate = 4; backing[4].mate = 0;
                decoder.predecoder.seed_view.bind_fixed(backing, std::array<size_t,4>{0,4,8,12});
                decoder.predecoder.seed_view.set_counts(std::array<size_t,3>{1,1,2});
                if (captained) {
                    import_captained(decoder);
                } else {
                    const auto first = decoder.prepare_predecode_import(4);
                    decoder.import_fixed_predecode_range<2>(decoder.predecoder.seed_view, first, 0, 4);
                    decoder.finish_fixed_predecode_import(decoder.predecoder.seed_view, first);
                }
            } else {
                decoder.import_predecode_state(dense);
            }
            for (RegionId id : {1U,2U,3U}) {
                ASSERT_TRUE(pool.is_unchanged_seed(id));
                ASSERT_TRUE(pool.is_lazy_seed(id));
                EXPECT_TRUE(pool.is_live(id, pool.generation(id)));
                EXPECT_EQ(pool.is_seed_node_deferred(id), node_deferred);
                const auto& node = decoder.flooder.graph.node(id - 1);
                if (node_deferred) expect_zero_seed_node(node);
                else {
                    EXPECT_EQ(node.region_that_arrived, id);
                    EXPECT_EQ(node.reached_from_source, &node);
                }
                EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].top_region_id, id);
                EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].wrapped_radius_cached, 0);
            }
            ASSERT_FALSE(pool.is_unchanged_seed(4));
            ASSERT_FALSE(pool.is_lazy_seed(4));
            for (size_t step = 0; decoder.active_tree_count() != 0 && step < 100; ++step) {
                auto event = decoder.flooder.run_until_next_mwpm_notification();
                ASSERT_NE(event.event_type, NO_EVENT);
                decoder.process_event(event);
            }
            ASSERT_EQ(decoder.active_tree_count(), 0U);
            for (RegionId id : {1U,2U,3U}) {
                ASSERT_TRUE(pool.is_unchanged_seed(id));
                ASSERT_TRUE(pool.is_lazy_seed(id));
                if (node_deferred) expect_zero_seed_node(decoder.flooder.graph.node(id - 1));
            }
            if (output == 2) {
                decode_prepared_detection_events_to_normalized_match_edges(decoder, false, true);
                expect_match_edges(decoder, {{0,1,1}, {2,UINT64_MAX,8}, {3,UINT64_MAX,16}});
                pm::obs_int obs = 0;
                for (const auto& edge : decoder.flooder.match_edges) obs ^= edge.obs_mask;
                EXPECT_EQ(obs, generic_result(4, edges, events).obs_mask);
                decoder.flooder.match_edges.clear();
            } else {
                const auto result = output == 1 ? solve_prepared_detection_events_deferring_seed_nodes(decoder)
                    : decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true);
                EXPECT_EQ(result, generic_result(4, edges, events));
                if (output == 1) {
                    // Only the eager root still needs detector-node cleanup.
                    const auto& root_node = decoder.flooder.graph.node(3);
                    EXPECT_EQ(root_node.reached_from_source, &root_node);
                    for (RegionId id : {1U,2U,3U}) {
                        EXPECT_EQ(pool.is_seed_node_deferred(id), node_deferred);
                        if (node_deferred) expect_zero_seed_node(decoder.flooder.graph.node(id - 1));
                    }
                    decoder.clear_pending_seed_nodes();
                    expect_zero_seed_node(root_node);
                }
            }
            for (RegionId id : {1U,2U,3U}) {
                // No full region was ever constructed, including on repeat shots.
                EXPECT_TRUE(pool.is_deferred_seed_slot(id));
                EXPECT_FALSE(pool.is_live(id, pool.generation(id)));
                EXPECT_EQ(pool.is_seed_node_deferred(id), node_deferred);
                const auto& node = decoder.flooder.graph.node(id - 1);
                expect_zero_seed_node(node);
                EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].top_region_id, NO_REGION);
                EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].wrapped_radius_cached, 0);
            }
            EXPECT_TRUE(pool.empty());
            // Duplicate extraction produces no paths or weight.
            EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                      (pm::MatchingResult{}));
            expect_clean(decoder);
            const auto end = pool.take_neutral_reset_end();
            if (end) pool.prepare_neutral_range(1, end);
            for (RegionId id : {1U,2U,3U}) {
                EXPECT_TRUE(pool.is_deferred_seed_slot(id));
                EXPECT_EQ(pool.is_seed_node_deferred(id), node_deferred);
            }
            decoder.predecoder.seed_view = dense_view;
        }
    }
}

TEST(StructuredActiveTrees, LazyMatchedSeedsMaterializeOnlyOnBoundaryOrPairCollision) {
    // A root either replaces a frozen boundary match directly, or first
    // absorbs a frozen pair and then augments through its other endpoint.
    // Neither fixture is inspected through pool[]/top_region/invariants before
    // the collision: those accessors would hide an eager import regression.
    for (bool paired : {false, true}) for (int import_route : {0,1,2})
    for (int output : {0,1,2}) {
        const bool fixed = import_route != 0;
        const bool captained = import_route == 2;
        const bool node_deferred = captained || !fixed;
        const std::vector<Edge> edges = paired
            ? std::vector<Edge>{{0,1,4}, {1,2,6}, {0,-1,8}}
            : std::vector<Edge>{{0,1,4}, {0,-1,2}};
        const std::vector<std::array<int64_t,3>> coordinates = paired
            ? std::vector<std::array<int64_t,3>>{{0,0,0}, {2,0,0}, {4,0,0}}
            : std::vector<std::array<int64_t,3>>{{0,0,0}, {2,0,0}};
        const std::vector<uint64_t> events = paired
            ? std::vector<uint64_t>{0,1,2} : std::vector<uint64_t>{0,1};
        const auto expected = generic_result(coordinates.size(), edges, events);
        auto decoder = make_decoder(coordinates, edges);
        auto& pool = *decoder.flooder.regions;
        std::array<PredecodeSeed,9> backing{};
        for (int repeat = 0; repeat < 3; ++repeat) {
            SCOPED_TRACE(::testing::Message() << "pair=" << paired << " route=" << import_route
                << " output=" << output << " repeat=" << repeat);
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            const auto dense_view = decoder.predecoder.seed_view;
            auto dense = dense_view.storage;
            dense[0].radius = 2;
            if (paired) {
                dense[0].mate = 1; dense[1].mate = 0;
                dense[1].radius = 2;
                dense[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(&decoder.flooder.graph.node(1));
                dense[1].match_slot = dense[0].match_slot ^ 1;
            } else {
                dense[0].mate = PredecodeSeed::BOUNDARY;
                dense[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(nullptr);
            }
            dense.back().radius = 2;
            if (fixed) {
                for (size_t i = 0; i < dense.size(); ++i) backing[3*i] = dense[i];
                if (paired) { backing[0].mate = 3; backing[3].mate = 0; }
                decoder.predecoder.seed_view.bind_fixed(backing, std::array<size_t,4>{0,3,6,9});
                decoder.predecoder.seed_view.set_counts(std::array<size_t,3>{1,1,paired ? 1U : 0U});
                if (captained) {
                    import_captained(decoder);
                } else {
                    const auto first = decoder.prepare_predecode_import(dense.size());
                    decoder.import_fixed_predecode_range<2>(decoder.predecoder.seed_view, first, 0, dense.size());
                    decoder.finish_fixed_predecode_import(decoder.predecoder.seed_view, first);
                }
            } else {
                decoder.import_predecode_state(dense);
            }
            ASSERT_EQ(decoder.active_tree_count(), 1U);
            for (RegionId id = 1; id < events.size(); ++id) {
                ASSERT_TRUE(pool.is_lazy_seed(id));
                EXPECT_EQ(pool.is_seed_node_deferred(id), node_deferred);
                if (node_deferred) expect_zero_seed_node(decoder.flooder.graph.node(id - 1));
            }
            ASSERT_FALSE(pool.is_lazy_seed(static_cast<RegionId>(events.size())));
            const auto collision = decoder.flooder.run_until_next_mwpm_notification();
            ASSERT_EQ(collision.event_type, REGION_HIT_REGION);
            const auto& hit = collision.region_hit_region_event_data;
            EXPECT_EQ(hit.region1, events.size());
            EXPECT_EQ(hit.region2, paired ? 2U : 1U);
            EXPECT_EQ(hit.edge.loc_from, &decoder.flooder.graph.node(events.size() - 1));
            EXPECT_EQ(hit.edge.loc_to, &decoder.flooder.graph.node(paired ? 1 : 0));
            EXPECT_EQ(hit.edge.obs_mask, paired ? 2U : 1U);
            decoder.process_event(collision);
            for (RegionId id = 1; id < events.size(); ++id) {
                EXPECT_FALSE(pool.is_lazy_seed(id));
                EXPECT_FALSE(pool.is_unchanged_seed(id));
                EXPECT_FALSE(pool.is_seed_node_deferred(id));
                const auto& node = decoder.flooder.graph.node(id - 1);
                EXPECT_EQ(node.reached_from_source, &node);
                EXPECT_EQ(node.region_that_arrived, id);
            }
            EXPECT_EQ(decoder.active_tree_count(), paired ? 1U : 0U);
            if (output == 2) {
                decode_prepared_detection_events_to_normalized_match_edges(decoder, false, true);
                expect_match_edges(decoder, paired
                    ? std::vector<std::array<uint64_t,3>>{{0,UINT64_MAX,4}, {1,2,2}}
                    : std::vector<std::array<uint64_t,3>>{{0,1,1}});
                pm::obs_int obs = 0;
                for (const auto& edge : decoder.flooder.match_edges) obs ^= edge.obs_mask;
                EXPECT_EQ(obs, expected.obs_mask);
                decoder.flooder.match_edges.clear();
            } else {
                const auto result = output == 1 ? solve_prepared_detection_events_deferring_seed_nodes(decoder)
                    : decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true);
                EXPECT_EQ(result, expected);
                if (output == 1) decoder.clear_pending_seed_nodes();
            }
            // A materialized matched endpoint must participate in final node
            // cleanup; retaining its former deferred flag would skip these writes.
            for (size_t i = 0; i < events.size(); ++i)
                expect_zero_seed_node(decoder.flooder.graph.node(i));
            expect_clean(decoder);
            EXPECT_TRUE(pool.empty());
            const auto end = pool.take_neutral_reset_end();
            if (end) pool.prepare_neutral_range(1, end);
            decoder.predecoder.seed_view = dense_view;
        }
    }
}

TEST(StructuredActiveTrees, ZeroOddEvenFailureAndMovesAcrossRepeatedShots) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,8}, {2,3,12}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, edges);
    for (size_t repeat = 0; repeat < 3; ++repeat) for (size_t mask = 0; mask < 16; ++mask) {
        std::vector<uint64_t> events;
        for (size_t k = 0; k < 4; ++k) if (mask & (size_t{1} << k)) events.push_back(k);
        prepare_cold(decoder, events);
        ASSERT_EQ(decoder.active_tree_count(), events.size());
        Mwpm moved(std::move(decoder));
        EXPECT_EQ(decoder.active_tree_count(), 0);
        EXPECT_EQ(moved.active_tree_count(), events.size());
        decoder = std::move(moved);
        EXPECT_EQ(moved.active_tree_count(), 0);
        decoder.verify_invariants();
        if (events.size() % 2) {
            EXPECT_THROW(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true), std::invalid_argument);
        } else {
            EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                generic_result(4, edges, events));
        }
        expect_clean(decoder);
    }
    prepare_cold(decoder, {0,3});
    decoder.reset();
    expect_clean(decoder);
}
#endif
