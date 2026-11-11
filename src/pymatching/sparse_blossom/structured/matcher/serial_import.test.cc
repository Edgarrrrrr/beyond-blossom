// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/flooder/region_pool.test.h"
#include "pymatching/sparse_blossom/structured/matcher/serial_import.test.h"

#if PM_PREDECODE_ENABLED
namespace pm::structured {
// Test the existing completion helper without changing the public driver API.
void process_prepared_timeline_until_completion(Mwpm&, bool, bool);

}

namespace {
using namespace pm::structured;
struct Edge { size_t u; int v; int weight; };

Mwpm make_decoder(const std::vector<std::array<int64_t, 3>>& coordinates, const std::vector<Edge>& edges) {
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
    oracle.flooder.sync_negative_weight_observables_and_detection_events();
    return pm::decode_detection_events_for_up_to_64_observables(oracle, events, false);
}

void expect_match_edges(const Mwpm& decoder, std::vector<std::array<uint64_t, 3>> expected) {
    std::vector<std::array<uint64_t, 3>> actual;
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

void expect_zero_seed_node(const DetectorNode& node) {
    EXPECT_EQ(node.region_that_arrived, NO_REGION);
    EXPECT_EQ(node.reached_from_source, nullptr);
    EXPECT_EQ(node.radius_of_arrival, 0);
    EXPECT_EQ(node.observables_crossed_from_source, 0U);
    EXPECT_FALSE(node.node_event_tracker.has_queued_time);
    EXPECT_FALSE(node.node_event_tracker.has_desired_time);
}

void expect_clean(const Mwpm& decoder) {
    EXPECT_TRUE(decoder.flooder.queue.empty());
    EXPECT_TRUE(decoder.flooder.regions->empty());
    EXPECT_EQ(decoder.node_arena.allocated.size(), decoder.node_arena.available.size());
    for (size_t i = 0; i < decoder.flooder.graph.num_nodes; ++i) {
        const auto& node = decoder.flooder.graph.node(i);
        expect_zero_seed_node(node);
        EXPECT_EQ(node.top_region_id(), NO_REGION);
    }
}

void set_pair(Mwpm& decoder, std::span<PredecodeSeed> seeds) {
    seeds[0].mate = 1; seeds[1].mate = 0;
    seeds[0].radius = seeds[1].radius = 2;
    seeds[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(&decoder.flooder.graph.node(1));
    seeds[1].match_slot = seeds[0].match_slot ^ 1;
}

void import_materialized_range(Mwpm& decoder, std::span<const PredecodeSeed> seeds) {
    const auto first = decoder.prepare_predecode_import(seeds);
    decoder.import_predecode_range(seeds, first, 0, seeds.size());
    // Spatial's legacy ranges eagerly publish nodes while matched regions may
    // remain lazy. Explicitly materialize this control to exercise cached/live
    // object reuse; the separate fixed-range test checks the actual API contract.
    for (size_t i = 0; i < seeds.size(); ++i) (void)decoder.flooder.regions->get(first + RegionId(i));
    decoder.finish_predecode_import(seeds, first);
}

using FinishGrid = StaticGridComponent<DetectorNode, pm::obs_int>;
decltype(FinishGrid::matching_next) original_next = nullptr;
std::vector<uint64_t> event_order;
size_t fail_at = SIZE_MAX;
Mwpm* importing_decoder = nullptr;
bool all_roots_published = false;

pm::GridNextEvent observe_schedule(const FinishGrid& grid, const DetectorNode& source, const pm::VaryingCT* radii) {
    // Collision owners all exist before scheduling. The ordinary importer has
    // all roots ready; Spatial's legacy finish creates roots in event order.
    for (const auto& node : grid.nodes) EXPECT_NE(node.top_region_id(), NO_REGION);
    EXPECT_EQ(importing_decoder->node_arena.allocated.size() - importing_decoder->node_arena.available.size(),
              all_roots_published ? 3U : event_order.size() + 1);
    event_order.push_back(source.detector_id);
    if (event_order.size() == fail_at) throw std::runtime_error("failure after first queued root");
    return original_next(grid, source, radii);
}
}  // namespace

TEST(StructuredSerialImport, RootsAndResidualEventsPreserveLegacyOrderAndRollback) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,8}, {2,-1,6}};
    const std::vector<uint64_t> events{0,1,2};
    for (bool serial : {false, true}) for (bool metadata : {false, true}) {
        auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
        auto& grid = *decoder.flooder.graph.node(0).grid_component;
        original_next = grid.matching_next;
        importing_decoder = &decoder;
        all_roots_published = serial;
        for (bool fail : {true, false}) {
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            const auto owned_seeds = decoder.predecoder.seed_view.storage;
            std::vector<PredecodeSeed> seeds(owned_seeds.begin(), owned_seeds.end());
            if (!metadata) for (auto& seed : seeds) seed.grid = nullptr;
            grid.matching_next = observe_schedule;
            event_order.clear();
            fail_at = fail ? 2 : SIZE_MAX;
            const auto import = [&] {
                if (serial) decoder.import_predecode_state(seeds);
                else import_materialized_range(decoder, seeds);
            };
            if (fail) {
                EXPECT_THROW(import(), std::runtime_error);
                EXPECT_EQ(event_order, (std::vector<uint64_t>{0,1}));
                EXPECT_FALSE(decoder.flooder.queue.empty());
                grid.matching_next = original_next;
                decoder.reset();
            } else {
                import();
                EXPECT_EQ(event_order, events);
                grid.matching_next = original_next;
                EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                          generic_result(3, edges, events));
            }
            expect_clean(decoder);
        }
    }
    importing_decoder = nullptr;
}

TEST(StructuredSerialImport, UntouchedSeedsStayCompactAcrossExtractionModesAndReuse) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,3,100}, {2,-1,6}, {3,-1,8}};
    const std::vector<uint64_t> events{0,1,2,3};
    for (int output : {0,1,2}) {
        auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, edges);
        auto& pool = *decoder.flooder.regions;
        for (int repeat = 0; repeat < 4; ++repeat) {
            // Exercise raw lazy lifetimes, then materialized legacy lifetimes,
            // and finally publish a lazy seed into the retained singleton slot.
            const bool serial = repeat != 2;
            SCOPED_TRACE(::testing::Message() << "output=" << output << " repeat=" << repeat);
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            auto seeds = decoder.predecoder.seed_view.storage;
            set_pair(decoder, seeds);
            seeds[2].mate = PredecodeSeed::BOUNDARY; seeds[2].radius = 6;
            seeds[2].match_slot = decoder.flooder.graph.node(2).index_of_neighbor(nullptr);
            seeds[3].radius = 2;
            if (serial) decoder.import_predecode_state(seeds);
            else import_materialized_range(decoder, seeds);
            for (RegionId id : {1U,2U,3U}) {
                ASSERT_TRUE(pool.is_unchanged_seed(id));
                EXPECT_EQ(pool.is_lazy_seed(id), serial);
                EXPECT_EQ(pool.is_seed_node_deferred(id), serial);
                const auto& node = decoder.flooder.graph.node(id - 1);
                if (serial) expect_zero_seed_node(node);
                else EXPECT_EQ(node.region_that_arrived, id);
                EXPECT_EQ(node.top_region_id(), id);
            }
            // Completion must retire tentative events and their trackers after
            // the final augmentation, including the active-tree early exit.
            process_prepared_timeline_until_completion(decoder, false, true);
            EXPECT_TRUE(decoder.flooder.queue.empty());
            for (RegionId id : {1U,2U,3U}) {
                EXPECT_TRUE(pool.is_unchanged_seed(id));
                EXPECT_EQ(pool.is_lazy_seed(id), serial);
            }
            if (output == 2) {
                decode_prepared_detection_events_to_normalized_match_edges(decoder, false, true);
                expect_match_edges(decoder, {{0,1,1}, {2,UINT64_MAX,8}, {3,UINT64_MAX,16}});
                decoder.flooder.match_edges.clear();
            } else {
                const auto result = output == 1 ? solve_prepared_detection_events_deferring_seed_nodes(decoder)
                    : decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true);
                EXPECT_EQ(result, generic_result(4, edges, events));
                if (output == 1) {
                    for (RegionId id : {1U,2U,3U}) {
                        EXPECT_EQ(pool.is_seed_node_deferred(id), serial);
                        if (serial) expect_zero_seed_node(decoder.flooder.graph.node(id - 1));
                    }
                    decoder.clear_pending_seed_nodes();
                }
            }
            for (RegionId id : {1U,2U,3U}) {
                EXPECT_EQ(pool.is_seed_node_deferred(id), serial);
                EXPECT_FALSE(pool.is_live(id, pool.generation(id)));
                if (repeat < 2) EXPECT_TRUE(pool.is_deferred_seed_slot(id));
                else EXPECT_TRUE(pool.is_cached_seed_slot(id));
            }
            EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                      (pm::MatchingResult{}));
            expect_clean(decoder);
            const auto end = pool.take_neutral_reset_end();
            if (end) pool.prepare_neutral_range(1, end);
        }
    }
}

TEST(StructuredSerialImport, BoundaryAndPairCollisionsMaterializeBeforeReadingPaths) {
    for (bool paired : {false, true}) for (bool serial : {false, true}) for (int output : {0,1,2}) {
        const std::vector<Edge> edges = paired ? std::vector<Edge>{{0,1,4}, {1,2,6}, {0,-1,8}}
                                             : std::vector<Edge>{{0,1,4}, {0,-1,2}};
        const std::vector<std::array<int64_t,3>> coordinates = paired
            ? std::vector<std::array<int64_t,3>>{{0,0,0}, {2,0,0}, {4,0,0}}
            : std::vector<std::array<int64_t,3>>{{0,0,0}, {2,0,0}};
        const std::vector<uint64_t> events = paired ? std::vector<uint64_t>{0,1,2} : std::vector<uint64_t>{0,1};
        const auto expected = generic_result(coordinates.size(), edges, events);
        auto decoder = make_decoder(coordinates, edges);
        auto& pool = *decoder.flooder.regions;
        for (int repeat = 0; repeat < 3; ++repeat) {
            SCOPED_TRACE(::testing::Message() << "paired=" << paired << " serial=" << serial
                << " output=" << output << " repeat=" << repeat);
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            auto seeds = decoder.predecoder.seed_view.storage;
            if (paired) set_pair(decoder, seeds);
            else {
                seeds[0].mate = PredecodeSeed::BOUNDARY; seeds[0].radius = 2;
                seeds[0].match_slot = decoder.flooder.graph.node(0).index_of_neighbor(nullptr);
            }
            seeds.back().radius = 2;
            if (serial) decoder.import_predecode_state(seeds);
            else import_materialized_range(decoder, seeds);
            for (RegionId id = 1; id < events.size(); ++id) {
                EXPECT_EQ(pool.is_lazy_seed(id), serial);
                EXPECT_EQ(pool.is_seed_node_deferred(id), serial);
                if (serial) expect_zero_seed_node(decoder.flooder.graph.node(id - 1));
            }
            // Do not inspect region/owner accessors before the real collision.
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
                EXPECT_EQ(decoder.flooder.graph.node(id - 1).reached_from_source,
                          &decoder.flooder.graph.node(id - 1));
            }
            if (output == 2) {
                decode_prepared_detection_events_to_normalized_match_edges(decoder, false, true);
                expect_match_edges(decoder, paired
                    ? std::vector<std::array<uint64_t,3>>{{0,UINT64_MAX,4}, {1,2,2}}
                    : std::vector<std::array<uint64_t,3>>{{0,1,1}});
                decoder.flooder.match_edges.clear();
            } else {
                const auto result = output == 1 ? solve_prepared_detection_events_deferring_seed_nodes(decoder)
                    : decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true);
                EXPECT_EQ(result, expected);
                if (output == 1) decoder.clear_pending_seed_nodes();
            }
            expect_clean(decoder);
            const auto end = pool.take_neutral_reset_end();
            if (end) pool.prepare_neutral_range(1, end);
        }
    }
}

TEST(StructuredSerialImport, ExplicitNodeQueriesAndHardResetRespectDeferredOwnership) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,-1,6}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
    for (int query : {0,1,2}) {
        decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
        auto seeds = decoder.predecoder.seed_view.storage;
        set_pair(decoder, seeds);
        decoder.import_predecode_state(seeds);
        auto& node = decoder.flooder.graph.node(0);
        ASSERT_TRUE(decoder.flooder.regions->is_seed_node_deferred(1));
        expect_zero_seed_node(node);
        if (query == 0) EXPECT_NE(node.owner_region(), nullptr);
        else if (query == 1) EXPECT_EQ(node.compute_wrapped_radius(), 0);
        else decoder.flooder.reschedule_events_at_detector_node(node);
        EXPECT_EQ(node.region_that_arrived, 1U);
        EXPECT_EQ(node.reached_from_source, &node);
        EXPECT_FALSE(decoder.flooder.regions->is_seed_node_deferred(1));
        EXPECT_TRUE(decoder.flooder.regions->is_unchanged_seed(1));
        decoder.reset();
        EXPECT_FALSE(decoder.flooder.regions->is_seed_node_deferred(1));
        EXPECT_FALSE(decoder.flooder.regions->is_seed_node_deferred(2));
        expect_clean(decoder);
    }
}

TEST(StructuredSerialImport, EmptyOddEvenFailureAndMovesPreserveCleanup) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,8}, {2,3,12}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, edges);
    for (size_t repeat = 0; repeat < 3; ++repeat) for (size_t mask = 0; mask < 16; ++mask) {
        std::vector<uint64_t> events;
        for (size_t k = 0; k < 4; ++k) if (mask & (size_t{1} << k)) events.push_back(k);
        decoder.predecoder.prepare(decoder.flooder.graph, events, {});
        decoder.import_predecode_state(decoder.predecoder.seed_view.storage);
        Mwpm moved(std::move(decoder));
        decoder = std::move(moved);
        if (events.size() % 2) {
            EXPECT_THROW(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true), std::invalid_argument);
        } else {
            EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                      generic_result(4, edges, events));
        }
        expect_clean(decoder);
    }
}

TEST(StructuredSerialImport, SpatialFixedRangesKeepEagerNodesAndRankIndexedExtraction) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,-1,6}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
    std::array<PredecodeSeed, 9> backing{};
    decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
    const auto dense_view = decoder.predecoder.seed_view;
    set_pair(decoder, dense_view.storage);
    for (size_t i = 0; i < 3; ++i) backing[3 * i] = dense_view.storage[i];
    backing[0].mate = 3;
    decoder.predecoder.seed_view.bind_fixed(backing, std::array<size_t, 4>{0,3,6,9});
    decoder.predecoder.seed_view.set_counts(std::array<size_t, 3>{1,1,1});
    const auto first = decoder.prepare_predecode_import(3);
    decoder.import_fixed_predecode_range<2>(decoder.predecoder.seed_view, first, 0, 1);
    decoder.import_fixed_predecode_range<2>(decoder.predecoder.seed_view, first, 1, 3);
    decoder.finish_fixed_predecode_import(decoder.predecoder.seed_view, first);
    for (RegionId id : {1U,2U,3U}) {
        EXPECT_EQ(decoder.flooder.graph.node(id - 1).region_that_arrived, id);
        EXPECT_EQ(decoder.flooder.regions->is_lazy_seed(id), id < 3);
        EXPECT_FALSE(decoder.flooder.regions->is_seed_node_deferred(id));
    }
    EXPECT_EQ(solve_prepared_detection_events_deferring_seed_nodes(decoder), generic_result(3, edges, {0,1,2}));
    decoder.clear_pending_seed_nodes();
    expect_clean(decoder);
    decoder.predecoder.seed_view = dense_view;
}

TEST(StructuredSerialImport, CompactAndPointerEndpointsSurviveMovesAndAlternatingImporters) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,-1,6}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
    ASSERT_TRUE(SerialImportTestAccess::direct_bound(decoder));
    for (unsigned repeat = 0; repeat < 6; ++repeat) {
        decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
        auto seeds = decoder.predecoder.seed_view.storage;
        set_pair(decoder, seeds);
        seeds[2].mate = PredecodeSeed::BOUNDARY;
        seeds[2].radius = 6;
        seeds[2].match_slot = decoder.flooder.graph.node(2).index_of_neighbor(nullptr);
        std::vector<PredecodeSeed> external(seeds.begin(), seeds.end());
        for (auto& seed : external) seed.grid = nullptr;
        const bool direct = repeat % 2 == 0;
        if (direct) decoder.import_predecode_state(external);
        else import_materialized_range(decoder, external);
        // Import owns all descriptors; later input storage is irrelevant.
        external.assign(external.size(), {});
        Mwpm moved(std::move(decoder));
        decoder = std::move(moved);
        ASSERT_TRUE(SerialImportTestAccess::direct_bound(decoder));
        auto& pool = *decoder.flooder.regions;
        for (RegionId id : {1U,2U,3U}) {
            EXPECT_EQ(RegionPoolTestAccess::compact_endpoints(pool, id), direct);
            EXPECT_EQ(pool.seed_node(id), &decoder.flooder.graph.node(id - 1));
        }
        // A read may materialize one endpoint; unchanged eligibility survives.
        EXPECT_EQ(pool.get(1)->match.edge.loc_to, &decoder.flooder.graph.node(1));
        EXPECT_TRUE(pool.is_unchanged_seed(1));
        EXPECT_FALSE(RegionPoolTestAccess::compact_endpoints(pool, 1));
        EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                  generic_result(3, edges, {0,1,2}));
        expect_clean(decoder);
    }
}

TEST(StructuredSerialImport, DirectBindingChecksEveryPublicIdAndFallsBackForPermutedNodes) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,3,6}};
    MatchingGraph graph(4, edges.size());
    for (size_t i = 0; i < edges.size(); ++i)
        graph.add_edge(edges[i].u, edges[i].v, edges[i].weight, {i});
    graph.configure_grid({5, 5, false, {{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, {}, pm::GridSimd::scalar});
    // Relabel two public IDs within the same component before construction.
    // Full-vertex parity and the original edge observables are unchanged.
    std::swap(graph.grid->detector_nodes[0], graph.grid->detector_nodes[3]);
    graph.node(0).detector_id = 0;
    graph.node(3).detector_id = 3;
    GraphFlooder flooder(std::move(graph));
    flooder.sync_negative_weight_observables_and_detection_events();
    Mwpm decoder(std::move(flooder));
    EXPECT_FALSE(SerialImportTestAccess::direct_bound(decoder));
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        Mwpm moved(std::move(decoder));
        decoder = std::move(moved);
        EXPECT_FALSE(SerialImportTestAccess::direct_bound(decoder));
        decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2,3}, {});
        std::vector<PredecodeSeed> seeds(decoder.predecoder.seed_view.storage.begin(), decoder.predecoder.seed_view.storage.end());
        for (auto& seed : seeds) seed.grid = nullptr;
        decoder.import_predecode_state(seeds);
        EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                  generic_result(4, edges, {0,1,2,3}));
        expect_clean(decoder);
    }
}

TEST(StructuredSerialImport, CaptainedCompactEndpointsKeepFixedRanksAcrossMovesAndFallbackMappings) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,-1,6}};
    const auto expected = generic_result(3, edges, {0,1,2});
    for (bool mapped : {false, true}) {
        MatchingGraph graph(3, edges.size());
        graph.add_edge(0, 1, 4, {0});
        graph.add_edge(1, 2, 100, {1});
        graph.add_boundary_edge(2, 6, {2});
        graph.configure_grid({5, 5, false, {{0,0,0}, {2,0,0}, {4,0,0}}, {}, pm::GridSimd::scalar});
        if (mapped) {
            std::swap(graph.grid->detector_nodes[0], graph.grid->detector_nodes[2]);
            graph.node(0).detector_id = 0;
            graph.node(2).detector_id = 2;
        }
        Mwpm decoder{GraphFlooder(std::move(graph))};
        EXPECT_EQ(SerialImportTestAccess::direct_bound(decoder), !mapped);
        const auto check = [&]<size_t Parts>() {
            for (bool scratch_path : {false, true}) for (bool rollback : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "parts=" << Parts << " mapped=" << mapped
                    << " scratch=" << scratch_path << " rollback=" << rollback);
                decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
                auto& pre = decoder.predecoder;
                pre.run();
                ASSERT_TRUE(pre.complete);
                const auto dense = pre.seed_view;
                std::array<PredecodeSeed, (Parts + 1) * 4> backing{};
                std::array<size_t, Parts + 2> bases{};
                for (size_t b = 0; b < bases.size(); ++b) bases[b] = 4 * b;
                std::array<size_t, Parts + 1> counts{};
                counts[0] = counts[1] = counts[Parts] = 1;
                const std::array<uint32_t, 3> slots{0, 4, uint32_t(4 * Parts)};
                for (size_t rank = 0; rank < 3; ++rank) {
                    auto seed = dense.storage[rank];
                    if (seed.mate < PredecodeSeed::BOUNDARY) seed.mate = slots[seed.mate];
                    seed.grid->predecode.active[seed.cell] = slots[rank] + 1;
                    backing[slots[rank]] = seed;
                }
                // This fixture imports the precomputed matches to exercise the
                // importer; their cost must be extracted exactly once by MWPM.
                pre.complete = pre.result_pending = false;
                pre.result = {};
                pre.seed_view.bind_fixed(backing, bases);
                pre.seed_view.set_counts(counts);
                auto input_backing = backing;
                for (auto slot : slots) input_backing[slot].grid = nullptr;
                SeedView input;
                input.bind_fixed(input_backing, bases);
                input.set_counts(counts);
                PredecodeImportScratch scratch;
                scratch.roots.reserve(3);
                const auto first = decoder.prepare_predecode_import(3);
                const size_t end = rollback ? 1 : 3;
                if (scratch_path)
                    decoder.import_fixed_predecode_range_captained<Parts>(input, first, 0, end, scratch);
                else
                    decoder.import_fixed_predecode_range_captained_no_scratch<Parts>(input, first, 0, end);
                EXPECT_EQ(decoder.active_tree_count(), 0U);
                EXPECT_TRUE(scratch.roots.empty());
                auto& pool = *decoder.flooder.regions;
                EXPECT_FALSE(pool.has_borrowed_seeds());
                for (RegionId id = first; id < first + end; ++id) {
                    EXPECT_TRUE(pool.is_lazy_seed(id));
                    EXPECT_TRUE(pool.is_seed_node_deferred(id));
                    EXPECT_EQ(RegionPoolTestAccess::compact_endpoints(pool, id), !mapped);
                    EXPECT_EQ(pool.seed_node(id), &decoder.flooder.graph.node(id - first));
                    expect_zero_seed_node(decoder.flooder.graph.node(id - first));
                }
                // Move only after all importer work has joined. Both bound
                // captained entry tables and the compact node base must survive.
                Mwpm moved(std::move(decoder));
                decoder = std::move(moved);
                EXPECT_EQ(SerialImportTestAccess::direct_bound(decoder), !mapped);
                if (rollback) {
                    decoder.reset();
                } else {
                    EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true), expected);
                }
                expect_clean(decoder);
                // The backing array is local; retire its view before it expires.
                decoder.predecoder.seed_view = dense;
                decoder.reset();
            }
        };
        check.template operator()<2>();
        check.template operator()<4>();
        check.template operator()<8>();
    }
}

TEST(StructuredSerialImport, BorrowedNormalEntryMatchesOwnedAcrossTiesAndInputPermutations) {
    for (const auto weights : {std::array<int,5>{0,0,0,2,2}, {2,2,2,2,2}, {4,8,12,6,8}}) {
        const std::vector<Edge> edges{{0,1,weights[0]}, {1,2,weights[1]}, {2,3,weights[2]},
                                      {2,-1,weights[3]}, {3,-1,weights[4]}};
        const std::vector<std::array<int64_t,3>> coords{{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}};
        auto candidate = make_decoder(coords, edges);
        auto owned = make_decoder(coords, edges);
        SerialImportTestAccess::use_owned_packed_import(owned);
        for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned mask = 0; mask < 16; ++mask) {
            std::vector<uint64_t> events;
            for (unsigned i = 0; i < 4; ++i) if (mask & (1U << i)) events.push_back(i);
            do {
                SCOPED_TRACE(::testing::Message() << "repeat=" << repeat << " mask=" << mask);
                const auto actual = decode_detection_events_for_up_to_64_observables(candidate, events, false);
                const auto expected = decode_detection_events_for_up_to_64_observables(owned, events, false);
                EXPECT_EQ(actual, expected);
                EXPECT_EQ(actual.weight, generic_result(4, edges, events).weight);
                EXPECT_FALSE(candidate.flooder.regions->has_borrowed_seeds());
                expect_clean(candidate);
                expect_clean(owned);
            } while (std::next_permutation(events.begin(), events.end()));
        }
        // Both decoders used the real normal entry. Only one is permitted to
        // borrow; the retained extraction bitmap proves that path ran.
        EXPECT_TRUE(SerialImportTestAccess::used_borrowed_extraction(candidate));
        EXPECT_FALSE(SerialImportTestAccess::used_borrowed_extraction(owned));
    }
}

TEST(StructuredSerialImport, BorrowedPairAndBoundaryTouchesPreserveMovesAndDeferredCleanup) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,3,100}, {2,-1,6}, {3,-1,8}};
    const std::vector<uint64_t> events{0,1,2,3};
    for (bool deferred : {false, true}) {
        auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, edges);
        for (unsigned touched : {0U,1U,2U,3U,4U,7U,0U}) {
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            auto seeds = decoder.predecoder.seed_view.storage;
            set_pair(decoder, seeds);
            seeds[2].mate = PredecodeSeed::BOUNDARY;
            seeds[2].radius = 6;
            seeds[2].match_slot = decoder.flooder.graph.node(2).index_of_neighbor(nullptr);
            seeds[3].radius = 2;
            decoder.predecoder.result.obs_mask = (uint64_t{1} << 0) | (uint64_t{1} << 3);
            decoder.predecoder.result.weight = 10;
            decoder.import_packed_predecode_state();
            auto& pool = *decoder.flooder.regions;
            ASSERT_TRUE(pool.has_borrowed_seeds());
            ASSERT_EQ(pool.changed_initial_seeds().size(), 1U);
            EXPECT_EQ(pool.changed_initial_seeds()[0], 4U);
            for (RegionId id : {1U,2U,3U}) {
                ASSERT_TRUE(pool.is_lazy_seed(id));
                if (touched & (1U << (id - 1))) {
                    const auto before = pool.changed_initial_seeds().size();
                    const RegionPool& logical_const = pool;
                    EXPECT_NE(logical_const.get(id), nullptr);
                    EXPECT_EQ(pool.changed_initial_seeds().size(), before + 1);
                    EXPECT_NE(logical_const.get(id), nullptr);
                    EXPECT_EQ(pool.changed_initial_seeds().size(), before + 1);
                }
            }
            Mwpm moved(std::move(decoder));
            decoder = std::move(moved);
            const auto actual = deferred ? solve_prepared_detection_events_deferring_seed_nodes(decoder) :
                decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true);
            EXPECT_EQ(actual, generic_result(4, edges, events));
            EXPECT_FALSE(pool.has_borrowed_seeds());
            EXPECT_TRUE(pool.changed_initial_seeds().empty());
            EXPECT_TRUE(pool.empty());
            if (deferred) {
                EXPECT_TRUE(decoder.has_pending_seed_cleanup());
                decoder.clear_pending_seed_nodes();
            }
            expect_clean(decoder);
            decoder.reset();
            EXPECT_FALSE(pool.has_borrowed_seeds());
            expect_clean(decoder);
        }
    }
}

TEST(StructuredSerialImport, BorrowedSchedulingFailureRollsBackBeforeOrdinaryReuse) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,8}, {2,-1,6}};
    auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, edges);
    auto& grid = *decoder.flooder.graph.node(0).grid_component;
    original_next = grid.matching_next;
    importing_decoder = &decoder;
    all_roots_published = true;
    for (bool fail : {true, false, true, false}) {
        decoder.predecoder.prepare(decoder.flooder.graph, {0,1,2}, {});
        grid.matching_next = observe_schedule;
        event_order.clear();
        fail_at = fail ? 2 : SIZE_MAX;
        if (fail) {
            EXPECT_THROW(prepare_packed_timeline_for_mwpm(decoder, false), std::runtime_error);
            EXPECT_TRUE(decoder.flooder.regions->has_borrowed_seeds());
            grid.matching_next = original_next;
            decoder.reset();
            auto& pool = *decoder.flooder.regions;
            EXPECT_FALSE(pool.has_borrowed_seeds());
            EXPECT_TRUE(pool.changed_initial_seeds().empty());
            auto* ordinary = pool.allocate();
            EXPECT_FALSE(pool.is_unchanged_seed(ordinary->region_id));
            EXPECT_TRUE(pool.changed_initial_seeds().empty());
            pool.release(ordinary->region_id);
        } else {
            prepare_packed_timeline_for_mwpm(decoder, false);
            EXPECT_TRUE(decoder.flooder.regions->has_borrowed_seeds());
            grid.matching_next = original_next;
            EXPECT_EQ(decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true),
                      generic_result(3, edges, {0,1,2}));
        }
        expect_clean(decoder);
    }
    importing_decoder = nullptr;
}

TEST(StructuredSerialImport, BorrowedFixedRanksSurviveSlotReuseAndPartialRollback) {
    for (unsigned kind : {0U,1U,2U}) {
        auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}}, {{0,1,4}, {1,2,100}, {2,-1,6}});
        auto& pool = *decoder.flooder.regions;
        auto& graph = decoder.flooder.graph;
        if (kind != 0) {
            pool.reserve_contiguous_ids(3);
            for (RegionId id : {1U,2U,3U}) {
                if (kind == 1) {
                    pool.construct_reserved(id);
                    pool.release(id);
                } else {
                    auto* node = &graph.node(id - 1);
                    pool.construct_reserved_seed(id, pm::VaryingCT::from_base_and_growth(2, 0), node,
                        {NO_REGION, {node, nullptr, 5}});
                    pool.release_unchanged_seed(id);
                }
            }
            pool.mark_completed_shot();
            pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
        }
        decoder.predecoder.prepare(graph, {0,1,2}, {});
        set_pair(decoder, decoder.predecoder.seed_view.storage);
        auto& boundary = decoder.predecoder.seed_view.storage[2];
        boundary.mate = PredecodeSeed::BOUNDARY;
        boundary.radius = 6;
        boundary.match_slot = graph.node(2).index_of_neighbor(nullptr);
        std::array<PredecodeSeed,9> backing{};
        for (auto& seed : backing) seed.detector_id = UINT32_MAX;
        for (size_t i = 0; i < 3; ++i) backing[3 * i] = decoder.predecoder.seed_view.storage[i];
        backing[0].mate = 3;
        SeedView fixed;
        fixed.bind_fixed(backing, std::array<size_t,4>{0,3,6,9});
        fixed.set_counts(std::array<size_t,3>{1,1,1});
        const auto borrowed_copy = fixed;
        pool.reserve_contiguous_ids(3);
        pool.borrow_seeds(fixed);
        fixed.retire();  // The pool owns the range metadata, not its address.
        pool.publish_reserved_borrowed_seed(2, pm::VaryingCT::from_base_and_growth(2, 0));
        EXPECT_EQ(pool.borrowed_seed(2).detector_id, 1U);
        EXPECT_EQ(pool.get(2)->match, (Match{1, {&graph.node(1), &graph.node(0), 1}}));
        ASSERT_EQ(pool.changed_initial_seeds().size(), 1U);
        EXPECT_EQ(pool.changed_initial_seeds()[0], 2U);
        pool.publish_reserved_borrowed_seed(1, pm::VaryingCT::from_base_and_growth(2, 0));
        pool.publish_reserved_borrowed_seed(3, pm::VaryingCT::from_base_and_growth(6, 0));
        EXPECT_EQ(pool.seed_extraction(3).obs_mask, 4U);
        EXPECT_EQ(pool.seed_extraction(3).mate, NO_REGION);
        pool.release(2);
        graph.node(1).reset();
        auto* ordinary = pool.allocate();
        ASSERT_EQ(ordinary->region_id, 2U);
        EXPECT_FALSE(pool.is_unchanged_seed(2));
        EXPECT_EQ(pool.changed_initial_seeds().size(), 1U);
        pool.release(2);
        for (RegionId id : {1U,2U,3U}) pool.finish_borrowed_seed(id);
        pool.finish_borrowed_seeds();
        pool.mark_completed_shot();
        EXPECT_FALSE(pool.has_borrowed_seeds());
        EXPECT_TRUE(pool.is_seed_node_deferred(1));
        EXPECT_FALSE(pool.is_seed_node_deferred(2));
        for (size_t slot : {1U,2U,4U,5U,7U,8U}) EXPECT_EQ(backing[slot].detector_id, UINT32_MAX);
        pool.reserve_contiguous_ids(3);
        pool.borrow_seeds(borrowed_copy);
        pool.publish_reserved_borrowed_seed(1, pm::VaryingCT::from_base_and_growth(2, 0));
        decoder.reset();  // Two reservations were never published.
        EXPECT_FALSE(pool.has_borrowed_seeds());
        EXPECT_TRUE(pool.changed_initial_seeds().empty());
        expect_clean(decoder);
    }
}

TEST(StructuredSerialImport, CaptainedBorrowedFixedRanksMoveAndRollback) {
    const std::vector<Edge> edges{{0,1,4}, {1,2,100}, {2,3,100}, {2,-1,6}, {3,-1,8}};
    const std::vector<uint64_t> events{0,1,2,3};
    const auto expected = generic_result(4, edges, events);
    const auto check = [&]<size_t Parts>() {
        for (bool empty_first : {false, true}) for (bool scratch_path : {false, true})
        for (bool deferred : {false, true}) for (unsigned touched : {0U,1U,2U,3U,4U,7U,8U}) {
            SCOPED_TRACE(::testing::Message() << "P=" << Parts << " empty-first=" << empty_first
                << " scratch=" << scratch_path << " deferred=" << deferred << " touched=" << touched);
            auto decoder = make_decoder({{0,0,0}, {2,0,0}, {4,0,0}, {6,0,0}}, edges);
            decoder.predecoder.prepare(decoder.flooder.graph, events, {});
            const auto dense = decoder.predecoder.seed_view;
            set_pair(decoder, dense.storage);
            dense.storage[2].mate = PredecodeSeed::BOUNDARY;
            dense.storage[2].radius = 6;
            dense.storage[2].match_slot = decoder.flooder.graph.node(2).index_of_neighbor(nullptr);
            dense.storage[3].radius = 2;
            auto& pre = decoder.predecoder;
            pre.result = {9, 10};
            pre.complete = pre.result_pending = false;
            std::array<PredecodeSeed, (Parts + 1) * 4> backing{};
            for (auto& seed : backing) seed.detector_id = UINT32_MAX;
            const std::array<uint32_t, 4> slots{empty_first ? 4U : 0U, empty_first ? 5U : 4U,
                uint32_t(4 * Parts), uint32_t(4 * Parts + 1)};
            std::array<size_t, Parts + 2> bases{};
            for (size_t b = 0; b < bases.size(); ++b) bases[b] = 4 * b;
            std::array<size_t, Parts + 1> counts{};
            counts[0] = empty_first ? 0 : 1;
            counts[1] = empty_first ? 2 : 1;
            counts[Parts] = 2;
            for (size_t rank = 0; rank < 4; ++rank) {
                auto seed = dense.storage[rank];
                if (seed.mate < PredecodeSeed::BOUNDARY) seed.mate = slots[seed.mate];
                seed.grid->predecode.active[seed.cell] = slots[rank] + 1;
                backing[slots[rank]] = seed;
            }
            pre.seed_view.bind_fixed(backing, bases);
            pre.seed_view.set_counts(counts);
            const auto first = decoder.prepare_packed_fixed_predecode_import();
            auto& pool = *decoder.flooder.regions;
            ASSERT_TRUE(pool.has_borrowed_seeds());
            PredecodeImportScratch scratch;
            scratch.roots.reserve(4);
            const bool rollback = touched == 8;
            const size_t end = rollback ? 1 : 4;
            if (scratch_path)
                decoder.import_fixed_predecode_range_captained<Parts, true>(pre.seed_view, first, 0, end, scratch);
            else
                decoder.import_fixed_predecode_range_captained_no_scratch<Parts, true>(pre.seed_view, first, 0, end);
            for (size_t rank = 0; rank < end; ++rank) {
                EXPECT_EQ(pool.borrowed_seed(first + rank).detector_id, rank);
                // Only matched seeds own an endpoint descriptor; roots use
                // their already-constructed region and published node fields.
                if (pool.borrowed_seed(first + rank).mate != PredecodeSeed::UNMATCHED)
                    EXPECT_EQ(pool.seed_node(first + rank), &decoder.flooder.graph.node(rank));
            }
            if (!rollback) {
                EXPECT_EQ(pool.changed_initial_seeds().size(), 1U);
                for (RegionId id : {1U,2U,3U}) if (touched & (1U << (id - 1))) {
                    const auto before = pool.changed_initial_seeds().size();
                    (void)pool.get(id);
                    (void)pool.get(id);
                    EXPECT_EQ(pool.changed_initial_seeds().size(), before + 1);
                }
                if (scratch_path) decoder.prepare_predecode_import_events(scratch);
                else decoder.prepare_fixed_predecode_events_from_seeds(pre.seed_view, first, 0, 4, scratch);
                decoder.publish_predecode_import_events(std::span<const PredecodeImportScratch>(&scratch, 1), 4);
            }
            Mwpm moved(std::move(decoder));
            decoder = std::move(moved);
            if (rollback) decoder.reset();
            else {
                EXPECT_EQ(deferred ? solve_prepared_detection_events_deferring_seed_nodes(decoder) :
                    decode_prepared_detection_events_for_up_to_64_observables(decoder, false, true), expected);
                EXPECT_TRUE(SerialImportTestAccess::used_borrowed_extraction(decoder));
                decoder.clear_pending_seed_nodes();
            }
            EXPECT_FALSE(pool.has_borrowed_seeds());
            EXPECT_TRUE(pool.changed_initial_seeds().empty());
            expect_clean(decoder);
            for (size_t slot = 0; slot < backing.size(); ++slot)
                if (std::find(slots.begin(), slots.end(), slot) == slots.end())
                    EXPECT_EQ(backing[slot].detector_id, UINT32_MAX);
            decoder.predecoder.seed_view = dense;
            decoder.reset();
            EXPECT_EQ(decode_detection_events_for_up_to_64_observables(decoder, events, false), expected);
            expect_clean(decoder);
        }
    };
    check.template operator()<2>();
    check.template operator()<4>();
    check.template operator()<8>();
}

#endif
