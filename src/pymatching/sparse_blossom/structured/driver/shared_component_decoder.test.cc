#include "pymatching/sparse_blossom/structured/predecode/one_hop_bound_kernel.h"
// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <thread>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif
#if defined(_OPENMP)
#include <omp.h>
#endif
#include "pymatching/sparse_blossom/structured/grid/static_grid_kernels.h"
#include "gtest/gtest.h"
#include "pymatching/sparse_blossom/driver/backend_user_graph.h"
#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/driver/shared_component_decoder.h"
#include "pymatching/sparse_blossom/structured/driver/time_partition_cuts.h"
#include "pymatching/sparse_blossom/structured/flooder/region_pool.test.h"
#include "pymatching/sparse_blossom/structured/matcher/serial_import.test.h"
using namespace pm;
using structured::ComponentDecoder;
using structured::SharedComponentDecoder;
using structured::DoubleBufferComponentDecoder;
static_assert(!std::is_copy_constructible_v<SharedComponentDecoder>);
namespace pm::structured {
struct OneHopPredecoderTestAccess {
    static auto scratch_storage(const OneHopPredecoder& p) {
        return std::array<std::pair<uintptr_t, size_t>, 3>{{
            {reinterpret_cast<uintptr_t>(p.candidate_masks.data()), p.candidate_masks.size()},
            {reinterpret_cast<uintptr_t>(p.degrees.data()), p.degrees.size()},
            {reinterpret_cast<uintptr_t>(p.order.data()), p.order.size()}}};
    }
};
struct ComponentDecoderTestAccess {
    static void stage_whole(ComponentDecoder& d, const std::vector<uint64_t>& events) { d.stage_component_shot(events); }
    static void scatter_whole(ComponentDecoder& d, std::span<const uint64_t> events, size_t c) {
        d.scatter_component_shot(events, c);
    }
    using BoundLayout = ComponentDecoder::BoundPartitionSeedLayout;
    static void bind(ComponentDecoder& d, size_t parts, size_t width,
                     TimePartitionCuts::Geometry geometry = TimePartitionCuts::Geometry::symmetric_width) {
        d.bind_partition_buckets(parts, width, geometry);
    }
    static auto geometry(const ComponentDecoder& d) { return d.bound_conflict_geometry; }
    static void stage_bound(ComponentDecoder& d, const std::vector<uint64_t>& events, std::span<BoundLayout> layouts) {
        d.stage_bound_partitioned_shot(events, layouts);
    }
    static void stage_group(ComponentDecoder& d, const std::vector<uint64_t>& events, std::span<BoundLayout> layouts) {
        d.stage_group_partitioned_shot(events, layouts);
    }
    static void fixed(ComponentDecoder& d) { d.bind_fixed_seed_storage(); }
    static const auto& fixed_bases(const ComponentDecoder& d) { return d.fixed_bucket_bases; }
    static const auto& component_bases(const ComponentDecoder& d) { return d.fixed_component_bases; }
    static const auto& index_storage(const ComponentDecoder& d) { return d.fixed_bucket_detector_ids; }
    static const auto& index_counts(const ComponentDecoder& d) { return d.fixed_index_counts; }
    static bool is_fixed(const ComponentDecoder& d) { return d.fixed_seed_storage; }
    static const auto& ranges(const ComponentDecoder& d) { return d.bound_detector_ranges; }
    static auto bounded_input(const ComponentDecoder& d, std::span<const uint64_t> events,
                              const BoundLayout& layout, size_t component, uint32_t mask) {
        return layout.bounded_input(events, mask, d.bound_detector_ranges[component]);
    }
    static void scatter_bound(ComponentDecoder& d, std::span<const uint64_t> events,
                              std::span<const BoundLayout> layouts, uint32_t mask) {
        d.scatter_bound_partitioned_shot(events, layouts, mask);
    }
    static void scatter_bound(ComponentDecoder& d, std::span<const uint64_t> events,
                              std::span<const BoundLayout> layouts, std::span<const uint32_t> masks) {
        if (!d.fixed_seed_storage) {
            d.scatter_bound_partitioned_shot(events, layouts, masks);
            return;
        }
        for (size_t c = 0; c < masks.size(); ++c) {
            if (!masks[c]) continue;
            std::array<size_t, 9> filled{}, counts{};
            auto& pre = d.components()[c].mwpm.predecoder;
            d.scatter_fixed_group(events, c, masks[c], filled);
            for (size_t b = 0; b <= d.bound_partition_count; ++b)
                counts[b] = (masks[c] & (1U << b)) ? filled[b] : pre.seed_view.active_ranges()[b].count();
            pre.seed_view.set_counts(std::span<const size_t>(counts).first(d.bound_partition_count + 1));
            pre.initialize_seed_counts();
        }
    }
    static auto binding(const ComponentDecoder& d) {
        return std::pair{d.bound_partition_count, d.bound_conflict_width};
    }
    static bool cached_stage(const ComponentDecoder& d) { return d.staged_partition_cached; }
    static std::vector<std::array<uint32_t, 5>> addresses(const ComponentDecoder& d) {
        std::vector<std::array<uint32_t, 5>> result;
        for (const auto& a : d.detector_addresses)
            result.push_back({a.component, a.bucket, a.local, a.cell, a.last_seen_epoch});
        return result;
    }
    static constexpr auto address_layout() {
        using A = ComponentDecoder::DetectorAddress;
        return std::array<size_t, 6>{sizeof(A), offsetof(A, component), offsetof(A, bucket),
            offsetof(A, local), offsetof(A, cell), offsetof(A, last_seen_epoch)};
    }
    static uint32_t epoch(const ComponentDecoder& d) { return d.shot_epoch; }
    static void epoch(ComponentDecoder& d, uint32_t value) { d.shot_epoch = value; }
    static std::vector<uint32_t> stamps(const ComponentDecoder& d) {
        std::vector<uint32_t> result;
        for (const auto& address : d.detector_addresses) result.push_back(address.last_seen_epoch);
        return result;
    }
    static void stamp_all(ComponentDecoder& d, uint32_t value) {
        for (auto& address : d.detector_addresses) address.last_seen_epoch = value;
    }
    static const std::vector<PredecodeSeed>& retired(const ComponentDecoder& d) { return d.retired_shot_seeds; }
    static bool staged(const ComponentDecoder& d) { return d.seed_storage_stage == ComponentDecoder::SeedStorageStage::staged; }
};
}
template <typename T> concept PublicBucketBinding = requires(T& d) { d.bind_partition_buckets(2, 3); };
template <typename T> concept PublicBoundLayout = requires { typename T::BoundPartitionSeedLayout; };
template <typename T> concept PublicWholeStage = requires(T& d, const std::vector<uint64_t>& events) {
    d.stage_component_shot(events);
};
static_assert(!PublicBucketBinding<ComponentDecoder> && !PublicBoundLayout<ComponentDecoder>);
static_assert(!PublicWholeStage<ComponentDecoder>);
static_assert(structured::ComponentDecoderTestAccess::address_layout() == std::array<size_t, 6>{16, 0, 2, 4, 8, 12});
#if PM_STRUCTURED_PROFILE_TIMERS
TEST(SharedComponents, ProfileLanesHaveDisjointCacheLines) {
    using Profile = structured::SharedComponentProfile;
    static_assert(alignof(Profile) >= 64 && alignof(Profile::Component) >= 64);
    static_assert(alignof(Profile::ComponentLane) >= 64 && sizeof(Profile::ComponentLane) % 64 == 0);
    static_assert(alignof(Profile::GlobalLane) >= 64 && sizeof(Profile::GlobalLane) % 64 == 0);
    auto profile = std::make_unique<Profile>();
    std::set<uintptr_t> occupied;
    const auto add_lane = [&](const auto& lane) {
        const auto begin = reinterpret_cast<uintptr_t>(&lane);
        EXPECT_EQ(begin % 64, 0U);
        for (size_t offset = 0; offset < sizeof(lane); offset += 64)
            EXPECT_TRUE(occupied.insert((begin + offset) / 64).second);
    };
    for (const auto& component : profile->component) {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(&component) % 64, 0U);
        for (const auto& lane : component.lanes) add_lane(lane);
    }
    for (const auto& lane : profile->lanes) add_lane(lane);
    // Captain/coordinator fields must not share even the edge lines of a lane.
    for (const auto& component : profile->component) {
        EXPECT_FALSE(occupied.contains(reinterpret_cast<uintptr_t>(&component.total_ns) / 64));
        EXPECT_FALSE(occupied.contains(reinterpret_cast<uintptr_t>(&component.group_prefix_ns) / 64));
#if PM_STRUCTURED_PROFILE_TIMELINE
        EXPECT_FALSE(occupied.contains(reinterpret_cast<uintptr_t>(&component.group_prefix_interval) / 64));
#endif
    }
    EXPECT_FALSE(occupied.contains(reinterpret_cast<uintptr_t>(&profile->partition_ns) / 64));
}
#endif
namespace {
StaticGridSpec pair_spec() {
    return {3,3,false,{{{0,0,0}},{{0,0,0}},{{2,0,0}},{{2,0,0}}},{},GridSimd::scalar};
}
BackendUserGraph pair_graph() {
    BackendUserGraph graph(4,2);
    graph.add_or_merge_edge(0,2,{0},2,-1);
    graph.add_or_merge_edge(1,3,{1},3,-1);
    graph.add_or_merge_boundary_edge(0,{1},7,-1);
    graph.add_or_merge_boundary_edge(1,{0},11,-1);
    return graph;
}
std::pair<stim::DetectorErrorModel, StaticGridSpec> model(bool rotated, size_t d, char basis) {
    std::string task = rotated ? "rotated_memory_" : "unrotated_memory_";
    task += basis;
    stim::CircuitGenParameters gen(d,d,task);
    gen.after_clifford_depolarization = 0.003;
    gen.after_reset_flip_probability = 0.003;
    gen.before_measure_flip_probability = 0.003;
    auto circuit = stim::generate_surface_code_circuit(gen).circuit;
    auto dem = stim::ErrorAnalyzer::circuit_to_detector_error_model(circuit,true,true,false,0,false,false);
    StaticGridSpec spec{d,d,rotated,{},{},GridSimd::scalar};
    spec.coordinates.resize(dem.count_detectors());
    std::set<uint64_t> ids;
    for(size_t k=0;k<spec.coordinates.size();++k) ids.insert(k);
    for(const auto& [id,p]:dem.get_detector_coordinates(ids))
        for(size_t k=0;k<3;++k) spec.coordinates[id][k]=int64_t(p[k]);
    return {std::move(dem),std::move(spec)};
}
struct PreparedRegionSnapshot {
    std::vector<uint64_t> values;
    std::vector<structured::RegionGeneration> generations;
};
void expect_clean_nodes(const ComponentDecoder& decoder, bool zero_inactive_timestamps = true) {
    for (const auto& component : decoder.components()) {
        EXPECT_FALSE(component.mwpm.has_pending_seed_cleanup());
        const auto& graph = component.mwpm.flooder.graph;
        for (size_t i = 0; i < graph.num_nodes; ++i) {
            const auto& n = graph.node(i);
            EXPECT_EQ(n.region_that_arrived, structured::NO_REGION);
            EXPECT_EQ(n.top_region(), nullptr);
            EXPECT_EQ(n.reached_from_source, nullptr);
            EXPECT_EQ(n.observables_crossed_from_source, 0U);
            EXPECT_EQ(n.radius_of_arrival, 0);
            EXPECT_EQ(n.grid_component->collision_states[n.grid_cell].wrapped_radius_cached, 0);
            if (zero_inactive_timestamps) {
                EXPECT_EQ(n.node_event_tracker.desired_time.value, 0U);
                EXPECT_EQ(n.node_event_tracker.queued_time.value, 0U);
            }
            EXPECT_FALSE(n.node_event_tracker.has_desired_time);
            EXPECT_FALSE(n.node_event_tracker.has_queued_time);
        }
    }
}
PreparedRegionSnapshot snapshot_imported_regions(const ComponentDecoder& decoder) {
    PreparedRegionSnapshot result;
    auto& v = result.values;
    const auto node_id = [](const structured::DetectorNode* node) { return node ? node->detector_id : SIZE_MAX; };
    const auto tracker = [&](const structured::QueuedEventTracker& t) {
        v.insert(v.end(), {t.desired_time.value, t.queued_time.value, t.has_desired_time, t.has_queued_time});
    };
    const auto edge = [&](const structured::CompressedEdge& e) {
        v.insert(v.end(), {node_id(e.loc_from), node_id(e.loc_to), e.obs_mask});
    };
    for (const auto& component : decoder.components()) {
        const auto& mwpm = component.mwpm;
        const auto& pre = mwpm.predecoder;
        const auto& pool = *mwpm.flooder.regions;
        v.insert(v.end(), {pre.complete, pre.seed_view.active_count()});
        for (const auto& range : pre.seed_view.active_ranges())
        for (const auto& seed : pre.seed_view.storage.subspan(range.begin, range.count())) {
            const auto mate = seed.mate < structured::PredecodeSeed::BOUNDARY ? pre.seed_view.rank_of(seed.mate) : seed.mate;
            const auto active = seed.grid->predecode.active[seed.cell];
            v.insert(v.end(), {seed.detector_id, seed.cell, mate, seed.radius, seed.match_slot,
                              active ? pre.seed_view.rank_of(active - 1) + 1 : 0, seed.grid->predecode.radii[seed.cell]});
#if PM_PREDECODE_CAP_CACHE
            v.push_back(seed.cap_saturated);
#endif
        }
        if (!pre.complete) for (structured::RegionId id = 1; id <= pre.seed_view.active_count(); ++id) {
            const auto& r = pool[id];
            result.generations.push_back(pool.generation(id));
            v.insert(v.end(), {r.region_id, r.blossom_parent, r.blossom_parent_top,
                              uint64_t(r.radius_value().data), r.match.region, r.blossom_children.size(),
                              r.shell_area.size(), r.alt_tree_node != nullptr});
            EXPECT_EQ(r.pool, &pool);
            tracker(r.shrink_event_tracker);
            edge(r.match.edge);
            for (const auto& child : r.blossom_children) {
                v.push_back(child.region);
                edge(child.edge);
            }
            for (const auto* node : r.shell_area)
                v.push_back(node_id(node));
            if (const auto* root = r.alt_tree_node) {
                v.insert(v.end(), {root->inner_region, root->outer_region, root->parent.alt_tree_node != nullptr,
                                  root->children.size(), root->visited});
                edge(root->inner_to_outer_edge);
                edge(root->parent.edge);
            }
        }
        // Include non-seed nodes touched by the preceding full MWPM solve.
        for (size_t id = 0; id < mwpm.flooder.graph.num_nodes; ++id) {
            const auto& n = mwpm.flooder.graph.node(id);
            const auto& collision = n.grid_component->collision_states[n.grid_cell];
            v.insert(v.end(), {n.detector_id, n.grid_cell, n.region_that_arrived, node_id(n.reached_from_source),
                              n.observables_crossed_from_source, uint64_t(n.radius_of_arrival),
                              collision.top_region_id, uint64_t(collision.wrapped_radius_cached)});
            tracker(n.node_event_tracker);
        }
        v.insert(v.end(), {uint64_t(mwpm.flooder.queue.cur_time), mwpm.flooder.queue.size()});
        for (const auto& bucket : mwpm.flooder.queue.bit_buckets) {
            v.push_back(bucket.size());
            for (const auto& event : bucket) {
                EXPECT_EQ(event.tentative_event_type, structured::LOOK_AT_NODE);
                v.insert(v.end(), {event.tentative_event_type, event.time.value,
                                  event.tentative_event_type == structured::LOOK_AT_NODE
                                      ? node_id(event.data_look_at_node) : SIZE_MAX});
            }
        }
    }
    return result;
}
}
TEST(SharedComponents, OrderedValidationRetainsErrorPriorityIgnoredIdsAndEpochWrap) {
    auto graph = pair_graph();
    graph.nodes.resize(5);
    graph.set_boundary({4});
    auto spec = pair_spec();
    spec.coordinates.push_back({0, 0, 2});
    ComponentDecoder decoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    using Access = structured::ComponentDecoderTestAccess;
    decoder.prepare_shot({0, 2, 4});
    EXPECT_EQ(Access::epoch(decoder), 1U);
    EXPECT_EQ(Access::stamps(decoder), std::vector<uint32_t>(5, 0));
    decoder.reset_after_shot();
    decoder.prepare_shot({4, 2, 0, 4});
    ASSERT_EQ(decoder.shot_seeds.size(), 2U);
    EXPECT_EQ(decoder.components()[0].global_detectors[decoder.shot_seeds[0].detector_id], 2U);
    EXPECT_EQ(decoder.components()[0].global_detectors[decoder.shot_seeds[1].detector_id], 0U);
    EXPECT_EQ(Access::stamps(decoder)[4], 0U);
    decoder.reset_after_shot();
    try {
        decoder.prepare_shot({2, 0, 2, 99, 98});
        FAIL() << "Expected the first out-of-range ID to win over the duplicate";
    } catch (const std::invalid_argument& e) {
        EXPECT_EQ(std::string(e.what()), "Detection event index 99 is not in the graph.");
    }
    EXPECT_EQ(Access::epoch(decoder), 3U);
    EXPECT_THROW(decoder.prepare_shot({0, 0}), std::invalid_argument);
    EXPECT_EQ(Access::epoch(decoder), 4U);
    EXPECT_NO_THROW(decoder.prepare_shot({4, 4}));
    EXPECT_TRUE(decoder.shot_seeds.empty());
    decoder.reset_after_shot();
    Access::epoch(decoder, UINT32_MAX);
    Access::stamp_all(decoder, 1);
    decoder.prepare_shot({0, 2});
    EXPECT_EQ(Access::epoch(decoder), 1U);
    EXPECT_EQ(Access::stamps(decoder), std::vector<uint32_t>(5, 0));
    decoder.reset_after_shot();
    decoder.prepare_shot({2, 0});
    EXPECT_EQ(Access::epoch(decoder), 2U);
    decoder.reset_after_shot();
    decoder.prepare_shot({});
    EXPECT_EQ(Access::epoch(decoder), 3U);
}

TEST(SharedComponents, CachedBucketSentinelsOrderedValidationAndEpochWrap) {
    using Access = structured::ComponentDecoderTestAccess;
    auto graph = pair_graph();
    graph.nodes.resize(5);
    graph.set_boundary({4});
    auto spec = pair_spec();
    spec.coordinates.push_back({0, 0, 2});
    ComponentDecoder decoder(graph, spec, NUM_DISTINCT_WEIGHTS, true);
    const auto initial = Access::addresses(decoder);
    ASSERT_EQ(initial.size(), 5U);
    EXPECT_EQ(initial[4][0], UINT16_MAX);
    for (const auto& a : initial) EXPECT_EQ(a[1], UINT16_MAX);
    for (size_t c = 0; c < decoder.components().size(); ++c) {
        const auto& component = decoder.components()[c];
        for (size_t local = 0; local < component.global_detectors.size(); ++local) {
            const auto& a = initial[component.global_detectors[local]];
            EXPECT_EQ(a[0], c);
            EXPECT_EQ(a[2], local);
            EXPECT_EQ(a[3], component.mwpm.flooder.graph.node(local).grid_cell);
        }
    }
    Access::bind(decoder, 2, 3);
    const auto bound = Access::addresses(decoder);
    EXPECT_EQ(bound[4][1], UINT16_MAX);
    const auto check_cache = [&] {
        const auto now = Access::addresses(decoder);
        for (size_t k = 0; k < now.size(); ++k)
            for (size_t field = 0; field < 4; ++field) EXPECT_EQ(now[k][field], bound[k][field]);
    };
    std::array<Access::BoundLayout, 2> layouts;
    const auto prepare = [&](const std::vector<uint64_t>& input) {
        Access::stage_bound(decoder, input, layouts);
        EXPECT_TRUE(Access::cached_stage(decoder));
        Access::scatter_bound(decoder, input, layouts, 7U);
        decoder.publish_partitioned_shot();
        EXPECT_FALSE(Access::cached_stage(decoder));
        check_cache();
    };
    prepare({0, 2, 4});
    EXPECT_EQ(Access::epoch(decoder), 1U);
    EXPECT_EQ(Access::stamps(decoder), std::vector<uint32_t>(5, 0));
    decoder.reset_after_shot();
    prepare({4, 2, 0, 4});
    EXPECT_EQ(decoder.shot_seeds[0].detector_id, 1U);
    EXPECT_EQ(decoder.shot_seeds[1].detector_id, 0U);
    decoder.reset_after_shot();
    try {
        Access::stage_bound(decoder, {2, 0, 2, 99, 98}, layouts);
        FAIL() << "Expected first range error before duplicate rejection";
    } catch (const std::invalid_argument& e) {
        EXPECT_EQ(std::string(e.what()), "Detection event index 99 is not in the graph.");
    }
    EXPECT_FALSE(Access::cached_stage(decoder));
    EXPECT_THROW(Access::stage_bound(decoder, {0, 0}, layouts), std::invalid_argument);
    EXPECT_EQ(Access::epoch(decoder), 4U);
    prepare({4, 4});
    EXPECT_TRUE(decoder.shot_seeds.empty());
    decoder.reset_after_shot();
    Access::epoch(decoder, UINT32_MAX);
    Access::stamp_all(decoder, 1);
    prepare({0, 2});
    EXPECT_EQ(Access::epoch(decoder), 1U);
    EXPECT_EQ(Access::stamps(decoder), std::vector<uint32_t>(5, 0));
    decoder.reset_after_shot();
    EXPECT_EQ(decoder.decode({0, 1, 2, 3}).weight, 10);
    decoder.reset_after_shot();
    check_cache();
    std::vector<size_t> explicit_boundary, ignored_boundary;
    decoder.get_nodes_on_shortest_path_from_source(0, SIZE_MAX, explicit_boundary);
    decoder.get_nodes_on_shortest_path_from_source(0, 4, ignored_boundary);
    EXPECT_EQ(explicit_boundary, ignored_boundary);
    explicit_boundary.clear(); ignored_boundary.clear();
    decoder.get_nodes_on_shortest_path_from_source(SIZE_MAX, 0, explicit_boundary);
    decoder.get_nodes_on_shortest_path_from_source(4, 0, ignored_boundary);
    EXPECT_EQ(explicit_boundary, ignored_boundary);
    EXPECT_THROW(decoder.get_nodes_on_shortest_path_from_source(0, 1, ignored_boundary), std::invalid_argument);
}

TEST(SharedComponents, GroupStaticSpansPreserveFullScanOrderStateAndIgnoredEvents) {
    using Access = structured::ComponentDecoderTestAccess;
    using Geometry = structured::TimePartitionCuts::Geometry;
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const size_t ignored = graph.nodes.size();
    graph.nodes.resize(ignored + 1);
    graph.set_boundary({ignored});
    spec.coordinates.push_back({0, 0, 2});
    ComponentDecoder full(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder bounded(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<std::vector<uint64_t>> shots(6);
    for (size_t g = 0; g < ignored; ++g) if (g % 13 == 1) shots[0].push_back(g);
    shots[1] = shots[0]; std::reverse(shots[1].begin(), shots[1].end());
    shots[2] = shots[0]; std::mt19937 rng(20261012); std::shuffle(shots[2].begin(), shots[2].end(), rng);
    for (auto g : full.components()[0].global_detectors) if (g % 7 == 2) shots[3].push_back(g);
    for (const auto& component : full.components())
        for (auto g : component.global_detectors) if (g % 11 == 3) shots[4].push_back(g);
    // Sorted input exercises binary search; ignored IDs can occur outside its
    // envelope. Repeated ignored IDs remain legal and force the full-scan path.
    shots.push_back(shots[0]); shots.back().push_back(ignored);
    shots.push_back(shots[0]);
    shots.back().insert(shots.back().begin(), ignored);
    shots.back().insert(shots.back().begin() + shots.back().size() / 2, ignored);
    shots.back().push_back(ignored);
    shots.push_back({ignored, ignored}); shots.push_back({ignored});
    const auto finish = [](ComponentDecoder& d, const auto& layouts, size_t parts) {
        for (size_t c = 0; c < d.components().size(); ++c) {
            auto& mwpm = d.components()[c].mwpm;
            auto& pre = mwpm.predecoder;
            if (!Access::is_fixed(d)) pre.prepare_partition_scratch(std::span<const size_t>(layouts[c].offsets).first(parts + 2));
            std::array<structured::OneHopPredecoder::PartialResult, 9> partial{};
            for (size_t b = 0; b < parts; ++b) {
                const auto order = Access::is_fixed(d)
                    ? pre.prepare_partition_order(b, pre.seed_view.active_ranges()[b].count(), partial[b])
                    : pre.prepare_partition_order(b, partial[b]);
                pre.run_order(order, partial[b]);
            }
            for (size_t b = 0; b < parts; ++b) pre.merge(partial[b]);
            const auto order = Access::is_fixed(d)
                ? pre.prepare_partition_order(parts, pre.seed_view.active_ranges()[parts].count(), partial[parts])
                : pre.prepare_partition_order(parts, partial[parts]);
            pre.run_order(order, partial[parts]); pre.merge(partial[parts]); pre.finish();
            if (!pre.complete) {
                if (Access::is_fixed(d)) {
                    const auto first = mwpm.prepare_predecode_import(pre.seed_view.active_count());
                    if (parts == 2) mwpm.import_fixed_predecode_range<2>(pre.seed_view, first, 0, pre.seed_view.active_count());
                    else mwpm.import_fixed_predecode_range<4>(pre.seed_view, first, 0, pre.seed_view.active_count());
                    mwpm.finish_fixed_predecode_import(pre.seed_view, first);
                } else mwpm.import_predecode_state(pre.seed_view.storage);
            }
        }
    };
    for (auto geometry : {Geometry::symmetric_width, Geometry::three_planes}) for (size_t parts : {2, 4}) {
        const size_t width = geometry == Geometry::three_planes ? 0 : 3;
        full = ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        bounded = ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        Access::bind(full, parts, width, geometry); Access::bind(bounded, parts, width, geometry);
        Access::fixed(bounded);
        const auto addresses = Access::addresses(bounded);
        for (size_t c = 0; c < 2; ++c) for (size_t b = 0; b < 5; ++b) {
            uint64_t min = UINT64_MAX, max = 0;
            for (size_t g = 0; g < addresses.size(); ++g) if (addresses[g][0] == c && addresses[g][1] == b) {
                min = std::min(min, uint64_t(g)); max = std::max(max, uint64_t(g));
            }
            EXPECT_EQ(Access::ranges(bounded)[c][b].min, min);
            EXPECT_EQ(Access::ranges(bounded)[c][b].max, max);
        }
        for (size_t lanes : {1, 2}) for (const auto& shot : shots) {
            SCOPED_TRACE(::testing::Message() << "G=" << int(geometry) << " P=" << parts << " lanes=" << lanes);
            std::array<Access::BoundLayout, 2> old_layouts, layouts;
            Access::stage_bound(full, shot, old_layouts); Access::stage_group(bounded, shot, layouts);
            for (size_t c = 0; c < 2; ++c) {
                EXPECT_EQ(layouts[c].offsets[parts + 1], bounded.components()[c].global_detectors.size());
                const bool ordered = std::adjacent_find(shot.begin(), shot.end(), std::greater_equal<uint64_t>{}) == shot.end();
                EXPECT_EQ(layouts[c].input_ordered, ordered);
                // Exhaust every owner mask, including empty/noncontiguous sets.
                for (uint32_t mask = 0; mask < (uint32_t{1} << (parts + 1)); ++mask) {
                    const auto select = [&](std::span<const uint64_t> input) {
                        std::vector<uint64_t> result;
                        for (auto g : input) if (addresses[g][0] == c && (mask & (1U << addresses[g][1])))
                            result.push_back(g);
                        return result;
                    };
                    const auto span = Access::bounded_input(bounded, shot, layouts[c], c, mask);
                    EXPECT_EQ(select(shot), select(span));
                    if (!mask) EXPECT_TRUE(span.empty());
                    else if (!ordered && !span.empty()) EXPECT_EQ(span.size(), shot.size());
                }
                for (size_t lane = 0; lane < lanes; ++lane) {
                    std::array<uint32_t, 2> masks{};
                    for (size_t b = lane; b < parts; b += lanes) masks[c] |= 1U << b;
                    if (!lane) masks[c] |= 1U << parts;
                    Access::scatter_bound(full, shot, old_layouts, masks);
                    Access::scatter_bound(bounded, Access::bounded_input(bounded, shot, layouts[c], c, masks[c]), layouts, masks);
                }
            }
            full.publish_partitioned_shot(); bounded.publish_partitioned_shot();
            finish(full, old_layouts, parts); finish(bounded, layouts, parts);
            const auto expected = snapshot_imported_regions(full), actual = snapshot_imported_regions(bounded);
            EXPECT_EQ(expected.values, actual.values); EXPECT_EQ(expected.generations, actual.generations);
            MatchingResult a, b;
            for (size_t c = 0; c < 2; ++c) {
                a += structured::solve_prepared_detection_events_deferring_seed_nodes(full.components()[c].mwpm);
                b += structured::solve_prepared_detection_events_deferring_seed_nodes(bounded.components()[c].mwpm);
            }
            EXPECT_EQ(a, b);
            full.reset(); bounded.reset();
        }
    }
    std::array<Access::BoundLayout, 2> layouts;
    try {
        Access::stage_group(bounded, {ignored, 0, 0, UINT64_MAX}, layouts);
        FAIL() << "Full range validation must precede duplicate rejection";
    } catch (const std::invalid_argument& e) {
        EXPECT_EQ(std::string(e.what()), "Detection event index " + std::to_string(UINT64_MAX) + " is not in the graph.");
    }
    EXPECT_FALSE(Access::staged(bounded));
    EXPECT_THROW(Access::stage_group(bounded, {ignored, 0, 0}, layouts), std::invalid_argument);
    EXPECT_FALSE(Access::staged(bounded));
    Access::stage_group(bounded, {}, layouts);
    for (size_t c = 0; c < 2; ++c) {
        EXPECT_TRUE(layouts[c].input_ordered);
        EXPECT_TRUE(Access::bounded_input(bounded, {}, layouts[c], c, 31).empty());
    }
    bounded.publish_partitioned_shot(); bounded.reset();
}

TEST(SharedComponents, GroupStaticSpansSupportArbitraryDetectorNumberingAndMoves) {
    using Access = structured::ComponentDecoderTestAccess;
    using Geometry = structured::TimePartitionCuts::Geometry;
    BackendUserGraph graph(57, 2);
    graph.set_boundary({0, 28, 56});
    StaticGridSpec spec{9, 27, false, {}, {}, GridSimd::scalar};
    spec.coordinates.resize(57); spec.groups.resize(57);
    std::vector<size_t> mapped;
    for (size_t g = 0; g < 57; ++g) if (!graph.is_boundary_node(g)) mapped.push_back(g);
    // This permutation mixes both component labels and all time buckets. ID
    // envelopes may overlap heavily, yet retaining the original filter is exact.
    const auto id = [&](size_t t, size_t c) { return mapped[(17 * (2 * t + c) + 11) % mapped.size()]; };
    for (size_t t = 0; t < 27; ++t) for (size_t c = 0; c < 2; ++c) {
        spec.coordinates[id(t, c)] = {0, 0, int64_t(t)};
        spec.groups[id(t, c)] = c;
        if (t) graph.add_or_merge_edge(id(t - 1, c), id(t, c), {}, 2, -1);
        if (t == 0 || t == 26) graph.add_or_merge_boundary_edge(id(t, c), {c}, 4, -1);
    }
    ComponentDecoder original(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    Access::bind(original, 2, 0, Geometry::three_planes);
    const auto before_move = Access::ranges(original);
    ComponentDecoder moved(std::move(original));
    ComponentDecoder bounded(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    bounded = std::move(moved);
    for (size_t c = 0; c < 2; ++c) for (size_t b = 0; b < 5; ++b) {
        EXPECT_EQ(Access::ranges(bounded)[c][b].min, before_move[c][b].min);
        EXPECT_EQ(Access::ranges(bounded)[c][b].max, before_move[c][b].max);
    }
    ComponentDecoder full(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<std::vector<uint64_t>> shots(7);
    for (size_t g = 0; g < 57; ++g) shots[0].push_back(g);
    shots[1] = shots[0]; std::reverse(shots[1].begin(), shots[1].end());
    shots[2] = shots[0]; std::mt19937 rng(15); std::shuffle(shots[2].begin(), shots[2].end(), rng);
    for (size_t t : {0, 6, 12, 13, 14, 20, 26}) shots[3].push_back(id(t, 0));
    std::sort(shots[3].begin(), shots[3].end());
    shots[4] = {0, 0, 28, 56}; shots[5] = {id(0, 1)}; // Shot 6 is empty.
    for (auto geometry : {Geometry::symmetric_width, Geometry::three_planes}) for (size_t parts : {2, 4}) {
        const size_t width = geometry == Geometry::three_planes ? 0 : 3;
        full = ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        bounded = ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        Access::bind(full, parts, width, geometry); Access::bind(bounded, parts, width, geometry);
        Access::fixed(bounded);
        const auto addresses = Access::addresses(bounded);
        for (size_t c = 0; c < 2; ++c) for (size_t b = 0; b < 5; ++b) {
            uint64_t min = UINT64_MAX, max = 0;
            for (size_t g = 0; g < addresses.size(); ++g) if (addresses[g][0] == c && addresses[g][1] == b) {
                min = std::min(min, uint64_t(g)); max = std::max(max, uint64_t(g));
            }
            EXPECT_EQ(Access::ranges(bounded)[c][b].min, min);
            EXPECT_EQ(Access::ranges(bounded)[c][b].max, max);
        }
        for (const auto& shot : shots) {
            std::array<Access::BoundLayout, 2> layouts, full_layouts;
            Access::stage_group(bounded, shot, layouts); Access::stage_bound(full, shot, full_layouts);
            for (size_t c = 0; c < 2; ++c) {
                EXPECT_EQ(layouts[c].offsets[parts + 1], bounded.components()[c].global_detectors.size());
                const auto select = [&](std::span<const uint64_t> input, uint32_t mask) {
                    std::vector<uint64_t> selected;
                    for (auto g : input) if (addresses[g][0] == c && (mask & (1U << addresses[g][1]))) selected.push_back(g);
                    return selected;
                };
                for (uint32_t mask = 0; mask < (1U << (parts + 1)); ++mask) {
                    const auto span = Access::bounded_input(bounded, shot, layouts[c], c, mask);
                    EXPECT_EQ(select(shot, mask), select(span, mask));
                    if (!mask) EXPECT_TRUE(span.empty());
                    else if (!layouts[c].input_ordered && !span.empty()) EXPECT_EQ(span.size(), shot.size());
                }
                // Scatter each bucket independently, exercising exact cursors
                // at both inclusive static endpoints and across ignored IDs.
                for (size_t b = 0; b <= parts; ++b) {
                    std::array<uint32_t, 2> masks{}; masks[c] = 1U << b;
                    Access::scatter_bound(full, shot, full_layouts, masks);
                    Access::scatter_bound(bounded, Access::bounded_input(bounded, shot, layouts[c], c, masks[c]), layouts, masks);
                }
            }
            full.publish_partitioned_shot(); bounded.publish_partitioned_shot();
            for (size_t c = 0; c < 2; ++c) {
                const auto& a = full.components()[c].mwpm.predecoder.seed_view;
                const auto& b = bounded.components()[c].mwpm.predecoder.seed_view;
                ASSERT_EQ(a.active_count(), b.active_count());
                for (const auto& range : b.active_ranges()) for (size_t slot = range.begin; slot < range.end; ++slot) {
                    const auto rank = range.rank_begin + slot - range.begin;
                    EXPECT_EQ(a.storage[rank].detector_id, b.storage[slot].detector_id);
                    EXPECT_EQ(a.storage[rank].cell, b.storage[slot].cell);
                    EXPECT_EQ(b.storage[slot].mate, structured::PredecodeSeed::UNMATCHED);
                    EXPECT_EQ(b.storage[slot].radius, 0U);
                    EXPECT_EQ(b.storage[slot].grid->predecode.active[b.storage[slot].cell], slot + 1);
                }
            }
            full.reset(); bounded.reset();
        }
    }
    // The helper's inclusive upper endpoint does not need max+1 arithmetic.
    auto ranges = Access::ranges(bounded)[0]; ranges[0].min = 0; ranges[0].max = UINT64_MAX;
    Access::BoundLayout layout; layout.input_ordered = true; layout.offsets[1] = 2;
    const std::array<uint64_t, 2> endpoints{0, UINT64_MAX};
    EXPECT_EQ(layout.bounded_input(endpoints, 1, ranges).size(), endpoints.size());
}

TEST(SharedComponents, CachedBucketMatchesDynamicLayoutsAndKeepsRebindingTransactional) {
    using Access = structured::ComponentDecoderTestAccess;
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder bound(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder dynamic(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<uint64_t> events;
    for (size_t k = 0; k < spec.coordinates.size(); ++k) if (k % 7 == 1) events.push_back(k);
    std::reverse(events.begin(), events.end());
    const auto finish = [](ComponentDecoder& d, const auto& layouts, size_t parts) {
        for (size_t c = 0; c < d.components().size(); ++c) {
            auto& mwpm = d.components()[c].mwpm;
            auto& pre = mwpm.predecoder;
            pre.prepare_partition_scratch(std::span<const size_t>(layouts[c].offsets).first(parts + 2));
            std::array<structured::OneHopPredecoder::PartialResult, 9> partial{};
            for (size_t b = 0; b < parts; ++b) {
                const auto order = pre.prepare_partition_order(b, partial[b]);
                pre.run_order(order, partial[b]);
            }
            for (size_t b = 0; b < parts; ++b) pre.merge(partial[b]);
            const auto order = pre.prepare_partition_order(parts, partial[parts]);
            pre.run_order(order, partial[parts]);
            pre.merge(partial[parts]);
            pre.finish();
            if (!pre.complete) mwpm.import_predecode_state(pre.seed_view.storage);
        }
    };
    for (size_t parts : {2, 4}) for (size_t width : {2, 3}) {
        SCOPED_TRACE(::testing::Message() << "P" << parts << " W" << width);
        Access::bind(bound, parts, width);
        const auto saved = Access::addresses(bound);
        const auto saved_ranges = Access::ranges(bound);
        const auto saved_epoch = Access::epoch(bound);
        EXPECT_THROW(Access::bind(bound, 3, width), std::invalid_argument);
        EXPECT_THROW(Access::bind(bound, parts, 1), std::invalid_argument);
        // A failure in the second component geometry must not partially rewrite C0.
        auto& second_grid = *bound.components()[1].seed_grid;
        const auto nt = second_grid.nt;
        second_grid.nt = 1;
        EXPECT_THROW(Access::bind(bound, parts == 2 ? 4 : 2, width), std::invalid_argument);
        second_grid.nt = nt;
        EXPECT_EQ(Access::addresses(bound), saved);
        for (size_t c = 0; c < 2; ++c) for (size_t b = 0; b < 5; ++b) {
            EXPECT_EQ(Access::ranges(bound)[c][b].min, saved_ranges[c][b].min);
            EXPECT_EQ(Access::ranges(bound)[c][b].max, saved_ranges[c][b].max);
        }
        EXPECT_EQ(Access::epoch(bound), saved_epoch);
        EXPECT_EQ(Access::binding(bound), std::make_pair(parts, width));
        std::array<structured::TimePartitionCuts, 2> cuts;
        std::array<ComponentDecoder::PartitionSeedLayout, 2> old_layouts;
        std::array<Access::BoundLayout, 2> new_layouts;
        for (size_t c = 0; c < 2; ++c) {
            const auto& grid = *bound.components()[c].seed_grid;
            cuts[c] = structured::TimePartitionCuts(grid.nx * grid.ny, grid.nt, parts, width);
            old_layouts[c].cuts = &cuts[c];
            for (auto global : bound.components()[c].global_detectors)
                EXPECT_EQ(saved[global][1], cuts[c].bucket(saved[global][3]));
        }
        for (const auto& shot : std::vector<std::vector<uint64_t>>{events, {}, events}) {
            Access::stage_bound(bound, shot, new_layouts);
            dynamic.stage_partitioned_shot(shot, old_layouts, parts);
            const auto stage_saved = Access::addresses(bound);
            EXPECT_THROW(Access::bind(bound, parts == 2 ? 4 : 2, width), std::logic_error);
            EXPECT_EQ(Access::addresses(bound), stage_saved);
            for (size_t c = 0; c < 2; ++c) EXPECT_EQ(new_layouts[c].offsets, old_layouts[c].offsets);
            // Distinct first component masks followed by their complementary scans.
            const uint32_t all = (uint32_t{1} << (parts + 1)) - 1;
            const std::array<uint32_t, 2> first_masks{1U, 2U};
            Access::scatter_bound(bound, shot, new_layouts, first_masks);
            dynamic.scatter_partitioned_shot(shot, old_layouts, first_masks);
            const std::array<uint32_t, 2> rest_masks{all ^ 1U, all ^ 2U};
            Access::scatter_bound(bound, shot, new_layouts, rest_masks);
            dynamic.scatter_partitioned_shot(shot, old_layouts, rest_masks);
            bound.publish_partitioned_shot(); dynamic.publish_partitioned_shot();
            EXPECT_FALSE(Access::cached_stage(bound));
            finish(bound, new_layouts, parts); finish(dynamic, old_layouts, parts);
            const auto prepared = snapshot_imported_regions(bound).values;
            EXPECT_EQ(prepared, snapshot_imported_regions(dynamic).values);
            // Published state no longer depends on cached input classification.
            // A cold internal rebind must preserve its queue, result and epoch.
            const auto prepared_epoch = Access::epoch(bound);
            Access::bind(bound, parts == 2 ? 4 : 2, width);
            EXPECT_EQ(snapshot_imported_regions(bound).values, prepared);
            EXPECT_EQ(Access::epoch(bound), prepared_epoch);
            Access::bind(bound, parts, width);
            MatchingResult bound_result, dynamic_result;
            for (size_t c = 0; c < 2; ++c) {
                bound_result += structured::solve_prepared_detection_events_deferring_seed_nodes(bound.components()[c].mwpm);
                dynamic_result += structured::solve_prepared_detection_events_deferring_seed_nodes(dynamic.components()[c].mwpm);
            }
            EXPECT_EQ(bound_result, dynamic_result);
            bound.reset(); dynamic.reset();
            // Public dynamic staging on the cached decoder leaves its binding intact.
            const auto cache_before_dynamic = Access::addresses(bound);
            bound.stage_partitioned_shot(shot, old_layouts, parts);
            bound.scatter_partitioned_shot(shot, old_layouts, all);
            bound.publish_partitioned_shot(); bound.reset_after_shot();
            const auto cache_after_dynamic = Access::addresses(bound);
            for (size_t k = 0; k < cache_before_dynamic.size(); ++k)
                for (size_t f = 0; f < 4; ++f) EXPECT_EQ(cache_before_dynamic[k][f], cache_after_dynamic[k][f]);
        }
    }
    // The public dynamic API may use a geometry different from the private
    // cache without reading or rebinding it. Verify the actual resulting order.
    const auto cached = Access::addresses(bound);
    std::array<structured::TimePartitionCuts, 2> different_cuts;
    std::array<ComponentDecoder::PartitionSeedLayout, 2> different_layouts;
    for (size_t c = 0; c < 2; ++c) {
        const auto& grid = *bound.components()[c].seed_grid;
        different_cuts[c] = structured::TimePartitionCuts(grid.nx * grid.ny, grid.nt, 2, 2);
        different_layouts[c].cuts = &different_cuts[c];
    }
    bound.stage_partitioned_shot(events, different_layouts, 2);
    bound.scatter_partitioned_shot(events, different_layouts, 7U);
    bound.publish_partitioned_shot();
    for (size_t c = 0; c < 2; ++c) {
        std::vector<uint64_t> expected;
        for (size_t b = 0; b <= 2; ++b) for (auto global : events)
            if (cached[global][0] == c && different_cuts[c].bucket(cached[global][3]) == b)
                expected.push_back(global);
        const auto& component = bound.components()[c];
        ASSERT_EQ(component.mwpm.predecoder.seed_view.active_count(), expected.size());
        for (size_t k = 0; k < expected.size(); ++k)
            EXPECT_EQ(component.global_detectors[component.mwpm.predecoder.seed_view.storage[k].detector_id], expected[k]);
    }
    bound.reset_after_shot();
    const auto after_dynamic = Access::addresses(bound);
    for (size_t k = 0; k < cached.size(); ++k)
        for (size_t f = 0; f < 4; ++f) EXPECT_EQ(cached[k][f], after_dynamic[k][f]);
    std::array<Access::BoundLayout, 2> layouts;
    ComponentDecoder moved(std::move(bound));
    EXPECT_THROW(Access::stage_bound(bound, {}, std::span(layouts).first(0)), std::logic_error);
    Access::stage_bound(moved, events, layouts);
    Access::scatter_bound(moved, events, layouts, 31U);
    moved.publish_partitioned_shot(); moved.reset_after_shot();
    EXPECT_EQ(Access::binding(moved), std::make_pair(size_t{4}, size_t{3}));
}

TEST(SharedComponents, RetainedSeedStorageResetsDefaultsAndCancelsInactiveStaging) {
    auto graph = pair_graph();
    ComponentDecoder decoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
    using Access = structured::ComponentDecoderTestAccess;
    decoder.prepare_shot({0, 1, 2, 3});
    const auto* backing = decoder.shot_seeds.data();
    for (auto& seed : decoder.shot_seeds) {
        seed.mate = 0;
        seed.radius = 42;
        seed.match_slot = 7;
#if PM_PREDECODE_CAP_CACHE
        seed.cap_saturated = true;
#endif
#if PM_STRUCTURED_PROFILE_COUNTERS
        seed.profile_finite_grow = true;
#endif
    }
    decoder.reset_after_shot();
    EXPECT_TRUE(decoder.shot_seeds.empty());
    EXPECT_EQ(decoder.shot_seeds.capacity(), 0U);
    EXPECT_EQ(Access::retired(decoder).data(), backing);
    decoder.reset_after_shot();
    EXPECT_EQ(Access::retired(decoder).data(), backing);
    EXPECT_EQ(Access::retired(decoder).front().radius, 42);
    ComponentDecoder moved(std::move(decoder));
    for (const auto& input : std::vector<std::vector<uint64_t>>{{0, 1, 2, 3}, {0}, {}, {0, 1, 2, 3}}) {
        moved.prepare_shot(input);
        EXPECT_EQ(moved.shot_seeds.data(), backing);
        for (const auto& seed : moved.shot_seeds) {
            EXPECT_EQ(seed.mate, structured::PredecodeSeed::UNMATCHED);
            EXPECT_EQ(seed.radius, 0);
            EXPECT_EQ(seed.match_slot, 0);
#if PM_PREDECODE_CAP_CACHE
            EXPECT_FALSE(seed.cap_saturated);
#endif
#if PM_STRUCTURED_PROFILE_COUNTERS
            EXPECT_FALSE(seed.profile_finite_grow);
#endif
        }
        moved.reset_after_shot();
    }
    std::array<structured::TimePartitionCuts, 2> cuts;
    std::array<ComponentDecoder::PartitionSeedLayout, 2> layouts;
    for (size_t c = 0; c < 2; ++c) {
        const auto& grid = *moved.components()[c].seed_grid;
        cuts[c] = structured::TimePartitionCuts(grid.nx * grid.ny, grid.nt, 2, 3);
        layouts[c].cuts = &cuts[c];
    }
    moved.stage_partitioned_shot({0, 1, 2, 3}, layouts, 2);
    ASSERT_TRUE(Access::staged(moved));
    for (auto& component : moved.components()) EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
    // Model an allocation/preparation failure before any owner was dispatched.
    // These stale records must not be inspected by either reset path.
    for (auto& seed : moved.shot_seeds) seed.grid = nullptr;
    EXPECT_NO_THROW(moved.reset());
    EXPECT_TRUE(moved.shot_seeds.empty());
    EXPECT_EQ(Access::retired(moved).data(), backing);
    EXPECT_EQ(moved.decode({0, 1, 2, 3}).weight, 10);
    moved.reset_after_shot();
    EXPECT_EQ(moved.decode_partitioned({{0, 1}, {0, 1}}).weight, 10);
    moved.reset_after_shot();
    EXPECT_EQ(Access::retired(moved).data(), backing);
}
#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(SharedComponents, WholePrefixRejectsSpatialParallelImportAndSignedConfigurations) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    const auto construct = [](size_t parts, bool parallel_import, bool negative) {
        auto graph = pair_graph();
        if (negative) graph.add_or_merge_boundary_edge(2, {}, -1, -1);
        return SharedComponentDecoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            {-1, -1}, parts, 100, 0, 3, parallel_import, Schedule::component_owned_whole,
            SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
    };
    EXPECT_THROW(construct(2, false, false), std::invalid_argument);
    EXPECT_THROW(construct(4, false, false), std::invalid_argument);
    EXPECT_THROW(construct(1, true, false), std::invalid_argument);
    EXPECT_THROW(construct(1, false, true), std::invalid_argument);
#if PM_SHARED_COMPONENT_PARALLELISM == 1
    EXPECT_NO_THROW(construct(1, false, false));
#else
    EXPECT_THROW(construct(1, false, false), std::invalid_argument);
#endif
}
TEST(SharedComponents, ComponentTailRejectsIncompatibleSchedules) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    const auto construct = [](size_t parts, size_t threshold, bool parallel_import, Schedule schedule) {
        auto graph = pair_graph();
        return SharedComponentDecoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            {-1, -1}, parts, threshold, 0, 3, parallel_import, schedule);
    };
    EXPECT_THROW(construct(2, 0, false, static_cast<Schedule>(255)), std::invalid_argument);
    EXPECT_THROW(construct(2, 0, true, Schedule::component_owned_tail), std::invalid_argument);
    EXPECT_THROW(construct(1, 0, false, Schedule::component_owned_tail), std::invalid_argument);
    EXPECT_THROW(construct(2, 1, false, Schedule::component_owned_tail), std::invalid_argument);
#if PM_SHARED_COMPONENT_PARALLELISM == 2
    EXPECT_THROW(construct(2, 0, false, Schedule::component_owned_tail), std::invalid_argument);
#else
    EXPECT_NO_THROW(construct(2, 0, false, Schedule::component_owned_tail));
    auto signed_graph = pair_graph();
    signed_graph.add_or_merge_boundary_edge(2, {}, -1, -1);
    EXPECT_THROW((SharedComponentDecoder(ComponentDecoder(signed_graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
        {-1, -1}, 2, 0, 0, 3, false, Schedule::component_owned_tail)), std::invalid_argument);
#endif
}

TEST(SharedComponents, TailOwnerRejectsIncompatiblePlacement) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    using Placement = SharedComponentDecoder::ContinuationPlacement;
    const auto construct = [](Schedule schedule, Placement placement) {
        auto graph = pair_graph();
        return SharedComponentDecoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            {-1, -1}, 2, 0, 0, 3, false, schedule, placement);
    };
    EXPECT_THROW(construct(Schedule::component_sequential, Placement::prefix_tail_owner), std::invalid_argument);
    EXPECT_THROW(construct(Schedule::component_sequential, static_cast<Placement>(255)), std::invalid_argument);
#if PM_SHARED_COMPONENT_PARALLELISM == 2
    EXPECT_THROW(construct(Schedule::component_owned_tail, Placement::prefix_tail_owner), std::invalid_argument);
#else
    EXPECT_NO_THROW(construct(Schedule::component_owned_tail, Placement::prefix_tail_owner));
#endif
}

#if PM_SHARED_COMPONENT_PARALLELISM == 1
#if PM_STRUCTURED_PROFILE_TIMELINE
namespace {
void expect_owner_publication_timeline(const SharedComponentDecoder& decoder) {
    const bool component_owned = decoder.profile.interior_interval.present;
    const auto& captain = decoder.profile.lanes[0].owner_scatter_interval;
    EXPECT_TRUE(captain.present);
    EXPECT_FALSE(decoder.serial_decoder().profile.seed_scatter_interval.present);
    EXPECT_FALSE(decoder.profile.lanes[0].owner_conflict_wait_interval.present);
    for (size_t lane = 0; lane < decoder.active_predecode_threads(0); ++lane) {
        const auto& scatter = decoder.profile.lanes[lane].owner_scatter_interval;
        const auto& worker = decoder.profile.component[0].lanes[lane].worker_interval;
        EXPECT_TRUE(scatter.present);
        EXPECT_FALSE(scatter.repeated);
        EXPECT_LE(scatter.start_ns, scatter.end_ns);
        EXPECT_LE(scatter.end_ns, worker.start_ns);
        EXPECT_LE(captain.end_ns, worker.start_ns);
        if (component_owned) {
            EXPECT_FALSE(decoder.profile.lanes[lane].owner_conflict_wait_interval.present);
            for (size_t c = 0; c < decoder.serial_decoder().components().size(); ++c) {
                const auto& p = decoder.profile.component[c];
                const size_t owner = decoder.team_size() == 1 ? 0 : c;
                EXPECT_EQ(p.conflict_owner_lane, owner);
                const auto& published = decoder.profile.lanes[owner].owner_scatter_interval;
                const auto& component_worker = p.lanes[lane].worker_interval;
                EXPECT_LE(published.end_ns, component_worker.start_ns);
                const auto& wait = p.lanes[lane].owner_conflict_wait_interval;
                EXPECT_EQ(wait.present, lane != owner);
                if (wait.present) {
                    EXPECT_FALSE(wait.repeated);
                    EXPECT_LE(scatter.end_ns, wait.start_ns);
                    EXPECT_LE(wait.start_ns, wait.end_ns);
                    EXPECT_LE(published.end_ns, wait.end_ns);
                    EXPECT_LE(wait.end_ns, component_worker.start_ns);
                    if (c) EXPECT_LE(decoder.profile.component[c - 1].lanes[lane].worker_interval.end_ns, wait.start_ns);
                }
            }
        } else if (lane) {
            const auto& wait = decoder.profile.lanes[lane].owner_conflict_wait_interval;
            EXPECT_TRUE(wait.present);
            EXPECT_FALSE(wait.repeated);
            EXPECT_LE(scatter.end_ns, wait.start_ns);
            EXPECT_LE(wait.start_ns, wait.end_ns);
            EXPECT_LE(captain.end_ns, wait.end_ns);
            EXPECT_LE(wait.end_ns, worker.start_ns);
        }
    }
}
void expect_continuation_timeline(const SharedComponentDecoder& decoder, bool tail_owner) {
    const auto& profile = decoder.profile;
    const auto count = decoder.serial_decoder().components().size();
    const bool dispatched = tail_owner && count == 2 && decoder.team_size() > 1 &&
        decoder.component_route(0) != structured::SharedRoute::whole_serial;
    EXPECT_EQ(profile.continuation_dispatch_interval.present, dispatched);
    EXPECT_EQ(profile.continuation_join_interval.present, dispatched);
    for (size_t c = 0; c < count; ++c) {
        const auto& p = profile.component[c];
        EXPECT_EQ(p.continuation_worker, dispatched ? c : 0U);
        ASSERT_TRUE(p.continuation_interval.present);
        EXPECT_FALSE(p.continuation_interval.repeated);
        EXPECT_LE(profile.prefix_interval.end_ns, p.continuation_interval.start_ns);
        if (c) EXPECT_LE(profile.component[c - 1].continuation_interval.end_ns, p.continuation_interval.start_ns);
        if (dispatched && c == 1) {
            EXPECT_LE(profile.component[0].continuation_interval.end_ns, profile.continuation_dispatch_interval.start_ns);
            EXPECT_LE(profile.prefix_interval.end_ns, profile.continuation_dispatch_interval.start_ns);
            EXPECT_LE(profile.prefix_interval.end_ns, profile.continuation_join_interval.start_ns);
            EXPECT_LE(profile.continuation_dispatch_interval.start_ns, p.continuation_interval.start_ns);
            EXPECT_LE(p.continuation_interval.end_ns, profile.continuation_join_interval.end_ns);
        }
    }
}
void expect_component_tail_timeline(const SharedComponentDecoder& decoder) {
    const auto& profile = decoder.profile;
    ASSERT_TRUE(profile.interior_interval.present);
    EXPECT_FALSE(profile.interior_interval.repeated);
    EXPECT_TRUE(profile.interior_dispatch_interval.present);
    EXPECT_TRUE(profile.interior_join_interval.present);
    EXPECT_LE(profile.interior_join_interval.end_ns, profile.interior_interval.end_ns);
    for (size_t c = 0; c < decoder.serial_decoder().components().size(); ++c) {
        const auto& p = profile.component[c];
        const auto expected_worker = decoder.team_size() == 1 ? 0 : c;
        EXPECT_EQ(p.tail_worker, expected_worker);
        EXPECT_EQ(p.conflict_owner_lane, expected_worker);
        ASSERT_TRUE(p.tail_interval.present);
        EXPECT_FALSE(p.tail_interval.repeated);
        EXPECT_FALSE(p.total_interval.present);
        EXPECT_FALSE(p.predecode_interval.present);
        EXPECT_FALSE(p.import_dispatch_interval.present);
        EXPECT_FALSE(p.import_join_interval.present);
        EXPECT_LE(profile.interior_interval.end_ns, p.tail_interval.start_ns);
        EXPECT_LE(p.tail_interval.end_ns, profile.prefix_interval.end_ns);
        EXPECT_LE(p.tail_interval.start_ns, p.partial_merge_interval.start_ns);
        EXPECT_LE(p.partial_merge_interval.end_ns, p.conflict_interval.start_ns);
        EXPECT_LE(p.conflict_interval.end_ns, p.tail_interval.end_ns);
        if (p.import_interval.present) {
            EXPECT_LE(p.conflict_interval.end_ns, p.import_interval.start_ns);
            EXPECT_LE(p.import_interval.end_ns, p.tail_interval.end_ns);
        }
        for (size_t lane = 0; lane < decoder.active_predecode_threads(c); ++lane) {
            const auto& worker = p.lanes[lane].worker_interval;
            EXPECT_TRUE(worker.present);
            EXPECT_LE(profile.interior_interval.start_ns, worker.start_ns);
            EXPECT_LE(worker.end_ns, profile.interior_join_interval.end_ns);
            if (c) EXPECT_LE(profile.component[c - 1].lanes[lane].worker_interval.end_ns, worker.start_ns);
            EXPECT_FALSE(p.lanes[lane].import_worker_interval.present);
        }
    }
    EXPECT_EQ(profile.tail_dispatch_interval.present, decoder.team_size() > 1);
    EXPECT_EQ(profile.tail_join_interval.present, decoder.team_size() > 1);

}
}
#endif
namespace {
PreparedRegionSnapshot snapshot_whole_prefix(const ComponentDecoder& decoder) {
    auto state = snapshot_imported_regions(decoder);
    for (const auto& c : decoder.components()) {
        const auto& pre = c.mwpm.predecoder;
        state.values.insert(state.values.end(), {pre.result.obs_mask, uint64_t(pre.result.weight),
            pre.result_pending, pre.has_live_cell_state(), c.seed_begin, c.seed_end});
    }
    return state;
}
#if PM_STRUCTURED_PROFILE_TIMELINE
void expect_whole_prefix_timeline(const SharedComponentDecoder& decoder) {
    const auto& p = decoder.profile;
    const bool dispatched = decoder.team_size() > 1 && decoder.serial_decoder().components().size() == 2;
    EXPECT_EQ(p.whole_prefix_dispatch_interval.present, dispatched);
    EXPECT_EQ(p.whole_prefix_join_interval.present, dispatched);
    EXPECT_FALSE(p.interior_interval.present);
    EXPECT_FALSE(p.tail_dispatch_interval.present);
    EXPECT_FALSE(decoder.serial_decoder().profile.seed_scatter_interval.present);
    for (const auto& lane : p.lanes) EXPECT_FALSE(lane.owner_scatter_interval.present);
    for (size_t c = 0; c < decoder.serial_decoder().components().size(); ++c) {
        const auto& cp = p.component[c];
        EXPECT_EQ(cp.prefix_worker, dispatched ? c : 0U);
        EXPECT_EQ(cp.active_threads, 1U);
        EXPECT_EQ(cp.partitions, 1U);
        EXPECT_FALSE(cp.tail_interval.present);
        EXPECT_FALSE(cp.bucket_count_interval.present);
        EXPECT_FALSE(cp.bucket_reorder_interval.present);
        EXPECT_FALSE(cp.prepare_interval.present);
        EXPECT_FALSE(cp.import_dispatch_interval.present);
        ASSERT_TRUE(cp.whole_prefix_interval.present);
        ASSERT_TRUE(cp.whole_seed_scatter_interval.present);
        EXPECT_FALSE(cp.whole_prefix_interval.repeated);
        EXPECT_FALSE(cp.whole_seed_scatter_interval.repeated);
        EXPECT_LE(p.partition_interval.end_ns, cp.whole_prefix_interval.start_ns);
        EXPECT_LE(cp.whole_prefix_interval.start_ns, cp.whole_seed_scatter_interval.start_ns);
        EXPECT_LE(cp.whole_seed_scatter_interval.end_ns, cp.whole_prefix_interval.end_ns);
        EXPECT_LE(cp.whole_prefix_interval.end_ns, p.prefix_interval.end_ns);
        for (const auto& phase : decoder.serial_decoder().components()[c].mwpm.predecoder.stats.timeline)
            if (phase.present) {
                EXPECT_LE(cp.whole_seed_scatter_interval.end_ns, phase.start_ns);
                EXPECT_LE(phase.end_ns, cp.whole_prefix_interval.end_ns);
            }
        if (dispatched && c == 1) {
            EXPECT_LE(p.whole_prefix_dispatch_interval.start_ns, cp.whole_prefix_interval.start_ns);
            EXPECT_LE(cp.whole_prefix_interval.end_ns, p.whole_prefix_join_interval.end_ns);
        }
    }
}
#endif
}
TEST(SharedComponents, WholePrefixMatchesSerialOrderedPreparedStateColdWarmBothStencils) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    size_t imported = 0, completed = 0;
    for (bool rotated : {false, true}) {
        auto [dem, spec] = model(rotated, 5, 'x');
        auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        ComponentDecoder addresses(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        std::vector<std::vector<uint64_t>> shots(6);
        for (size_t k = 0; k < spec.coordinates.size(); ++k) {
            shots[0].push_back(k);
            if (k % 7 == 1) shots[1].push_back(k);
            if (k % 13 == 1) shots[2].push_back(k);
        }
        for (size_t c = 0; c < 2; ++c)
            for (auto id : addresses.components()[c].global_detectors)
                if (id % 7 == 1) shots[3 + c].push_back(id);
        for (size_t threads : {1, 2}) {
            ComponentDecoder serial(graph, spec, NUM_DISTINCT_WEIGHTS, false);
            SharedComponentDecoder whole(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                std::vector<int>(threads, -1), 1, 100000, 0, 3, false, Schedule::component_owned_whole,
                SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
            whole.run_with_team([&] {
                for (size_t repeat = 0; repeat < 2; ++repeat) for (auto shot : shots) {
                    if (repeat) std::reverse(shot.begin(), shot.end());
                    serial.prepare_for_mwpm(shot);
                    whole.prepare_for_mwpm(shot);
                    const auto a = snapshot_whole_prefix(serial);
                    const auto b = snapshot_whole_prefix(whole.serial_decoder());
                    EXPECT_EQ(a.values, b.values);
                    EXPECT_EQ(a.generations, b.generations);
                    for (size_t c = 0; c < 2; ++c) {
                        EXPECT_EQ(whole.component_route(c), structured::SharedRoute::serial);
                        EXPECT_EQ(whole.active_predecode_threads(c), 1U);
                        const auto& component = whole.serial_decoder().components()[c];
                        std::vector<uint64_t> expected_order, actual_order;
                        for (auto id : shot)
                            if (std::find(component.global_detectors.begin(), component.global_detectors.end(), id) !=
                                component.global_detectors.end()) expected_order.push_back(id);
                        for (const auto& range : component.mwpm.predecoder.seed_view.active_ranges())
                        for (const auto& seed : component.mwpm.predecoder.seed_view.storage.subspan(range.begin, range.count()))
                            actual_order.push_back(component.global_detectors[seed.detector_id]);
                        EXPECT_EQ(actual_order, expected_order);
                        imported += !component.mwpm.predecoder.complete;
                        completed += component.mwpm.predecoder.complete;
                    }
#if PM_STRUCTURED_PROFILE_TIMELINE
                    expect_whole_prefix_timeline(whole);
#endif
                    EXPECT_EQ(whole.solve_prepared(), serial.solve_prepared());
#if PM_STRUCTURED_PROFILE_TIMELINE
                    expect_continuation_timeline(whole, true);
#endif
                    whole.reset_after_shot(); serial.reset_after_shot();
                    EXPECT_EQ(snapshot_whole_prefix(whole.serial_decoder()).values, snapshot_whole_prefix(serial).values);
                }
            });
        }
    }
    EXPECT_GT(imported, 0U); EXPECT_GT(completed, 0U);
}
TEST(SharedComponents, WholePrefixKeepsPrivateStagingEpochAndUnusedCacheAcrossMoves) {
    using Access = structured::ComponentDecoderTestAccess;
    auto graph = pair_graph(); graph.nodes.resize(5); graph.set_boundary({4});
    auto spec = pair_spec(); spec.coordinates.push_back({0, 0, 2});
    ComponentDecoder input(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    Access::bind(input, 2, 3);
    const auto initial = Access::addresses(input);
    const std::vector<uint64_t> shot{4, 3, 1, 2, 0, 4};
    Access::stage_whole(input, shot);
    EXPECT_TRUE(Access::staged(input)); EXPECT_FALSE(Access::cached_stage(input));
    for (const auto& c : input.components()) EXPECT_FALSE(c.mwpm.predecoder.has_live_cell_state());
    for (size_t c = 0; c < 2; ++c) Access::scatter_whole(input, shot, c);
    input.publish_partitioned_shot(); input.reset_after_shot();
    Access::epoch(input, UINT32_MAX); Access::stamp_all(input, UINT32_MAX);
    Access::stage_whole(input, {0, 1});
    EXPECT_EQ(Access::epoch(input), 1U);
    EXPECT_EQ(Access::stamps(input), std::vector<uint32_t>(5, 0));
    for (auto& seed : input.shot_seeds) seed.grid = nullptr;
    input.reset();  // Unfilled retained records never become cleanup authority.
    EXPECT_THROW(Access::stage_whole(input, {0, 0}), std::invalid_argument);
    try {
        Access::stage_whole(input, {0, 0, 99, 98}); FAIL();
    } catch (const std::invalid_argument& e) {
        EXPECT_EQ(std::string(e.what()), "Detection event index 99 is not in the graph.");
    }
    input.prepare_for_mwpm({0, 1});
    EXPECT_THROW(Access::stage_whole(input, {}), std::logic_error);
    input.reset();
    SharedComponentDecoder whole(std::move(input), {-1, -1}, 1, 0, 0, 3, false,
        SharedComponentDecoder::PrefixSchedule::component_owned_whole,
        SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
    EXPECT_THROW(Access::stage_whole(input, {}), std::logic_error);
    whole.run_with_team([&] {
        whole.prepare_for_mwpm(shot);
        EXPECT_EQ(whole.solve_prepared().weight, 10);
        whole.reset_after_shot();
        EXPECT_THROW(whole.prepare_for_mwpm({0, 0}), std::invalid_argument);
        whole.prepare_for_mwpm({}); EXPECT_NO_THROW(whole.solve_prepared()); whole.reset_after_shot();
    });
    const auto kept = Access::addresses(whole.serial_decoder());
    EXPECT_EQ(Access::binding(whole.serial_decoder()), (std::pair<size_t, size_t>{2, 3}));
    for (size_t k = 0; k < initial.size(); ++k)
        for (size_t f = 0; f < 4; ++f) EXPECT_EQ(initial[k][f], kept[k][f]);
    SharedComponentDecoder spatial(std::move(whole.serial_decoder()), {-1, -1}, 4, 0, 0, 2, false);
    EXPECT_EQ(Access::binding(spatial.serial_decoder()), (std::pair<size_t, size_t>{4, 2}));
    for (size_t c = 0; c < spatial.serial_decoder().components().size(); ++c) {
        const auto& component = spatial.serial_decoder().components()[c];
        const auto& grid = *component.seed_grid;
        const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, 4, 2);
        const auto rebound = Access::addresses(spatial.serial_decoder());
        for (auto id : component.global_detectors) EXPECT_EQ(rebound[id][1], cuts.bucket(rebound[id][3]));
    }
    spatial.run_with_team([&] { EXPECT_EQ(spatial.decode({0, 1, 2, 3}).weight, 10); });
}
namespace {
std::array<std::atomic<size_t>, 2> whole_neighborhood_calls;
std::array<uint32_t (*)(const structured::OneHopPredecoder::Grid&, size_t, uint16_t), 2> whole_neighborhoods;
size_t whole_failure_mask;
uint32_t fail_whole_component(const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    const auto c = grid.decoder_component;
    ++whole_neighborhood_calls[c];
    if (whole_failure_mask & (size_t{1} << c))
        throw std::runtime_error(c ? "whole component one failure" : "whole component zero failure");
    return whole_neighborhoods[c](grid, cell, boundary);
}
}
TEST(SharedComponents, WholePrefixJoinsEitherAndBothOwnerFailuresBeforePublishedCleanup) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> dense(spec.coordinates.size()), sparse;
    std::iota(dense.begin(), dense.end(), 0);
    for (size_t k = 1; k < dense.size(); k += 7) sparse.push_back(k);
    ComponentDecoder serial(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    const auto expected = serial.decode(sparse);
    for (size_t threads : {1, 2}) for (size_t failed : {1, 2, 3}) {
        SharedComponentDecoder whole(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 1, 0, 0, 3, false,
            SharedComponentDecoder::PrefixSchedule::component_owned_whole,
            SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
        whole_failure_mask = failed;
        for (size_t c = 0; c < 2; ++c) {
            auto& grid = *whole.serial_decoder().components()[c].seed_grid;
            whole_neighborhoods[c] = grid.predecode.neighborhood;
            grid.predecode.neighborhood = fail_whole_component;
            whole_neighborhood_calls[c] = 0;
        }
        whole.run_with_team([&] {
            try { whole.prepare_for_mwpm(dense); FAIL(); }
            catch (const std::runtime_error& e) {
                EXPECT_EQ(std::string(e.what()), failed & 1 ? "whole component zero failure" : "whole component one failure");
            }
            for (size_t c = 0; c < 2; ++c) {
                EXPECT_GT(whole_neighborhood_calls[c].load(), 0U);
                whole.serial_decoder().components()[c].seed_grid->predecode.neighborhood = whole_neighborhoods[c];
#if PM_STRUCTURED_PROFILE_TIMELINE
                EXPECT_TRUE(whole.profile.component[c].whole_prefix_interval.present);
                EXPECT_TRUE(whole.profile.component[c].whole_seed_scatter_interval.present);
                EXPECT_LE(whole.profile.component[c].whole_prefix_interval.end_ns, whole.profile.prefix_interval.end_ns);
#endif
            }
            const auto& retired = structured::ComponentDecoderTestAccess::retired(whole.serial_decoder());
            ASSERT_EQ(retired.size(), dense.size());
            for (const auto& seed : retired) {
                ASSERT_NE(seed.grid, nullptr);
                EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0U);
                EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
            }
            expect_clean_nodes(whole.serial_decoder());
            whole.prepare_for_mwpm(sparse); EXPECT_EQ(whole.solve_prepared(), expected); whole.reset_after_shot();
            // Force cold reservation failure in either component, then recover.
            for (size_t c = 0; c < 2; ++c) {
                whole.reset();
                auto& pool = *whole.serial_decoder().components()[c].mwpm.flooder.regions;
                structured::RegionPoolTestAccess::set_generation(pool, 1,
                    std::numeric_limits<structured::RegionGeneration>::max());
                EXPECT_THROW(whole.prepare_for_mwpm(sparse), std::overflow_error);
                expect_clean_nodes(whole.serial_decoder());
                for (const auto& component : whole.serial_decoder().components()) {
                    EXPECT_TRUE(component.mwpm.flooder.queue.empty());
                    EXPECT_TRUE(component.mwpm.flooder.regions->empty());
                    EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
                }
                structured::RegionPoolTestAccess::set_generation(pool, 1, 0);
                whole.prepare_for_mwpm(sparse); EXPECT_EQ(whole.solve_prepared(), expected); whole.reset_after_shot();
            }
        });
    }
}
TEST(SharedComponents, WholePrefixPriorOwnersSurviveImplicitNormalFallbackMoveAndExactlyOnceReset) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> shot;
    for (size_t i = 1; i < spec.coordinates.size(); i += 7) shot.push_back(i);
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    const auto expected = reference.decode(shot);
    for (size_t threads : {1, 2}) {
        SharedComponentDecoder whole(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 1, 100000, 2, 3, false,
            SharedComponentDecoder::PrefixSchedule::component_owned_whole,
            SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
        whole.run_with_team([&] {
            whole.prepare_for_mwpm(shot); EXPECT_EQ(whole.solve_prepared(), expected);
            ASSERT_TRUE(whole.serial_decoder().components()[1].mwpm.has_pending_seed_cleanup());
            EXPECT_EQ(whole.decode(shot), expected); expect_clean_nodes(whole.serial_decoder());
#if PM_STRUCTURED_PROFILE_TIMELINE
            EXPECT_FALSE(whole.profile.component[0].whole_prefix_interval.present);
#endif
            const auto matches = whole.decode_to_matches(shot);
            EXPECT_FALSE(matches.empty()); expect_clean_nodes(whole.serial_decoder());
            whole.prepare_for_mwpm(shot); EXPECT_EQ(whole.solve_prepared(), expected);
            whole.prepare_for_mwpm({});  // Previous owner cleanup precedes fallback owner0 descriptors.
            EXPECT_EQ(whole.component_route(0), structured::SharedRoute::whole_serial);
            EXPECT_EQ(whole.solve_prepared(), MatchingResult{});
#if PM_STRUCTURED_PROFILE_TIMELINE
            expect_continuation_timeline(whole, true);
            EXPECT_FALSE(whole.profile.component[1].whole_prefix_interval.present);
#endif
            whole.prepare_for_mwpm(shot); EXPECT_EQ(whole.solve_prepared(), expected);
            EXPECT_THROW((SharedComponentDecoder(std::move(whole))), std::logic_error);
        });
        SharedComponentDecoder moved(std::move(whole));
        EXPECT_THROW(whole.team_size(), std::logic_error);
        moved.run_with_team([&] {
            moved.prepare_for_mwpm(shot); EXPECT_EQ(moved.solve_prepared(), expected);
            std::vector<std::pair<size_t, size_t>> prior;
            for (size_t c = 0; c < 2; ++c) {
                auto& mwpm = moved.serial_decoder().components()[c].mwpm;
                ASSERT_TRUE(mwpm.has_pending_seed_cleanup());
                ASSERT_TRUE(mwpm.flooder.regions->has_pending_neutral_reset());
                for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
                    for (const auto& seed : mwpm.predecoder.seed_view.storage.subspan(range.begin, range.count())) prior.emplace_back(c, seed.detector_id);
            }
            moved.reset_after_shot(); expect_clean_nodes(moved.serial_decoder());
            for (const auto& [c, id] : prior) {
                auto& node = moved.serial_decoder().components()[c].mwpm.flooder.graph.node(id);
                node.observables_crossed_from_source = 123; node.set_wrapped_radius(246);
            }
            for (const auto& component : moved.serial_decoder().components()) {
                EXPECT_FALSE(component.mwpm.flooder.regions->has_pending_neutral_reset());
                EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
            }
            moved.reset_after_shot();
            for (const auto& [c, id] : prior) {
                auto& node = moved.serial_decoder().components()[c].mwpm.flooder.graph.node(id);
                EXPECT_EQ(node.observables_crossed_from_source, 123U);
                EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].wrapped_radius_cached, 246);
                node.reset();
            }
            moved.prepare_for_mwpm(shot); moved.reset_after_shot(); expect_clean_nodes(moved.serial_decoder());
            moved.prepare_for_mwpm(shot); EXPECT_EQ(moved.solve_prepared(), expected);
            moved.reset(); expect_clean_nodes(moved.serial_decoder());
        });
    }
}
TEST(SharedComponents, WholePrefixHandlesOneComponentAndEitherOrBothFormalFailures) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    using Placement = SharedComponentDecoder::ContinuationPlacement;
    for (size_t threads : {1, 2}) {
        BackendUserGraph one_graph(2, 1);
        one_graph.add_or_merge_edge(0, 1, {0}, 2, -1); one_graph.add_or_merge_boundary_edge(0, {}, 7, -1);
        StaticGridSpec one_spec{3, 3, false, {{{0, 0, 0}}, {{2, 0, 0}}}, {}, GridSimd::scalar};
        SharedComponentDecoder one(ComponentDecoder(one_graph, one_spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 1, 10000, 0, 3, false, Schedule::component_owned_whole, Placement::prefix_tail_owner);
        one.run_with_team([&] {
            for (const auto& events : std::vector<std::vector<uint64_t>>{{}, {0}, {0, 1}}) {
                one.prepare_for_mwpm(events);
#if PM_STRUCTURED_PROFILE_TIMELINE
                expect_whole_prefix_timeline(one);
#endif
                EXPECT_NO_THROW(one.solve_prepared());
#if PM_STRUCTURED_PROFILE_TIMELINE
                expect_continuation_timeline(one, true);
#endif
                one.reset_after_shot(); expect_clean_nodes(one.serial_decoder());
            }
        });
        for (size_t failed : {1, 2, 3}) {
            BackendUserGraph graph(4, 2);
            graph.add_or_merge_edge(0, 2, {0}, 2, -1); graph.add_or_merge_edge(1, 3, {1}, 3, -1);
            if (!(failed & 1)) graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
            if (!(failed & 2)) graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
            SharedComponentDecoder whole(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
                std::vector<int>(threads, -1), 1, 0, 0, 3, false, Schedule::component_owned_whole, Placement::prefix_tail_owner);
            whole.run_with_team([&] {
                whole.prepare_for_mwpm({0, 1}); EXPECT_THROW(whole.solve_prepared(), std::invalid_argument);
#if PM_STRUCTURED_PROFILE_TIMELINE
                expect_continuation_timeline(whole, true);
#endif
                expect_clean_nodes(whole.serial_decoder());
                whole.prepare_for_mwpm({0, 1, 2, 3}); EXPECT_EQ(whole.solve_prepared().weight, 10);
                whole.reset_after_shot(); expect_clean_nodes(whole.serial_decoder());
            });
        }
    }
}

TEST(SharedComponents, CachedBucketRebindsMovedDecoderAndPreservesSharedMove) {
    using Access = structured::ComponentDecoderTestAccess;
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder input(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    Access::bind(input, 2, 3);
    const auto old = Access::addresses(input);
    SharedComponentDecoder shared(std::move(input), {-1, -1}, 4, 0, 0, 2, false,
        SharedComponentDecoder::PrefixSchedule::component_owned_tail,
        SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
    const auto rebound = Access::addresses(shared.serial_decoder());
    size_t changed = 0;
    for (size_t c = 0; c < 2; ++c) {
        const auto& component = shared.serial_decoder().components()[c];
        const auto& grid = *component.seed_grid;
        const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, 4, 2);
        for (auto global : component.global_detectors) {
            EXPECT_EQ(rebound[global][1], cuts.bucket(rebound[global][3]));
            changed += rebound[global][1] != old[global][1];
        }
    }
    EXPECT_GT(changed, 0U);
    SharedComponentDecoder moved(std::move(shared));
    EXPECT_EQ(Access::addresses(moved.serial_decoder()), rebound);
    SharedComponentDecoder reference(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
        {-1}, 4, 0, 0, 2, false, SharedComponentDecoder::PrefixSchedule::component_owned_tail,
        SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
    std::vector<uint64_t> shot;
    for (size_t k = 0; k < spec.coordinates.size(); k += 13) shot.push_back(k);
    std::vector<uint64_t> state;
    MatchingResult expected;
    reference.run_with_team([&] {
        reference.prepare_for_mwpm(shot);
        state = snapshot_imported_regions(reference.serial_decoder()).values;
        expected = reference.solve_prepared(); reference.reset_after_shot();
    });
    moved.run_with_team([&] {
        for (size_t repeat = 0; repeat < 2; ++repeat) {
            moved.prepare_for_mwpm(shot);
            EXPECT_EQ(snapshot_imported_regions(moved.serial_decoder()).values, state);
            EXPECT_EQ(moved.solve_prepared(), expected);
            moved.reset_after_shot();
        }
    });
}

TEST(SharedComponents, OwnerScatterStableBucketsAndPreparedStateAcrossThreads) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder addresses(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<std::vector<uint64_t>> shots(6);
    for (size_t c = 0; c < 2; ++c)
        for (auto global : addresses.components()[c].global_detectors) {
            if (global % 7 == 1) shots[0].push_back(global);
            if (c == 1 && global % 9 == 1) shots[1].push_back(global); // Empty C0 still fills C1.
            if (global % 11 == 2) shots[2].push_back(global);
        }
    std::sort(shots[0].begin(), shots[0].end());
    std::reverse(shots[2].begin(), shots[2].end());
    shots[4] = shots[0];
    shots[5] = shots[1];
    for (size_t parts : {2, 4}) {
        std::vector<std::vector<uint64_t>> reference_states(shots.size());
        std::vector<MatchingResult> reference_results(shots.size());
        for (size_t mode : {0, 1, 2, 3}) for (size_t threads : {1, 2, 4}) {
            const bool import_parallel = mode == 1;
            SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                std::vector<int>(threads, -1), parts, 0, 0, 3, import_parallel, mode >= 2 ? SharedComponentDecoder::PrefixSchedule::component_owned_tail
                    : SharedComponentDecoder::PrefixSchedule::component_sequential, mode == 3
                    ? SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner
                    : SharedComponentDecoder::ContinuationPlacement::component_captain);
            decoder.run_with_team([&] {
                for (size_t k = 0; k < shots.size(); ++k) {
                    SCOPED_TRACE(::testing::Message() << "P" << parts << " T" << threads << " I" << import_parallel << " shot" << k);
                    decoder.prepare_for_mwpm(shots[k]);
                    const auto& serial = decoder.serial_decoder();
                    for (size_t c = 0; c < 2; ++c) {
                        const auto& component = serial.components()[c];
                        const auto& grid = *component.seed_grid;
                        const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, parts, 3);
                        std::vector<uint64_t> expected;
                        for (size_t b = 0; b <= parts; ++b) for (auto global : shots[k]) {
                            auto found = std::find(component.global_detectors.begin(), component.global_detectors.end(), global);
                            if (found == component.global_detectors.end()) continue;
                            auto local = size_t(found - component.global_detectors.begin());
                            if (cuts.bucket(component.mwpm.flooder.graph.node(local).grid_cell) == b) expected.push_back(global);
                        }
                        const auto seeds = component.mwpm.predecoder.seed_view.storage;
                        ASSERT_EQ(seeds.size(), expected.size());
                        for (size_t i = 0; i < seeds.size(); ++i) {
                            EXPECT_EQ(component.global_detectors[seeds[i].detector_id], expected[i]);
                            EXPECT_EQ(grid.predecode.active[seeds[i].cell], i + 1);
                        }
                    }
                    const auto state = snapshot_imported_regions(serial).values;
                    if (mode == 0 && threads == 1) reference_states[k] = state;
                    else EXPECT_EQ(state, reference_states[k]);
#if PM_STRUCTURED_PROFILE_TIMELINE
                    expect_owner_publication_timeline(decoder);
                    if (mode >= 2) expect_component_tail_timeline(decoder);
#endif
                    const auto result = decoder.solve_prepared();
#if PM_STRUCTURED_PROFILE_TIMELINE
                    expect_continuation_timeline(decoder, mode == 3);
#endif
                    if (mode == 0 && threads == 1) reference_results[k] = result;
                    else EXPECT_EQ(result, reference_results[k]);
                    decoder.reset_after_shot();
                }
            });
        }
    }
}

TEST(SharedComponents, FusedOwnerPublicationHandlesConflictOnlyAndEveryP4Lane) {
    // Rounds=3*d gives all four W3 interiors at least one detector. Small
    // same-time-slice models otherwise leave the middle P4 buckets empty.
    BackendUserGraph graph(54, 2);
    StaticGridSpec spec{9, 27, false, {}, {}, GridSimd::scalar};
    for (size_t t = 0; t < 27; ++t) for (size_t c = 0; c < 2; ++c) {
        spec.coordinates.push_back({0, 0, int64_t(t)});
        spec.groups.push_back(c);
        if (t) graph.add_or_merge_edge(2 * (t - 1) + c, 2 * t + c, {}, 2 + 2 * (t % 3), -1);
        if (t == 0 || t == 26) graph.add_or_merge_boundary_edge(2 * t + c, {c}, 4, -1);
    }
    ComponentDecoder addresses(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    for (size_t parts : {2, 4}) {
        std::vector<std::vector<uint64_t>> shots(6);
        std::array<std::array<size_t, 9>, 2> bucket_counts{};
        for (size_t c = 0; c < 2; ++c) {
            const auto& component = addresses.components()[c];
            const auto& grid = *component.seed_grid;
            const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, parts, 3);
            for (size_t i = 0; i < component.global_detectors.size(); ++i) {
                const auto global = component.global_detectors[i];
                const auto bucket = cuts.bucket(component.mwpm.flooder.graph.node(i).grid_cell);
                ++bucket_counts[c][bucket];
                shots[0].push_back(global);
                shots[bucket == parts ? 1 : 2].push_back(global);
                if (c == 1) shots[3].push_back(global);
            }
        }
        for (size_t c = 0; c < 2; ++c)
            for (size_t b = 0; b <= parts; ++b) ASSERT_GT(bucket_counts[c][b], 0);
        shots[5] = shots[0];
        for (auto& shot : shots) std::sort(shot.begin(), shot.end());
        std::vector<std::vector<uint64_t>> expected_states(shots.size());
        std::vector<MatchingResult> expected_results(shots.size());
        for (size_t mode : {0, 1, 2}) for (size_t threads : {1, 2, 4}) {
            const bool parallel_import = mode == 1;
            SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                std::vector<int>(threads, -1), parts, 0, 0, 3, parallel_import, mode == 2 ? SharedComponentDecoder::PrefixSchedule::component_owned_tail
                    : SharedComponentDecoder::PrefixSchedule::component_sequential);
            decoder.run_with_team([&] {
                // Alternate all/conflict-only/interior-only/empty-C0/empty
                // shots twice; the next fill must not reuse an old ready flag.
                for (size_t repeat = 0; repeat < 2; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                    SCOPED_TRACE(::testing::Message() << "P" << parts << " T" << threads
                        << " I" << parallel_import << " repeat" << repeat << " shot" << k);
                    decoder.prepare_for_mwpm(shots[k]);
                    const auto state = snapshot_imported_regions(decoder.serial_decoder()).values;
                    if (mode == 0 && threads == 1 && repeat == 0) expected_states[k] = state;
                    else EXPECT_EQ(state, expected_states[k]);
#if PM_STRUCTURED_PROFILE_TIMELINE
                    expect_owner_publication_timeline(decoder);
                    if (mode == 2) expect_component_tail_timeline(decoder);
#endif
                    std::vector<structured::PredecodeSeed> conflict_seeds;
                    for (const auto& component : decoder.serial_decoder().components()) {
                        const auto& pre = component.mwpm.predecoder;
                        const auto range = pre.scratch_layout().back();
                        for (size_t i = range.seed_begin; i < range.seed_end; ++i)
                            conflict_seeds.push_back(pre.seed_view.storage[i]);
                    }
                    const auto result = decoder.solve_prepared();
                    if (mode == 0 && threads == 1 && repeat == 0) expected_results[k] = result;
                    else EXPECT_EQ(result, expected_results[k]);
                    decoder.reset_after_shot();
                    expect_clean_nodes(decoder.serial_decoder());
                    for (const auto& seed : conflict_seeds) {
                        EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0U);
                        EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
                        seed.grid->predecode.active[seed.cell] = 123;
                        seed.grid->predecode.radii[seed.cell] = 246;
                    }
                    decoder.reset_after_shot();
                    for (const auto& seed : conflict_seeds) {
                        // These are actual old conflict records in each
                        // component, so repeating a stale owner range fails.
                        EXPECT_EQ(seed.grid->predecode.active[seed.cell], 123U);
                        EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 246);
                        seed.grid->predecode.active[seed.cell] = 0;
                        seed.grid->predecode.radii[seed.cell] = 0;
                    }
                }
            });
        }
    }
}

TEST(SharedComponents, ComponentTailScatterMasksKeepComponentRecordsDisjoint) {
    BackendUserGraph graph(54, 2);
    StaticGridSpec spec{9, 27, false, {}, {}, GridSimd::scalar};
    for (size_t t = 0; t < 27; ++t) for (size_t c = 0; c < 2; ++c) {
        spec.coordinates.push_back({0, 0, int64_t(t)});
        spec.groups.push_back(c);
        if (t) graph.add_or_merge_edge(2 * (t - 1) + c, 2 * t + c, {}, 2, -1);
        if (t == 0 || t == 26) graph.add_or_merge_boundary_edge(2 * t + c, {c}, 4, -1);
    }
    std::vector<uint64_t> events(54);
    std::iota(events.rbegin(), events.rend(), 0);  // Valid unsorted input remains stable within each bucket.
    for (size_t parts : {2, 4}) {
        ComponentDecoder decoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        std::array<structured::TimePartitionCuts, 2> cuts;
        std::array<ComponentDecoder::PartitionSeedLayout, 2> layouts;
        for (size_t c = 0; c < 2; ++c) {
            const auto& grid = *decoder.components()[c].seed_grid;
            cuts[c] = structured::TimePartitionCuts(grid.nx * grid.ny, grid.nt, parts, 3);
            layouts[c].cuts = &cuts[c];
        }
        decoder.stage_partitioned_shot(events, layouts, parts);
        uint32_t even_interiors = 0;
        for (size_t b = 0; b < parts; b += 2) even_interiors |= uint32_t{1} << b;
        const std::array<uint32_t, 2> first_masks{even_interiors | (uint32_t{1} << parts), even_interiors};
        const uint32_t all = (uint32_t{1} << (parts + 1)) - 1;
        const std::array<uint32_t, 2> second_masks{all ^ first_masks[0], all ^ first_masks[1]};
        const auto check = [&](std::array<uint32_t, 2> published) {
            for (size_t c = 0; c < 2; ++c) {
                const auto& component = decoder.components()[c];
                size_t expected_index = 0;
                for (size_t b = 0; b <= parts; ++b) for (auto event : events) {
                    const auto found = std::find(component.global_detectors.begin(), component.global_detectors.end(), event);
                    if (found == component.global_detectors.end()) continue;
                    const auto local = size_t(found - component.global_detectors.begin());
                    const auto& node = component.mwpm.flooder.graph.node(local);
                    if (cuts[c].bucket(node.grid_cell) != b) continue;
                    if (published[c] & (uint32_t{1} << b)) {
                        EXPECT_EQ(component.seed_grid->predecode.active[node.grid_cell], expected_index + 1);
                        const auto& seed = component.mwpm.predecoder.seed_view.storage[expected_index];
                        EXPECT_EQ(seed.detector_id, local);
                        EXPECT_EQ(seed.cell, node.grid_cell);
                        EXPECT_EQ(seed.grid, component.seed_grid);
                    } else {
                        EXPECT_EQ(component.seed_grid->predecode.active[node.grid_cell], 0U);
                    }
                    ++expected_index;
                }
                EXPECT_EQ(expected_index, component.mwpm.predecoder.seed_view.active_count());
                EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
            }
        };
        decoder.scatter_partitioned_shot(events, layouts, std::span<const uint32_t>(first_masks));
        check(first_masks);
        decoder.scatter_partitioned_shot(events, layouts, std::span<const uint32_t>(second_masks));
        check({all, all});
        decoder.publish_partitioned_shot();
        decoder.reset_after_shot();
        for (const auto& component : decoder.components())
            for (const auto active : component.seed_grid->predecode.active) EXPECT_EQ(active, 0U);
    }
}

namespace {
const structured::TimePartitionCuts* failing_cuts;
size_t failing_bucket;
uint32_t (*original_neighborhood)(const structured::OneHopPredecoder::Grid&, size_t, uint16_t);
uint32_t fail_selected_bucket(const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    if (failing_cuts->bucket(cell) == failing_bucket)
        throw std::runtime_error("injected post-fill Degree failure");
    return original_neighborhood(grid, cell, boundary);
}
}
TEST(SharedComponents, OwnerScatterPublishesBothSpansBeforeJoinedFailureRecovery) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> events(spec.coordinates.size());
    std::iota(events.begin(), events.end(), 0);
    for (size_t bucket : {0, 1}) for (size_t threads : {1, 2, 4}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, true);
        auto& component = decoder.serial_decoder().components()[0];
        auto& grid = *component.seed_grid;
        const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, 2, 3);
        failing_cuts = &cuts;
        failing_bucket = bucket;
        original_neighborhood = grid.predecode.neighborhood;
        grid.predecode.neighborhood = fail_selected_bucket;
        decoder.run_with_team([&] {
            EXPECT_THROW(decoder.prepare_for_mwpm(events), std::runtime_error);
            grid.predecode.neighborhood = original_neighborhood;
            const auto& retired = structured::ComponentDecoderTestAccess::retired(decoder.serial_decoder());
            ASSERT_EQ(retired.size(), events.size());
            for (const auto& seed : retired) {
                ASSERT_NE(seed.grid, nullptr);
                EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0U);
                EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
            }
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(events);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder());
        });
    }
}
namespace {
std::array<decltype(original_neighborhood), 2> original_tail_neighborhoods;
uint32_t fail_tail_component_zero(const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    if (failing_cuts->bucket(cell) == failing_bucket) throw std::runtime_error("component zero failure");
    return original_tail_neighborhoods[0](grid, cell, boundary);
}
uint32_t fail_tail_component_one(const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    if (failing_cuts->bucket(cell) == (failing_bucket == 2 ? 2 : 0))
        throw std::runtime_error("component one failure");
    return original_tail_neighborhoods[1](grid, cell, boundary);
}
}
TEST(SharedComponents, ComponentTailErrorsKeepComponentPriorityAfterAllJoins) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> events(spec.coordinates.size());
    std::iota(events.begin(), events.end(), 0);
    for (size_t bucket : {1, 2}) for (size_t threads : {1, 2, 4}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, false,
            SharedComponentDecoder::PrefixSchedule::component_owned_tail);
        auto& g0 = *decoder.serial_decoder().components()[0].seed_grid;
        auto& g1 = *decoder.serial_decoder().components()[1].seed_grid;
        const structured::TimePartitionCuts cuts(g0.nx * g0.ny, g0.nt, 2, 3);
        failing_cuts = &cuts;
        failing_bucket = bucket;
        original_tail_neighborhoods = {g0.predecode.neighborhood, g1.predecode.neighborhood};
        g0.predecode.neighborhood = fail_tail_component_zero;
        g1.predecode.neighborhood = fail_tail_component_one;
        decoder.run_with_team([&] {
            try {
                decoder.prepare_for_mwpm(events);
                FAIL() << "Both configured components must fail";
            } catch (const std::runtime_error& error) {
                EXPECT_EQ(std::string(error.what()), "component zero failure");
            }
            g0.predecode.neighborhood = original_tail_neighborhoods[0];
            g1.predecode.neighborhood = original_tail_neighborhoods[1];
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(events);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
        });
    }
}

TEST(SharedComponents, ComponentTailJoinsInteriorConflictAndImportFailures) {
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> dense(spec.coordinates.size()), sparse;
    std::iota(dense.begin(), dense.end(), 0);
    for (size_t i = 1; i < dense.size(); i += 7) sparse.push_back(i);
    // A dequeued event can leave an inactive timestamp payload on a node
    // never reached by a region. Successful reset must match the legacy state,
    // whereas hard recovery resets every graph node to its all-zero state.
    SharedComponentDecoder reference(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
        {-1}, 2, 0, 0, 3, false);
    std::vector<uint64_t> expected_reset_state;
    reference.run_with_team([&] {
        reference.prepare_for_mwpm(sparse);
        reference.solve_prepared();
        reference.reset_after_shot();
        expect_clean_nodes(reference.serial_decoder(), false);
        expected_reset_state = snapshot_imported_regions(reference.serial_decoder()).values;
    });
    // Both components and both interior owners can fail; bucket 2 fails in
    // the tail after the shared interior join, while its peer tail completes.
    for (size_t c : {0, 1}) for (size_t bucket : {0, 1, 2}) for (size_t threads : {1, 2, 4}) {
        SCOPED_TRACE(::testing::Message() << "C" << c << " bucket" << bucket << " T" << threads);
        SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, false,
            SharedComponentDecoder::PrefixSchedule::component_owned_tail);
        auto& grid = *decoder.serial_decoder().components()[c].seed_grid;
        const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, 2, 3);
        const auto& component = decoder.serial_decoder().components()[c];
        bool occupied = false;
        for (size_t id = 0; id < component.mwpm.flooder.graph.num_nodes; ++id)
            occupied |= cuts.bucket(component.mwpm.flooder.graph.node(id).grid_cell) == bucket;
        ASSERT_TRUE(occupied);  // The injected interior/conflict job must contain real seeds.
        failing_cuts = &cuts;
        failing_bucket = bucket;
        original_neighborhood = grid.predecode.neighborhood;
        grid.predecode.neighborhood = fail_selected_bucket;
        decoder.run_with_team([&] {
            EXPECT_THROW(decoder.prepare_for_mwpm(dense), std::runtime_error);
            grid.predecode.neighborhood = original_neighborhood;
            const auto& retired = structured::ComponentDecoderTestAccess::retired(decoder.serial_decoder());
            ASSERT_EQ(retired.size(), dense.size());
            for (const auto& seed : retired) {
                ASSERT_NE(seed.grid, nullptr);
                EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0U);
                EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
            }
            expect_clean_nodes(decoder.serial_decoder());
#if PM_STRUCTURED_PROFILE_TIMELINE
            // Both conflict owners finished their whole-shot fill before
            // any fallible work, including when C0 stops a lane before C1.
            for (size_t owner = 0; owner < std::min(threads, size_t{2}); ++owner) {
                const auto& scatter = decoder.profile.lanes[owner].owner_scatter_interval;
                EXPECT_TRUE(scatter.present);
                EXPECT_LE(scatter.end_ns, decoder.profile.interior_join_interval.end_ns);
                EXPECT_FALSE(decoder.profile.lanes[owner].owner_conflict_wait_interval.present);
            }
            if (bucket == 2) {
                EXPECT_TRUE(decoder.profile.component[1 - c].tail_interval.present);
                EXPECT_LE(decoder.profile.component[1 - c].tail_interval.end_ns,
                          decoder.profile.prefix_interval.end_ns);
            }
#endif
            decoder.prepare_for_mwpm(sparse);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder(), false);
            for (const auto& component : decoder.serial_decoder().components()) {
                EXPECT_TRUE(component.mwpm.flooder.queue.empty());
                EXPECT_TRUE(component.mwpm.flooder.regions->empty());
                EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
            }
            EXPECT_EQ(snapshot_imported_regions(decoder.serial_decoder()).values, expected_reset_state);
            // Force a real contiguous-ID reservation exception in either tail.
            auto& pool = *decoder.serial_decoder().components()[c].mwpm.flooder.regions;
            structured::RegionPoolTestAccess::set_generation(
                pool, 1, std::numeric_limits<structured::RegionGeneration>::max());
            EXPECT_THROW(decoder.prepare_for_mwpm(sparse), std::overflow_error);
            expect_clean_nodes(decoder.serial_decoder());
            for (const auto& component : decoder.serial_decoder().components()) {
                EXPECT_TRUE(component.mwpm.flooder.regions->empty());
                EXPECT_TRUE(component.mwpm.flooder.queue.empty());
            }
            structured::RegionPoolTestAccess::set_generation(pool, 1, 0);
            decoder.prepare_for_mwpm(sparse);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder(), false);
            for (const auto& component : decoder.serial_decoder().components()) {
                EXPECT_TRUE(component.mwpm.flooder.queue.empty());
                EXPECT_TRUE(component.mwpm.flooder.regions->empty());
                EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
            }
            EXPECT_EQ(snapshot_imported_regions(decoder.serial_decoder()).values, expected_reset_state);
        });
    }
}

TEST(SharedComponents, ComponentTailPreservesCleanupOwnerAcrossImplicitNextMoveAndReset) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> shot;
    for (size_t i = 1; i < spec.coordinates.size(); i += 7) shot.push_back(i);
    for (auto placement : {SharedComponentDecoder::ContinuationPlacement::component_captain,
                           SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner})
    for (size_t threads : {1, 2, 4}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, false,
            SharedComponentDecoder::PrefixSchedule::component_owned_tail, placement);
        MatchingResult expected;
        std::vector<uint64_t> prepared;
        decoder.run_with_team([&] {
            expected = decoder.decode(shot);  // Ordinary API still performs immediate cleanup.
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(shot);
            prepared = snapshot_imported_regions(decoder.serial_decoder()).values;
            EXPECT_EQ(decoder.solve_prepared(), expected);
            ASSERT_TRUE(decoder.serial_decoder().components()[1].mwpm.has_pending_seed_cleanup());
            EXPECT_EQ(decoder.decode(shot), expected);  // Consume the old C1 owner before replacing it.
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(shot);
            EXPECT_EQ(snapshot_imported_regions(decoder.serial_decoder()).values, prepared);
            EXPECT_EQ(decoder.solve_prepared(), expected);
        });
        SharedComponentDecoder moved(std::move(decoder));
        moved.run_with_team([&] {
            moved.prepare_for_mwpm(shot);  // Inactive move preserves the owner descriptor.
            EXPECT_EQ(snapshot_imported_regions(moved.serial_decoder()).values, prepared);
            EXPECT_EQ(moved.solve_prepared(), expected);
            auto& mwpm = moved.serial_decoder().components()[1].mwpm;
            ASSERT_TRUE(mwpm.has_pending_seed_cleanup());
            const auto id = mwpm.predecoder.seed_view.storage.front().detector_id;
            moved.reset_after_shot();
            expect_clean_nodes(moved.serial_decoder());
            auto& node = mwpm.flooder.graph.node(id);
            node.observables_crossed_from_source = 123;
            node.set_wrapped_radius(246);
            moved.reset_after_shot();
            EXPECT_EQ(node.observables_crossed_from_source, 123U);
            EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].wrapped_radius_cached, 246);
            node.reset();
            moved.prepare_for_mwpm(shot);
            moved.reset_after_shot();  // Abandon: fully reset after both tails have joined.
            expect_clean_nodes(moved.serial_decoder());
            moved.prepare_for_mwpm(shot);
            EXPECT_EQ(moved.solve_prepared(), expected);
            moved.reset();
            expect_clean_nodes(moved.serial_decoder());
        });
    }
}

TEST(SharedComponents, ComponentTailFallbackOneComponentAndSolveFailure) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    for (auto placement : {SharedComponentDecoder::ContinuationPlacement::component_captain,
                           SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner})
    for (size_t threads : {1, 2, 4}) {
        auto fallback_graph = pair_graph();
        SharedComponentDecoder fallback(ComponentDecoder(fallback_graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 10, 3, false, Schedule::component_owned_tail, placement);
        fallback.run_with_team([&] {
            fallback.prepare_for_mwpm({0, 1});
            EXPECT_EQ(fallback.component_route(0), structured::SharedRoute::whole_serial);
#if PM_STRUCTURED_PROFILE_TIMELINE
            EXPECT_FALSE(fallback.profile.interior_interval.present);
            EXPECT_FALSE(fallback.profile.component[0].tail_interval.present);
#endif
            EXPECT_NO_THROW(fallback.solve_prepared());
#if PM_STRUCTURED_PROFILE_TIMELINE
            expect_continuation_timeline(fallback, placement == SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
#endif
            fallback.reset_after_shot();
        });
        BackendUserGraph one_graph(2, 1);
        one_graph.add_or_merge_edge(0, 1, {0}, 2, -1);
        one_graph.add_or_merge_boundary_edge(0, {}, 7, -1);
        StaticGridSpec one_spec{3, 3, false, {{{0, 0, 0}}, {{2, 0, 0}}}, {}, GridSimd::scalar};
        SharedComponentDecoder one(ComponentDecoder(one_graph, one_spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, false, Schedule::component_owned_tail, placement);
        one.run_with_team([&] {
            ASSERT_EQ(one.serial_decoder().components().size(), 1U);
            one.prepare_for_mwpm({0});
            EXPECT_NO_THROW(one.solve_prepared());
#if PM_STRUCTURED_PROFILE_TIMELINE
            expect_continuation_timeline(one, placement == SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
#endif
            one.reset_after_shot();
            expect_clean_nodes(one.serial_decoder());
        });
        BackendUserGraph disconnected(4, 2);
        disconnected.add_or_merge_edge(0, 2, {0}, 2, -1);
        disconnected.add_or_merge_edge(1, 3, {1}, 3, -1);
        disconnected.add_or_merge_boundary_edge(0, {1}, 7, -1);
        SharedComponentDecoder failing(ComponentDecoder(disconnected, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, false, Schedule::component_owned_tail, placement);
        failing.run_with_team([&] {
            failing.prepare_for_mwpm({0, 1});
            EXPECT_THROW(failing.solve_prepared(), std::invalid_argument);
#if PM_STRUCTURED_PROFILE_TIMELINE
            expect_continuation_timeline(failing, placement == SharedComponentDecoder::ContinuationPlacement::prefix_tail_owner);
#endif
            expect_clean_nodes(failing.serial_decoder());
            failing.prepare_for_mwpm({0, 1, 2, 3});
            EXPECT_EQ(failing.solve_prepared().weight, 10);
            failing.reset_after_shot();
            expect_clean_nodes(failing.serial_decoder());
        });
    }
}

TEST(SharedComponents, TailOwnerContinuesAfterEitherOrBothFormalFailuresAndRecovers) {
    using Schedule = SharedComponentDecoder::PrefixSchedule;
    using Placement = SharedComponentDecoder::ContinuationPlacement;
    // With no boundary an odd component fails in actual MWPM. A boundary on
    // only the other component makes that continuation complete successfully.
    for (size_t failed : {1, 2, 3}) for (size_t threads : {1, 2, 4}) {
        BackendUserGraph graph(4, 2);
        graph.add_or_merge_edge(0, 2, {0}, 2, -1);
        graph.add_or_merge_edge(1, 3, {1}, 3, -1);
        if (!(failed & 1)) graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
        if (!(failed & 2)) graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
        SharedComponentDecoder decoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, false, Schedule::component_owned_tail,
            Placement::prefix_tail_owner);
        decoder.run_with_team([&] {
            for (size_t repeat = 0; repeat < 2; ++repeat) {
                decoder.prepare_for_mwpm({0, 1});
                EXPECT_THROW(decoder.solve_prepared(), std::invalid_argument);
#if PM_STRUCTURED_PROFILE_TIMELINE
                // Both scopes must have returned before reset, even when C0
                // failed; their measured intervals remain in Shared's profile.
                expect_continuation_timeline(decoder, true);
#endif
                expect_clean_nodes(decoder.serial_decoder());
                for (const auto& c : decoder.serial_decoder().components()) {
                    EXPECT_TRUE(c.mwpm.flooder.queue.empty());
                    EXPECT_TRUE(c.mwpm.flooder.regions->empty());
                    EXPECT_FALSE(c.mwpm.predecoder.has_live_cell_state());
                }
                EXPECT_THROW(decoder.solve_prepared(), std::logic_error);
                decoder.prepare_for_mwpm({0, 1, 2, 3});
                EXPECT_EQ(decoder.solve_prepared().weight, 10);
#if PM_STRUCTURED_PROFILE_TIMELINE
                expect_continuation_timeline(decoder, true);
#endif
                decoder.reset_after_shot();
                expect_clean_nodes(decoder.serial_decoder());
            }
        });
    }
}

#endif
TEST(SharedComponents, NeutralRegionsResetOnlySuccessfulImportsAcrossVariableShots) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<std::vector<uint64_t>> shots(3);
    for (size_t k = 0; k < spec.coordinates.size(); ++k) {
        if (k % 7 == 1)
            shots[0].push_back(k);
        if (k % 17 == 1)
            shots[1].push_back(k);
    }
    const auto states = [](const ComponentDecoder& decoder) {
        std::vector<uint8_t> result;
        for (const auto& component : decoder.components()) {
            const auto& pool = *component.mwpm.flooder.regions;
            for (structured::RegionId id = 1; id <= pool.capacity(); ++id)
                result.push_back(structured::RegionPoolTestAccess::state(pool, id));
        }
        return result;
    };
    const auto check_reset = [](const ComponentDecoder& decoder, const std::vector<bool>& imported) {
        expect_clean_nodes(decoder);
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            const auto& pool = *decoder.components()[c].mwpm.flooder.regions;
            EXPECT_TRUE(pool.empty());
            EXPECT_FALSE(pool.has_pending_neutral_reset());
            if (imported[c])
                for (structured::RegionId id = 1; id < structured::RegionPoolTestAccess::next(pool); ++id) {
                    EXPECT_TRUE(structured::RegionPoolTestAccess::state(pool, id) == 3U ||
                                pool.is_cached_seed_slot(id) || pool.is_deferred_seed_slot(id));
                    EXPECT_FALSE(pool.is_live(id, pool.generation(id)));
                    EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
                }
            for (auto id = structured::RegionPoolTestAccess::lifetime_end(pool); id <= pool.capacity(); ++id)
                EXPECT_EQ(structured::RegionPoolTestAccess::state(pool, id), 0U);
        }
    };
    size_t imported_components = 0;
    for (bool parallel_import : {false, true}) for (size_t threads : {1, 2, 4}) {
        SharedComponentDecoder reference(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            {-1}, 2, 0, 0, 3, false);
        std::vector<MatchingResult> expected;
        reference.run_with_team([&] {
            for (const auto& shot : shots)
                expected.push_back(reference.decode(shot));
        });
        SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, parallel_import);
        decoder.run_with_team([&] {
            decoder.reset_after_shot();
            PreparedRegionSnapshot cold;
            for (size_t index : {0, 1, 2, 0, 1}) {
                decoder.prepare_for_mwpm(shots[index]);
                if (index == 0) {
                    const auto prepared = snapshot_imported_regions(decoder.serial_decoder());
                    if (cold.values.empty()) {
                        cold = prepared;
                    } else {
                        EXPECT_EQ(prepared.values, cold.values);
                        ASSERT_EQ(prepared.generations.size(), cold.generations.size());
                        for (size_t k = 0; k < cold.generations.size(); ++k)
                            EXPECT_GT(prepared.generations[k], cold.generations[k]);
                    }
                }
                std::vector<bool> imported;
                for (const auto& component : decoder.serial_decoder().components()) {
                    imported.push_back(!component.mwpm.predecoder.complete);
                    imported_components += imported.back();
                    EXPECT_FALSE(component.mwpm.flooder.regions->has_pending_neutral_reset());
                }
                EXPECT_EQ(decoder.solve_prepared(), expected[index]);
                for (size_t c = 0; c < imported.size(); ++c) {
                    EXPECT_EQ(decoder.serial_decoder().components()[c].mwpm.flooder.regions->has_pending_neutral_reset(),
                              imported[c]);
                    const auto& mwpm = decoder.serial_decoder().components()[c].mwpm;
                    EXPECT_EQ(mwpm.has_pending_seed_cleanup(), imported[c]);
                    for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
                    for (size_t slot = range.begin; slot < range.end; ++slot) {
                        const auto& seed = mwpm.predecoder.seed_view.storage[slot];
                        const auto id = static_cast<structured::RegionId>(1 + range.rank_begin + slot - range.begin);
                        const bool node_deferred = imported[c] && mwpm.flooder.regions->is_seed_node_deferred(id);
                        const auto& n = mwpm.flooder.graph.node(seed.detector_id);
                        EXPECT_EQ(n.region_that_arrived, structured::NO_REGION);
                        EXPECT_EQ(n.top_region(), nullptr);
                        EXPECT_EQ(n.reached_from_source, imported[c] && !node_deferred ? &n : nullptr);
                        if (node_deferred) {
                            EXPECT_NE(seed.mate, structured::PredecodeSeed::UNMATCHED);
                            EXPECT_EQ(n.radius_of_arrival, 0);
                            EXPECT_EQ(n.observables_crossed_from_source, 0U);
                            EXPECT_FALSE(n.node_event_tracker.has_queued_time);
                            EXPECT_FALSE(n.node_event_tracker.has_desired_time);
                            EXPECT_EQ(n.grid_component->collision_states[n.grid_cell].top_region_id, structured::NO_REGION);
                        }
                    }
                }
                decoder.reset_after_shot();
                check_reset(decoder.serial_decoder(), imported);
                const auto after_reset = states(decoder.serial_decoder());
                decoder.reset_after_shot();
                EXPECT_EQ(states(decoder.serial_decoder()), after_reset);
            }
            decoder.prepare_for_mwpm(shots[0]);
            decoder.reset_after_shot();  // Abandoned imported prefix: hard reset, no neutral rebuild.
            for (auto state : states(decoder.serial_decoder()))
                EXPECT_EQ(state, 0U);
            EXPECT_THROW(decoder.prepare_for_mwpm({shots[0][0], shots[0][0]}), std::invalid_argument);
            for (auto state : states(decoder.serial_decoder()))
                EXPECT_EQ(state, 0U);
            decoder.prepare_for_mwpm(shots[0]);
            EXPECT_EQ(decoder.solve_prepared(), expected[0]);
            decoder.reset_after_shot();
        });
    }
    EXPECT_GT(imported_components, 0U);
    ComponentDecoder serial(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder serial_reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    PreparedRegionSnapshot cold;
    for (size_t index : {0, 1, 2, 0}) {
        const auto expected = serial_reference.decode(shots[index]);
        serial.prepare_for_mwpm(shots[index]);
        if (index == 0) {
            const auto prepared = snapshot_imported_regions(serial);
            if (cold.values.empty()) {
                cold = prepared;
            } else {
                EXPECT_EQ(prepared.values, cold.values);
                ASSERT_EQ(prepared.generations.size(), cold.generations.size());
                for (size_t k = 0; k < cold.generations.size(); ++k)
                    EXPECT_GT(prepared.generations[k], cold.generations[k]);
            }
        }
        std::vector<bool> imported;
        for (const auto& component : serial.components())
            imported.push_back(!component.mwpm.predecoder.complete);
        EXPECT_EQ(serial.solve_prepared(), expected);
        serial.reset_after_shot();
        check_reset(serial, imported);
        const auto after_reset = states(serial);
        serial.reset_after_shot();
        EXPECT_EQ(states(serial), after_reset);
    }
    serial.prepare_for_mwpm(shots[0]);
    serial.reset_after_shot();
    for (auto state : states(serial))
        EXPECT_EQ(state, 0U);
}

TEST(SharedComponents, DeferredSeedCleanupImplicitNextMoveAndRepeatedReset) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> shot;
    for (size_t i = 1; i < spec.coordinates.size(); i += 7)
        shot.push_back(i);
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    const auto expected_serial = reference.decode(shot);
    const auto imported_nodes = [](const ComponentDecoder& decoder) {
        std::vector<std::pair<size_t, size_t>> nodes;
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            const auto& mwpm = decoder.components()[c].mwpm;
            if (mwpm.has_pending_seed_cleanup())
                for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
                    for (const auto& seed : mwpm.predecoder.seed_view.storage.subspan(range.begin, range.count()))
                    nodes.emplace_back(c, seed.detector_id);
        }
        EXPECT_FALSE(nodes.empty());
        return nodes;
    };
    const auto poison_after_reset = [](ComponentDecoder& decoder, const auto& nodes) {
        for (const auto& [c, id] : nodes) {
            auto& n = decoder.components()[c].mwpm.flooder.graph.node(id);
            n.observables_crossed_from_source = 123;
            n.set_wrapped_radius(246);
        }
    };
    const auto check_no_second_writes = [](ComponentDecoder& decoder, const auto& nodes) {
        for (const auto& [c, id] : nodes) {
            auto& n = decoder.components()[c].mwpm.flooder.graph.node(id);
            EXPECT_EQ(n.observables_crossed_from_source, 123U);
            EXPECT_EQ(n.grid_component->collision_states[n.grid_cell].wrapped_radius_cached, 246);
            n.reset();  // Restore the deliberate post-reset sentinel.
        }
    };
    ComponentDecoder serial(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    serial.prepare_for_mwpm(shot);
    EXPECT_EQ(serial.solve_prepared(), expected_serial);
    ComponentDecoder moved(std::move(serial));
    EXPECT_EQ(moved.decode(shot), expected_serial);  // Implicit cleanup before normal decode.
    expect_clean_nodes(moved);
    moved.prepare_for_mwpm(shot);
    EXPECT_EQ(moved.solve_prepared(), expected_serial);
    moved.prepare_for_mwpm(shot);  // Implicit cleanup before another split preparation.
    EXPECT_EQ(moved.solve_prepared(), expected_serial);
    const auto serial_nodes = imported_nodes(moved);
    moved.reset_after_shot();
    expect_clean_nodes(moved);
    poison_after_reset(moved, serial_nodes);
    moved.reset_after_shot();
    check_no_second_writes(moved, serial_nodes);

    for (bool parallel_import : {false, true}) for (size_t threads : {1, 2, 4}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 3, parallel_import);
        MatchingResult expected;
        decoder.run_with_team([&] {
            expected = decoder.decode(shot);
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(shot);
            EXPECT_EQ(decoder.solve_prepared(), expected);
            EXPECT_EQ(decoder.decode(shot), expected);
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(shot);
            EXPECT_EQ(decoder.solve_prepared(), expected);
        });
        SharedComponentDecoder transferred(std::move(decoder));
        transferred.run_with_team([&] {
            transferred.prepare_for_mwpm(shot);
            EXPECT_EQ(transferred.solve_prepared(), expected);
            const auto nodes = imported_nodes(transferred.serial_decoder());
            transferred.reset_after_shot();
            expect_clean_nodes(transferred.serial_decoder());
            poison_after_reset(transferred.serial_decoder(), nodes);
            transferred.reset_after_shot();
            check_no_second_writes(transferred.serial_decoder(), nodes);
        });
    }
}

TEST(SharedComponents, DeferredSeedCleanupJoinsSuccessfulAndFailingComponents) {
    BackendUserGraph graph(4, 2);
    graph.add_or_merge_edge(0, 2, {0}, 2, -1);
    graph.add_or_merge_edge(1, 3, {1}, 3, -1);
    graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
    for (bool parallel_import : {false, true}) for (size_t threads : {1, 2, 4}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), 2, 0, 0, 2, parallel_import);
        decoder.run_with_team([&] {
            // C0 can finish deferred extraction while the odd C1 fails. The
            // outer join must finish before recovery resets either graph.
            decoder.prepare_for_mwpm({0, 1});
            EXPECT_THROW(decoder.solve_prepared(), std::invalid_argument);
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm({0, 1, 2, 3});
            EXPECT_EQ(decoder.solve_prepared().weight, 10);
            decoder.reset();
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm({0, 1});
            decoder.reset_after_shot();  // Abandoned prefix never becomes pending.
            expect_clean_nodes(decoder.serial_decoder());
        });
    }
}

TEST(SharedComponents, OwnerCellRangesPublishFinalIndicesAndClearAfterShot) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::mt19937_64 rng(2026101202);
    std::vector<std::vector<uint64_t>> shots(3);
    for (size_t k = 0; k < spec.coordinates.size(); ++k) {
        if (rng() % 7 == 0)
            shots[0].push_back(k);
        if (rng() % 13 == 0)
            shots[1].push_back(k);
    }
    for (size_t parts : {2, 4}) for (size_t width : {2, 3}) {
        SharedComponentDecoder reference(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            {-1}, parts, 0, 0, width, false);
        std::vector<MatchingResult> expected;
        reference.run_with_team([&] {
            for (const auto& shot : shots)
                expected.push_back(reference.decode(shot));
        });
        for (size_t threads : {1, 2, 4}) {
            SharedComponentDecoder decoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                std::vector<int>(threads, -1), parts, 0, 0, width, false);
            decoder.run_with_team([&] {
                for (size_t iteration = 0; iteration < 2; ++iteration) {
                    for (size_t s = 0; s < shots.size(); ++s) {
                        decoder.prepare_for_mwpm(shots[s]);
                        std::vector<structured::PredecodeSeed> prepared;
                        for (const auto& component : decoder.serial_decoder().components()) {
                            const auto& pre = component.mwpm.predecoder;
                            EXPECT_TRUE(pre.has_live_cell_state());
                            const auto scratch = pre.scratch_layout();
                            ASSERT_EQ(scratch.size(), parts + 1);
                            size_t next_seed = 0;
                            for (const auto& range : scratch) {
                                EXPECT_EQ(range.seed_begin, next_seed);
                                next_seed = range.seed_end;
                                EXPECT_EQ(range.mask_address % 64, 0);
                                EXPECT_EQ(range.degree_address % 64, 0);
                                EXPECT_EQ(range.order_address % 64, 0);
                            }
                            EXPECT_EQ(next_seed, pre.seed_view.active_count());
                            for (size_t k = 0; k < pre.seed_view.active_count(); ++k) {
                                const auto& seed = pre.seed_view.storage[k];
                                EXPECT_EQ(seed.grid->predecode.active[seed.cell], k + 1);
                                EXPECT_EQ(seed.grid->predecode.radii[seed.cell], seed.radius);
                                prepared.push_back(seed);
                            }
                        }
#if PM_STRUCTURED_PROFILE_TIMELINE
                        for (size_t c = 0; c < decoder.serial_decoder().components().size(); ++c) {
                            for (size_t b = 0; b <= parts; ++b) {
                                const auto& spans = decoder.partition_timing(c, b).timeline;
#if PM_SHARED_COMPONENT_PARALLELISM == 1
                                // Index publication is fused into the one-shot
                                // conflict/owner scan, before either component
                                // reads that bucket in Degree.
                                EXPECT_FALSE(spans[size_t(structured::PredecodePhase::CellIndex)].present);
                                const auto lane = b == parts ? 0 : b % decoder.active_predecode_threads(c);
                                const auto& index = decoder.profile.lanes[lane].owner_scatter_interval;
#else
                                const auto& index = spans[size_t(structured::PredecodePhase::CellIndex)];
#endif
                                const auto& degree = spans[size_t(structured::PredecodePhase::Degree)];
                                EXPECT_TRUE(index.present);
                                EXPECT_FALSE(index.repeated);
                                EXPECT_LE(index.start_ns, index.end_ns);
                                EXPECT_LE(index.end_ns, degree.start_ns);
                            }
                        }
#endif
                        EXPECT_EQ(decoder.solve_prepared(), expected[s]);
                        decoder.reset_after_shot();
                        for (const auto& seed : prepared) {
                            EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0);
                            EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
                        }
                        for (const auto& component : decoder.serial_decoder().components()) {
                            EXPECT_FALSE(component.mwpm.predecoder.has_live_cell_state());
                            EXPECT_TRUE(component.mwpm.predecoder.seed_view.storage.empty());
                        }
                        decoder.reset_after_shot();
                    }
                }
            });
        }
    }
}

TEST(SharedComponents, SplitLifecycleFallbackAndNegativeNormalizationRemainExact) {
    for (bool negative : {false, true}) {
        BackendUserGraph graph(4, 2);
        graph.add_or_merge_edge(0, 2, {0}, negative ? -2 : 2, -1);
        graph.add_or_merge_edge(1, 3, {1}, 3, -1);
        graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
        graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
        for (size_t route = 0; route < 3; ++route) {
            ComponentDecoder reference(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
            ComponentDecoder serial(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
            SharedComponentDecoder shared(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
                {-1, -1}, route == 0 ? 1 : 2, route == 1 ? 100 : 0, route == 2 ? 100 : 0, 2, false);
            shared.run_with_team([&] {
                for (size_t mask = 0; mask < 16; ++mask) {
                    std::vector<uint64_t> events;
                    for (size_t i = 0; i < 4; ++i)
                        if (mask & (1U << i))
                            events.push_back(i);
                    const auto expected = reference.decode(events);
                    serial.prepare_for_mwpm(events);
                    shared.prepare_for_mwpm(events);
                    EXPECT_EQ(serial.solve_prepared(), expected);
                    EXPECT_EQ(shared.solve_prepared(), expected);
                    serial.reset_after_shot();
                    shared.reset_after_shot();
                }
            });
        }
    }
}
TEST(SharedComponents, SplitLifecyclePublishesBothComponentsBeforeMwpmAndPreservesResults) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder serial_reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder serial(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    SharedComponentDecoder shared_reference(
        ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false), {-1, -1}, 2, 0, 0, 2, false);
    SharedComponentDecoder shared(
        ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false), {-1, -1}, 2, 0, 0, 2, false);
    std::mt19937_64 rng(2026101201);
    std::vector<std::vector<uint64_t>> shots(24);
    for (auto& events : shots)
        for (size_t i = 0; i < graph.nodes.size(); ++i)
            if (rng() % 5 == 0)
                events.push_back(i);
    std::vector<MatchingResult> expected;
    shared_reference.run_with_team([&] {
        for (const auto& events : shots)
            expected.push_back(shared_reference.decode(events));
    });
    size_t incomplete_components = 0;
    shared.run_with_team([&] {
        EXPECT_THROW(shared.solve_prepared(), std::logic_error);
        for (size_t shot = 0; shot < shots.size(); ++shot) {
            const auto& events = shots[shot];
            const auto serial_expected = serial_reference.decode(events);
            serial.prepare_for_mwpm(events);
            shared.prepare_for_mwpm(events);
            for (const auto* decoder : {&serial, &shared.serial_decoder()}) {
                for (const auto& component : decoder->components()) {
                    const auto& mwpm = component.mwpm;
                    if (!mwpm.predecoder.complete) {
                        ++incomplete_components;
                        EXPECT_FALSE(mwpm.flooder.queue.empty());
                        EXPECT_FALSE(mwpm.flooder.regions->empty());
                    }
#if PM_STRUCTURED_PROFILE_TIMERS
                    EXPECT_EQ(mwpm.predecoder.stats.mwpm_ns, 0U);
                    EXPECT_EQ(mwpm.predecoder.stats.extract_ns, 0U);
                    EXPECT_EQ(mwpm.predecoder.stats.reset_ns, 0U);
#endif
                }
            }
            EXPECT_THROW(shared.prepare_for_mwpm(events), std::logic_error);
            EXPECT_THROW(serial.prepare_for_mwpm(events), std::logic_error);
            EXPECT_THROW(serial.decode_partitioned(serial.partition_detection_events(events)), std::logic_error);
            const auto serial_result = serial.solve_prepared();
            const auto shared_result = shared.solve_prepared();
            EXPECT_EQ(serial_result.weight, serial_expected.weight);
            EXPECT_EQ(serial_result.obs_mask, serial_expected.obs_mask);
            EXPECT_EQ(shared_result.weight, expected[shot].weight);
            EXPECT_EQ(shared_result.obs_mask, expected[shot].obs_mask);
#if PM_STRUCTURED_PROFILE_TIMELINE
            for (const auto* decoder : {&serial, &shared.serial_decoder()})
                for (const auto& component : decoder->components()) {
                    const auto& timeline = component.mwpm.predecoder.stats.timeline;
                    const auto& mwpm = timeline[static_cast<size_t>(structured::PredecodePhase::Mwpm)];
                    const auto end = decoder == &serial ? serial.profile.prefix_interval.end_ns
                                                        : shared.profile.prefix_interval.end_ns;
                    if (mwpm.present)
                        EXPECT_GE(mwpm.start_ns, end);
                }
            for (size_t c = 0; c < 2; ++c) {
                const auto& p = shared.profile.component[c];
                for (size_t lane = 0; lane < shared.active_predecode_threads(c); ++lane) {
                    EXPECT_TRUE(p.lanes[lane].worker_interval.present);
                    EXPECT_FALSE(p.lanes[lane].worker_interval.repeated);
                }
            }
#endif
            shared.reset_after_shot();
            serial.reset_after_shot();
            EXPECT_TRUE(serial.shot_seeds.empty());
            EXPECT_TRUE(shared.serial_decoder().shot_seeds.empty());
            EXPECT_THROW(shared.solve_prepared(), std::logic_error);
            EXPECT_THROW(serial.solve_prepared(), std::logic_error);
        }
        // A prefix may be abandoned safely; the next shot starts from idle.
        shared.prepare_for_mwpm(shots.front());
        serial.prepare_for_mwpm(shots.front());
        shared.reset_after_shot();
        serial.reset_after_shot();
        shared.prepare_for_mwpm(shots.front());
        serial.prepare_for_mwpm(shots.front());
        EXPECT_EQ(shared.solve_prepared().weight, expected.front().weight);
        EXPECT_EQ(serial.solve_prepared().weight, expected.front().weight);
        shared.reset_after_shot();
        serial.reset_after_shot();
        EXPECT_THROW(shared.prepare_for_mwpm({0, 0}), std::invalid_argument);
        EXPECT_THROW(serial.prepare_for_mwpm({0, 0}), std::invalid_argument);
        EXPECT_EQ(shared.decode({}).weight, 0);
        EXPECT_EQ(serial.decode({}).weight, 0);
    });
    EXPECT_GT(incomplete_components, 0U);
}
#endif
#if defined(_OPENMP) && PM_PREDECODE_ENABLED
#if PM_STRUCTURED_PROFILE_TIMELINE
TEST(SharedComponents, TimelineContainsConcurrentWorkersAndNestedPhases) {
    auto graph = pair_graph();
    SharedComponentDecoder decoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
                                   {-1, -1, -1, -1}, 2, 0, 0, 2, false);
    decoder.run_with_team([&] {
        decoder.decode({0, 1, 2, 3});
        const auto& p = decoder.profile;
        ASSERT_TRUE(p.total_interval.present);
        EXPECT_EQ(p.total_interval.end_ns - p.total_interval.start_ns, p.total_ns);
        auto contained = [&](const structured::PhaseInterval& outer,
                             const structured::PhaseInterval& inner) {
            ASSERT_TRUE(inner.present);
            EXPECT_FALSE(inner.repeated);
            EXPECT_LE(outer.start_ns, inner.start_ns);
            EXPECT_LE(inner.start_ns, inner.end_ns);
            EXPECT_LE(inner.end_ns, outer.end_ns);
        };
        contained(p.total_interval, p.partition_interval);
#if PM_SHARED_COMPONENT_PARALLELISM == 1
        EXPECT_FALSE(p.dispatch_interval.present);
        EXPECT_FALSE(p.join_interval.present);
#else
        contained(p.total_interval, p.dispatch_interval);
        contained(p.total_interval, p.join_interval);
#endif
        contained(p.total_interval, p.merge_interval);
        for (size_t c = 0; c < 2; ++c) {
            const auto& component = p.component[c];
#if !PM_STRUCTURED_PROFILE_COUNTERS
            EXPECT_EQ(component.seeds, 0);
            EXPECT_EQ(component.conflict_seeds, 0);
#endif
            contained(p.total_interval, component.total_interval);
#if PM_SHARED_COMPONENT_PARALLELISM == 1
            EXPECT_FALSE(component.prepare_interval.present);
#else
            contained(component.total_interval, component.prepare_interval);
#endif
            contained(component.total_interval, component.predecode_interval);
            contained(component.predecode_interval, component.join_interval);
            contained(component.predecode_interval, component.conflict_interval);
            contained(component.total_interval, component.continuation_interval);
            contained(component.total_interval, component.clear_interval);
            EXPECT_LE(component.predecode_interval.end_ns, component.continuation_interval.start_ns);
            EXPECT_LE(component.continuation_interval.end_ns, component.clear_interval.start_ns);
            EXPECT_EQ(component.total_interval.end_ns - component.total_interval.start_ns, component.total_ns);
#if PM_SHARED_COMPONENT_PARALLELISM == 2
            for (size_t lane = 0; lane < 2; ++lane)
                contained(component.predecode_interval, p.lanes[2 * c + lane].worker_interval);
#endif
        }
#if PM_SHARED_COMPONENT_PARALLELISM == 1
        EXPECT_LE(p.component[0].total_interval.end_ns, p.component[1].total_interval.start_ns);
        for (size_t lane = 0; lane < 2; ++lane) {
            const auto& worker = p.lanes[lane].worker_interval;
            EXPECT_TRUE(worker.present);
            EXPECT_TRUE(worker.repeated);
            EXPECT_LE(p.total_interval.start_ns, worker.start_ns);
            EXPECT_LE(worker.end_ns, p.total_interval.end_ns);
        }
#endif
        decoder.decode({});
        EXPECT_FALSE(decoder.profile.component[0].import_interval.present);
        EXPECT_FALSE(decoder.profile.component[1].import_interval.present);
    });
}
#endif
TEST(SharedComponents, FixedAlgorithmIsDeterministicAcrossThreadsAndMatchesGenericWeight) {
    for (bool rotated : {false,true}) for(char basis : {'x','z'}) {
        auto [dem,spec]=model(rotated,9,basis);
        auto graph=detector_error_model_to_backend_user_graph(dem,false,NUM_DISTINCT_WEIGHTS);
        auto oracle=detector_error_model_to_mwpm(dem,NUM_DISTINCT_WEIGHTS,true);
        std::mt19937_64 rng(20261011);
        std::vector<std::vector<uint64_t>> inputs(96);
        std::vector<MatchingResult> expected(inputs.size());
        for(size_t shot=0;shot<inputs.size();++shot) {
            for(size_t n=0;n<spec.coordinates.size();++n)
                if(rng()%(shot%3==0?9:60)==0) inputs[shot].push_back(n);
            expected[shot]=decode_detection_events_for_up_to_64_observables(oracle,inputs[shot],false);
        }
        for(size_t parts:{2,4}) for(size_t width:{2,3}) {
            std::vector<MatchingResult> one(inputs.size());
            for(size_t threads:{1,2,4,8}) {
                SharedComponentDecoder decoder(ComponentDecoder(graph,spec,NUM_DISTINCT_WEIGHTS,false),
                    std::vector<int>(threads,-1),parts,0,0,width,true);
                decoder.run_with_team([&] {
                    ASSERT_EQ(decoder.team_size(),threads);
                    EXPECT_EQ(decoder.thread_snapshots()[0].component,
#if PM_SHARED_COMPONENT_PARALLELISM == 1
                        structured::SharedThreadSnapshot::SHARED_COMPONENT);
#else
                        threads == 1 && decoder.serial_decoder().components().size() == 2
                            ? structured::SharedThreadSnapshot::SHARED_COMPONENT : size_t{0});
#endif
                    for(size_t repeat=0;repeat<2;++repeat) for(size_t shot=0;shot<inputs.size();++shot) {
                        SCOPED_TRACE(::testing::Message()<<rotated<<basis<<" P"<<parts<<" W"<<width<<" T"<<threads<<" shot"<<shot);
                        const auto result=decoder.decode(inputs[shot]);
                        ASSERT_EQ(result.weight,expected[shot].weight);
                        if(threads==1 && repeat==0) one[shot]=result;
                        ASSERT_EQ(result,one[shot]);
                        if(shot%16==0) {
                            std::set<uint64_t> parity;
                            obs_int obs=0;
                            for(const auto& match:decoder.decode_to_matches(inputs[shot])) {
                                for(auto id:{match.u,match.v}) if(id>=0) {
                                    if(!parity.erase(id)) parity.insert(id);
                                }
                                obs^=match.obs_mask;
                            }
                            ASSERT_EQ(parity,std::set<uint64_t>(inputs[shot].begin(),inputs[shot].end()));
                            ASSERT_EQ(obs,result.obs_mask);
                        }
                    }
                });
            }
        }
    }
}
TEST(SharedComponents, ReentrantFailureRecoveryAndMoveLifecycle) {
    auto graph=pair_graph();
    SharedComponentDecoder decoder(ComponentDecoder(graph,pair_spec(),NUM_DISTINCT_WEIGHTS,false),{-1,-1,-1,-1},2,0,0,3,true);
    EXPECT_THROW(decoder.decode({0}),std::logic_error);
    decoder.run_with_team([&] {
        EXPECT_THROW(decoder.run_with_team([]{}),std::logic_error);
        EXPECT_THROW(SharedComponentDecoder moved(std::move(decoder)),std::logic_error);
        for(int i=0;i<4;++i) {
            EXPECT_THROW(decoder.decode({4}),std::invalid_argument);
            EXPECT_THROW(decoder.decode({0,0}),std::invalid_argument);
            EXPECT_EQ(decoder.decode({0,2}).weight,4);
            EXPECT_EQ(decoder.decode({}).weight,0);
            decoder.reset();
        }
    });
    SharedComponentDecoder moved(std::move(decoder));
    EXPECT_THROW(decoder.run_with_team([]{}),std::logic_error);
    moved.run_with_team([&]{EXPECT_EQ(moved.decode({1,3}).weight,6);});
    EXPECT_THROW(moved.run_with_team([]{throw std::runtime_error("callback");}),std::runtime_error);
    moved.run_with_team([&]{EXPECT_EQ(moved.decode({0,2}).weight,4);});
}
TEST(SharedComponents, SerialFallbackPrecedesSpatialPreparation) {
    auto graph=pair_graph();
    SharedComponentDecoder decoder(ComponentDecoder(graph,pair_spec(),NUM_DISTINCT_WEIGHTS,false),{-1,-1,-1,-1},2,3,2,3,true);
    decoder.run_with_team([&] {
        decoder.decode({0});
        EXPECT_EQ(decoder.component_route(0),structured::SharedRoute::whole_serial);
        EXPECT_EQ(decoder.component_route(1),structured::SharedRoute::whole_serial);
#if PM_STRUCTURED_PROFILE_TIMERS
        EXPECT_EQ(decoder.profile.component[0].continuation_worker, 0U);
        EXPECT_EQ(decoder.profile.component[1].continuation_worker, 0U);
#endif
        decoder.decode({0,2});
        EXPECT_EQ(decoder.component_route(0),structured::SharedRoute::serial);
        EXPECT_EQ(decoder.component_route(1),structured::SharedRoute::serial);
#if PM_STRUCTURED_PROFILE_TIMERS
        EXPECT_EQ(decoder.profile.component[0].continuation_worker, 0U);
        EXPECT_EQ(decoder.profile.component[1].continuation_worker, PM_SHARED_COMPONENT_PARALLELISM == 1 ? 0U : 2U);
#endif
    });
}
TEST(SharedComponents, SignedGraphCorrectionMatchesExactSerialResult) {
    auto graph=pair_graph();
    graph.add_or_merge_boundary_edge(2,{0},-3,-1);
    for(size_t threads:{1,2,4,8}) {
        ComponentDecoder reference(graph,pair_spec(),NUM_DISTINCT_WEIGHTS,false);
        SharedComponentDecoder decoder(ComponentDecoder(graph,pair_spec(),NUM_DISTINCT_WEIGHTS,false),
            std::vector<int>(threads,-1),2,0,0,3,true);
        decoder.run_with_team([&] {
            for(size_t repeat=0;repeat<3;++repeat) for(size_t mask=0;mask<16;++mask) {
                std::vector<uint64_t> input;
                for(size_t k=0;k<4;++k) if(mask&(1U<<k))input.push_back(k);
                EXPECT_EQ(decoder.decode(input),reference.decode(input));
            }
        });
    }
}
TEST(SharedComponents, FourPartitionsExerciseWideGridInteriorsAndSimdFootprints) {
    auto [dem,spec]=model(true,25,'z');
    if(grid_avx512vl_available()) spec.simd=GridSimd::avx512vl;
    auto graph=detector_error_model_to_backend_user_graph(dem,false,NUM_DISTINCT_WEIGHTS);
    auto oracle=detector_error_model_to_mwpm(dem,NUM_DISTINCT_WEIGHTS,true);
    std::mt19937_64 rng(2026101102);
    std::vector<std::vector<uint64_t>> inputs(16);
    std::vector<MatchingResult> expected, one(16);
    for(auto& input:inputs) {
        for(size_t n=0;n<spec.coordinates.size();++n) if(rng()%70==0)input.push_back(n);
        expected.push_back(decode_detection_events_for_up_to_64_observables(oracle,input,false));
    }
    for(const auto& delta:SURFACE_OFFSETS) EXPECT_LE(std::abs(delta[2]),1);
    for(size_t threads:{1,2,4,8}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph,spec,NUM_DISTINCT_WEIGHTS,false),
            std::vector<int>(threads,-1),4,0,0,2,true);
        decoder.run_with_team([&] {
            for(size_t repeat=0;repeat<4;++repeat)for(size_t k=0;k<inputs.size();++k) {
                auto result=decoder.decode(inputs[k]);
                EXPECT_EQ(result.weight,expected[k].weight);
                if(threads==1&&repeat==0)one[k]=result;
                EXPECT_EQ(result,one[k]);
            }
        });
    }
}

TEST(SharedComponents, BulkImportMatchesSerialImportAndPreservesQueueAccounting) {
    auto [dem,spec]=model(true,9,'z');
    auto graph=detector_error_model_to_backend_user_graph(dem,false,NUM_DISTINCT_WEIGHTS);
    std::mt19937_64 rng(2026101103);
    std::vector<std::vector<uint64_t>> inputs(24);
    std::vector<MatchingResult> expected(inputs.size());
    for(auto& input:inputs)
        for(size_t n=0;n<spec.coordinates.size();++n)
            if(rng()%40==0) input.push_back(n);
    for(bool parallel_import:{false,true}) for(size_t threads:{1,4,8}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph,spec,NUM_DISTINCT_WEIGHTS,false),
            std::vector<int>(threads,-1),4,0,0,2,parallel_import);
        size_t imported=0;
        decoder.run_with_team([&] {
            for(size_t repeat=0;repeat<2;++repeat) for(size_t i=0;i<inputs.size();++i) {
                SCOPED_TRACE(::testing::Message()<<"import "<<parallel_import<<" T"<<threads<<" shot"<<i);
                const auto actual=decoder.decode(inputs[i]);
                if(!parallel_import&&threads==1&&repeat==0) expected[i]=actual;
                ASSERT_EQ(actual,expected[i]);
                for(size_t c=0;c<decoder.serial_decoder().components().size();++c) {
                    const auto& mwpm=decoder.serial_decoder().components()[c].mwpm;
                    EXPECT_TRUE(mwpm.flooder.queue.empty());
                    EXPECT_TRUE(mwpm.flooder.regions->empty());
                    if(!mwpm.predecoder.complete) {
                        ++imported;
#if PM_STRUCTURED_PROFILE_COUNTERS
                        EXPECT_EQ(mwpm.predecoder.stats.import_regions,mwpm.predecoder.seed_view.active_count());
                        EXPECT_GT(mwpm.flooder.queue.profile.pushes,0U);
#endif
#if PM_STRUCTURED_PROFILE_TIMERS
                        EXPECT_GT(decoder.profile.component[c].import_ns,0U);
                        EXPECT_GE(decoder.profile.component[c].import_ns,
                                  decoder.profile.component[c].import_join_ns);
#endif
                    }
#if PM_STRUCTURED_PROFILE_COUNTERS
                    const auto& q=mwpm.flooder.queue.profile;
                    EXPECT_EQ(q.pushes,q.pops+q.cleared+mwpm.flooder.queue.size());
                    EXPECT_EQ(q.pops,q.valid+q.stale_superseded+q.stale_cancelled+q.deferred_requeues);
#endif
                }
                std::set<uint64_t> parity;
                obs_int obs=0;
                for(const auto& match:decoder.decode_to_matches(inputs[i])) {
                    for(auto id:{match.u,match.v}) if(id>=0) {
                        if(!parity.erase(id)) parity.insert(id);
                    }
                    obs^=match.obs_mask;
                }
                EXPECT_EQ(parity,std::set<uint64_t>(inputs[i].begin(),inputs[i].end()));
                EXPECT_EQ(obs,actual.obs_mask);
#if PM_STRUCTURED_PROFILE_TIMERS
                EXPECT_GT(decoder.profile.total_ns,0U);
                EXPECT_GE(decoder.profile.total_ns,decoder.profile.merge_ns);
#endif
            }
        });
        EXPECT_GT(imported,0U);
    }
}
TEST(SharedComponents, ImportedContinuationFailureJoinsGroupsAndRecovers) {
    BackendUserGraph graph(4,2);
    graph.add_or_merge_edge(0,2,{0},2,-1);
    graph.add_or_merge_edge(1,3,{1},3,-1);
    for(bool parallel_import:{false,true}) for(size_t threads:{1,4,8}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph,pair_spec(),NUM_DISTINCT_WEIGHTS,false),
            std::vector<int>(threads,-1),2,0,0,2,parallel_import);
        decoder.run_with_team([&] {
            for(size_t repeat=0;repeat<4;++repeat) {
                // Both components have odd parity, enter import, and then fail
                // MWPM. Recovery must wait for both captains and their helpers.
                EXPECT_THROW(decoder.decode({0,1}),std::invalid_argument);
                for(size_t c=0;c<decoder.serial_decoder().components().size();++c) {
                    const auto& mwpm=decoder.serial_decoder().components()[c].mwpm;
                    EXPECT_TRUE(mwpm.flooder.regions->empty());
                    EXPECT_TRUE(mwpm.flooder.queue.empty());
                    EXPECT_EQ(mwpm.node_arena.allocated.size(),mwpm.node_arena.available.size());
#if PM_STRUCTURED_PROFILE_TIMERS
                    EXPECT_GT(decoder.profile.component[c].import_ns,0U);
                    EXPECT_GE(decoder.profile.component[c].total_ns,decoder.profile.component[c].import_ns);
#endif
                }
                EXPECT_EQ(decoder.decode({0,1,2,3}).weight,10);
                EXPECT_EQ(decoder.decode({}).weight,0);
            }
        });
    }
}
#if PM_SHARED_COMPONENT_PARALLELISM == 1
TEST(SharedComponents, SequentialGroupTwoLanesMatchSameAlgorithmAndRetainFallbacks) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder serial(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<std::vector<uint64_t>> inputs{{}};
    const auto& components = serial.components();
    ASSERT_EQ(components.size(), 2);
    std::vector<uint64_t> small, asymmetric, dense;
    for (size_t c = 0; c < 2; ++c) {
        ASSERT_GE(components[c].global_detectors.size(), 96);
        small.push_back(components[c].global_detectors[0]);
        for (size_t i = 0; i < 96; ++i) {
            dense.push_back(components[c].global_detectors[i]);
            if (c == 0 || i < 4)
                asymmetric.push_back(components[c].global_detectors[i]);
        }
    }
    inputs.push_back(small);
    inputs.push_back(asymmetric);
    inputs.push_back(dense);
    std::vector<MatchingResult> expected(inputs.size());
    SharedComponentDecoder one(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                               {-1}, 2, 64, 32, 2, false);
    one.run_with_team([&] {
        for (size_t k = 0; k < inputs.size(); ++k) {
            expected[k] = one.decode(inputs[k]);
            EXPECT_EQ(expected[k].weight, serial.decode(inputs[k]).weight);
        }
    });
    SharedComponentDecoder two(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                               {-1, -1}, 2, 64, 32, 2, false);
    two.run_with_team([&] {
        ASSERT_EQ(two.team_size(), 2);
        const auto ids = two.thread_snapshots();
        ASSERT_EQ(ids.size(), 2);
        EXPECT_NE(ids[0].thread_id, ids[1].thread_id);
        for (size_t lane = 0; lane < 2; ++lane) {
            EXPECT_EQ(ids[lane].component, structured::SharedThreadSnapshot::SHARED_COMPONENT);
            EXPECT_EQ(ids[lane].lane, lane);
        }
        for (size_t repeat = 0; repeat < 8; ++repeat) {
            for (size_t k = 0; k < inputs.size(); ++k) {
                EXPECT_EQ(two.decode(inputs[k]), expected[k]);
                EXPECT_EQ(two.thread_snapshots()[0].thread_id, ids[0].thread_id);
                EXPECT_EQ(two.thread_snapshots()[1].thread_id, ids[1].thread_id);
                if (k < 2) {
                    EXPECT_EQ(two.component_route(0), structured::SharedRoute::whole_serial);
                    EXPECT_EQ(two.component_route(1), structured::SharedRoute::whole_serial);
                } else {
                    EXPECT_EQ(two.component_route(0), structured::SharedRoute::partitioned);
                    EXPECT_EQ(two.active_predecode_threads(0), 2);
                    EXPECT_EQ(two.active_predecode_threads(1), k == 2 ? 1 : 2);
                    EXPECT_EQ(two.component_route(1), k == 2 ? structured::SharedRoute::serial : structured::SharedRoute::partitioned);
                }
#if PM_STRUCTURED_PROFILE_TIMELINE
                const auto& timing = two.profile;
                EXPECT_LE(timing.component[0].total_interval.end_ns, timing.component[1].total_interval.start_ns);
                if (k == 3) {
                    EXPECT_TRUE(timing.lanes[1].worker_interval.present);
                    EXPECT_TRUE(timing.lanes[1].worker_interval.repeated);
                }
#endif
            }
            two.reset();
            EXPECT_TRUE(two.serial_decoder().shot_seeds.empty());
        }
    });
}

TEST(SharedComponents, SequentialGroupJoinsAfterContinuationFailureAndReset) {
    BackendUserGraph graph(4, 2);
    graph.add_or_merge_edge(0, 2, {0}, 2, -1);
    graph.add_or_merge_edge(1, 3, {1}, 3, -1);
    for (bool parallel_import : {false, true}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
                                       {-1, -1}, 2, 0, 0, 2, parallel_import);
        decoder.run_with_team([&] {
            for (size_t repeat = 0; repeat < 8; ++repeat) {
                EXPECT_THROW(decoder.decode({0, 1}), std::invalid_argument);
                for (const auto& component : decoder.serial_decoder().components()) {
                    EXPECT_TRUE(component.mwpm.flooder.regions->empty());
                    EXPECT_TRUE(component.mwpm.flooder.queue.empty());
                    EXPECT_EQ(component.mwpm.node_arena.allocated.size(), component.mwpm.node_arena.available.size());
                }
                EXPECT_EQ(decoder.decode({0, 1, 2, 3}).weight, 10);
                EXPECT_EQ(decoder.active_predecode_threads(0), 2);
                EXPECT_EQ(decoder.active_predecode_threads(1), 2);
                EXPECT_EQ(decoder.decode({}).weight, 0);
                decoder.reset();
                EXPECT_TRUE(decoder.serial_decoder().shot_seeds.empty());
            }
        });
    }
}
#endif
#endif



namespace {
size_t reference_time_bucket(size_t cell, size_t plane, size_t nt, size_t partitions, size_t width) {
    const size_t t = cell / plane;
    size_t owner = 0;
    for (size_t k = 1; k < partitions; ++k) {
        const size_t cut = 1 + (nt - 2) * k / partitions;
        if (t < cut ? cut - t <= width : t - cut < width)
            return partitions;
        owner += t >= cut;
    }
    return owner;
}
}
TEST(TimePartitionCuts, MatchesOriginalTimeFormulaForEverySmallGridCell) {
    for (size_t plane : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{16}})
        for (size_t nt = 2; nt <= 40; ++nt)
            for (size_t parts : {size_t{1}, size_t{2}, size_t{4}, size_t{8}})
                for (size_t width = 2; width <= nt + 3; ++width) {
                    const structured::TimePartitionCuts cuts(plane, nt, parts, width);
                    for (size_t cell = 0; cell < plane * nt; ++cell)
                        ASSERT_EQ(cuts.bucket(cell), reference_time_bucket(cell, plane, nt, parts, width))
                            << "plane=" << plane << " nt=" << nt << " P=" << parts
                            << " W=" << width << " cell=" << cell;
                }
}
TEST(TimePartitionCuts, GuardEndpointsUseWholePlanesAndHalfOpenUpperEdge) {
    // nt=18, P=2 puts the separating plane at t=9. W=2 includes t=7..10.
    const structured::TimePartitionCuts cuts(7, 18, 2, 2);
    EXPECT_EQ(cuts.bucket(7 * 7 - 1), 0);
    EXPECT_EQ(cuts.bucket(7 * 7), 2);
    EXPECT_EQ(cuts.bucket(11 * 7 - 1), 2);
    EXPECT_EQ(cuts.bucket(11 * 7), 1);
    // All three cuts coincide on t=1 when there are only the two halo planes.
    const structured::TimePartitionCuts overlap(3, 2, 4, 2);
    for (size_t cell = 0; cell < 6; ++cell)
        EXPECT_EQ(overlap.bucket(cell), 4);
}
TEST(TimePartitionCuts, SaturatesWideGuardsAndSupportsLargestValidVolume) {
    const auto max = std::numeric_limits<size_t>::max();
    for (size_t nt : {size_t{2}, size_t{3}, size_t{17}, size_t{40}})
        for (size_t parts : {size_t{1}, size_t{2}, size_t{4}, size_t{8}})
            for (size_t width : {max / 2, max - 1, max}) {
                const structured::TimePartitionCuts cuts(7, nt, parts, width);
                for (size_t cell = 0; cell < 7 * nt; ++cell)
                    ASSERT_EQ(cuts.bucket(cell), reference_time_bucket(cell, 7, nt, parts, width));
            }
    const structured::TimePartitionCuts largest(1, max, 4, 2);
    EXPECT_EQ(largest.bucket(0), 0);
    EXPECT_EQ(largest.bucket(max - 1), 3);
    const structured::TimePartitionCuts covered(1, max, 4, max);
    EXPECT_EQ(covered.bucket(0), 4);
    EXPECT_EQ(covered.bucket(max - 1), 4);
}
TEST(TimePartitionCuts, RejectsInvalidGeometryBeforeMultiplyingCellCounts) {
    EXPECT_THROW((structured::TimePartitionCuts(0, 10, 2, 2)), std::invalid_argument);
    EXPECT_THROW((structured::TimePartitionCuts(1, 1, 2, 2)), std::invalid_argument);
    EXPECT_THROW((structured::TimePartitionCuts(1, 10, 3, 2)), std::invalid_argument);
    EXPECT_THROW((structured::TimePartitionCuts(1, 10, 2, 1)), std::invalid_argument);
    EXPECT_THROW((structured::TimePartitionCuts(std::numeric_limits<size_t>::max(), 2, 2, 2)), std::overflow_error);
}

TEST(TimePartitionCuts, ThreePlanesMatchClippedUnionAndSeparateReadWriteFootprints) {
    using Cuts = structured::TimePartitionCuts;
    for (size_t nt = 2; nt <= 129; ++nt) for (size_t parts : {1, 2, 4, 8}) {
        const Cuts cuts(7, nt, parts, 0, Cuts::Geometry::three_planes);
        std::array<std::set<int>, 8> writes, reads;
        for (size_t t = 0; t < nt; ++t) {
            size_t expected = 0;
            for (size_t k = 1; k < parts; ++k) {
                const auto q = 1 + (nt - 2) * k / parts;
                if (t + 1 >= q && t < q + 2) { expected = parts; break; }
                expected += t >= q;
            }
            for (size_t cell = t * 7; cell < (t + 1) * 7; ++cell) ASSERT_EQ(cuts.bucket(cell), expected);
            if (t && t + 1 < nt && expected < parts) {
                for (int d = -1; d <= 1; ++d) writes[expected].insert(int(t) + d);
                for (int d = -2; d <= 2; ++d) reads[expected].insert(int(t) + d);
            }
        }
        for (size_t a = 0; a < parts; ++a) for (size_t b = 0; b < parts; ++b) if (a != b)
            for (auto t : writes[a]) {
                ASSERT_FALSE(writes[b].contains(t));
                ASSERT_FALSE(reads[b].contains(t));
            }
    }
    const auto max = std::numeric_limits<size_t>::max();
    const Cuts largest(1, max, 4, 0, Cuts::Geometry::three_planes);
    EXPECT_EQ(largest.bucket(0), 0U);
    EXPECT_EQ(largest.bucket(max - 1), 3U);
    EXPECT_THROW((Cuts(1, 10, 2, 3, Cuts::Geometry::three_planes)), std::invalid_argument);
    EXPECT_THROW((Cuts(1, 10, 2, 0, Cuts::Geometry::symmetric_width)), std::invalid_argument);
    EXPECT_THROW((Cuts(1, 10, 2, 0, static_cast<Cuts::Geometry>(255))), std::invalid_argument);
    EXPECT_THROW((Cuts(max, 2, 2, 0, Cuts::Geometry::three_planes)), std::overflow_error);
}

TEST(TimePartitionCuts, GridConstructorPreservesLegacyTimeBuckets) {
    using Cuts = structured::TimePartitionCuts;
    for (size_t nx : {1, 3, 7}) for (size_t ny : {2, 5, 17})
    for (size_t nt = 2; nt <= 40; ++nt) for (size_t parts : {1, 2, 4, 8})
    for (auto geometry : {Cuts::Geometry::symmetric_width, Cuts::Geometry::three_planes}) {
        const size_t width = geometry == Cuts::Geometry::three_planes ? 0 : 3;
        const Cuts old(nx * ny, nt, parts, width, geometry);
        const Cuts grid(nx, ny, nt, parts, width, geometry);
        for (size_t cell = 0; cell < nx * ny * nt; ++cell)
            ASSERT_EQ(grid.bucket(cell), old.bucket(cell));
    }
}

TEST(TimePartitionCuts, Ty2x2UnionSeparatesReadWriteFootprintsIncludingShortAxes) {
    using Cuts = structured::TimePartitionCuts;
    for (size_t ny = 2; ny <= 18; ++ny) for (size_t nt = 2; nt <= 18; ++nt) {
        const size_t y_cut = 1 + (ny - 2) / 2, t_cut = 1 + (nt - 2) / 2;
        std::array<std::set<std::array<int, 2>>, 4> writes, reads;
        for (size_t nx : {1, 3, 7}) {
            const Cuts cuts(nx, ny, nt, 4, 0, Cuts::Geometry::ty_2x2);
            Cuts copied(cuts);
            Cuts moved(std::move(copied));
            for (size_t t = 0; t < nt; ++t) for (size_t y = 0; y < ny; ++y) {
                const bool conflict = (t + 1 >= t_cut && t < t_cut + 2)
                    || (y + 1 >= y_cut && y < y_cut + 2);
                const size_t expected = conflict ? 4 : 2 * (t >= t_cut) + (y >= y_cut);
                for (size_t x = 0; x < nx; ++x) {
                    const size_t cell = x + nx * (y + ny * t);
                    ASSERT_EQ(cuts.bucket(cell), expected);
                    ASSERT_EQ(moved.bucket(cell), expected);
                }
                if (nx != 1 || expected == 4 || !t || t + 1 == nt || !y || y + 1 == ny) continue;
                // Conservative Chebyshev supersets include all actual stencil
                // reads/writes, including a neighbor's own radius scan.
                for (int dt = -1; dt <= 1; ++dt) for (int dy = -1; dy <= 1; ++dy)
                    writes[expected].insert({int(t) + dt, int(y) + dy});
                for (int dt = -2; dt <= 2; ++dt) for (int dy = -2; dy <= 2; ++dy)
                    reads[expected].insert({int(t) + dt, int(y) + dy});
            }
        }
        for (size_t a = 0; a < 4; ++a) for (size_t b = 0; b < 4; ++b) if (a != b)
            for (const auto& cell : writes[a]) {
                ASSERT_FALSE(writes[b].contains(cell));
                ASSERT_FALSE(reads[b].contains(cell));
            }
    }
}

TEST(TimePartitionCuts, Ty2x2RejectsInvalidDimensionsConfigurationAndVolume) {
    using Cuts = structured::TimePartitionCuts;
    constexpr auto geometry = Cuts::Geometry::ty_2x2;
    EXPECT_THROW((Cuts(7, 18, 4, 0, geometry)), std::invalid_argument);  // Missing nx/ny.
    EXPECT_THROW((Cuts(0, 10, 10, 4, 0, geometry)), std::invalid_argument);
    EXPECT_THROW((Cuts(7, 1, 10, 4, 0, geometry)), std::invalid_argument);
    EXPECT_THROW((Cuts(7, 10, 1, 4, 0, geometry)), std::invalid_argument);
    EXPECT_THROW((Cuts(7, 10, 10, 2, 0, geometry)), std::invalid_argument);
    EXPECT_THROW((Cuts(7, 10, 10, 4, 3, geometry)), std::invalid_argument);
    const auto max = std::numeric_limits<size_t>::max();
    EXPECT_THROW((Cuts(max, 2, 2, 4, 0, geometry)), std::overflow_error);
    EXPECT_THROW((Cuts(1, max / 2 + 1, 2, 4, 0, geometry)), std::overflow_error);
}

TEST(SharedComponents, GroupCacheGeometryRebindsAtomicallyAndSurvivesMoves) {
    using Access = structured::ComponentDecoderTestAccess;
    using Geometry = structured::TimePartitionCuts::Geometry;
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder decoder(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    for (auto geometry : {Geometry::three_planes, Geometry::symmetric_width, Geometry::three_planes})
    for (size_t parts : {2, 4, 8}) {
        const size_t width = geometry == Geometry::three_planes ? 0 : 3;
        Access::bind(decoder, parts, width, geometry);
        EXPECT_EQ(Access::geometry(decoder), geometry);
        const auto addresses = Access::addresses(decoder);
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            const auto& grid = *decoder.components()[c].seed_grid;
            const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, parts, width, geometry);
            for (auto global : decoder.components()[c].global_detectors)
                EXPECT_EQ(addresses[global][1], cuts.bucket(addresses[global][3]));
        }
        EXPECT_THROW(Access::bind(decoder, parts, 3, Geometry::three_planes), std::invalid_argument);
        EXPECT_EQ(Access::addresses(decoder), addresses);
        EXPECT_EQ(Access::geometry(decoder), geometry);
        ComponentDecoder moved(std::move(decoder));
        EXPECT_EQ(Access::addresses(moved), addresses);
        EXPECT_EQ(Access::geometry(moved), geometry);
        decoder = std::move(moved);
    }
}

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(SharedComponents, OwnerClearPreservesShotDataAcrossRoutesErrorsAndMoves) {
    auto graph = pair_graph();
    struct Config { size_t threads, parts, component_threshold, shot_threshold; };
    for (const auto config : {Config{1, 1, 0, 0}, Config{4, 1, 0, 0}, Config{4, 2, 0, 0},
                              Config{4, 2, 100, 0}, Config{4, 2, 0, 100}}) {
        SharedComponentDecoder decoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(config.threads, -1), config.parts,
            config.component_threshold, config.shot_threshold, 2, true);
        auto check = [&](SharedComponentDecoder& current) {
            size_t seeds = 0;
            for (size_t c = 0; c < current.serial_decoder().components().size(); ++c) {
                const auto& mwpm = current.serial_decoder().components()[c].mwpm;
                const auto& pre = mwpm.predecoder;
                seeds += pre.seed_view.active_count();
                EXPECT_TRUE(pre.complete);
                EXPECT_FALSE(pre.result_pending);
                for (const auto& range : pre.seed_view.active_ranges())
        for (const auto& seed : pre.seed_view.storage.subspan(range.begin, range.count())) {
                    EXPECT_NE(seed.mate, structured::PredecodeSeed::UNMATCHED);
                    EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0);
                    EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
                }
#if PM_STRUCTURED_PROFILE_COUNTERS
                EXPECT_EQ(pre.stats.normalized_seeds, pre.seed_view.active_count());
#endif
#if PM_STRUCTURED_PROFILE_TIMERS
                const auto& p = current.profile.component[c];
                EXPECT_GT(p.clear_ns, 0);
                EXPECT_GE(p.total_ns, p.clear_ns + p.continuation_ns);
                EXPECT_GE(current.profile.total_ns, p.total_ns);
#endif
            }
            EXPECT_EQ(seeds, 4);
            EXPECT_EQ(current.serial_decoder().shot_seeds.size(), 4);
        };
        decoder.run_with_team([&] {
            for (size_t repeat = 0; repeat < 3; ++repeat) {
                EXPECT_EQ(decoder.decode({0, 1, 2, 3}).weight, 10);
                check(decoder);
                const auto matches = decoder.decode_to_matches({0, 1, 2, 3});
                check(decoder);
                EXPECT_EQ(matches.size(), 2);
                EXPECT_THROW(decoder.decode({0, 0}), std::invalid_argument);
                EXPECT_THROW(decoder.decode({0, UINT64_MAX}), std::invalid_argument);
                decoder.reset();
                EXPECT_TRUE(decoder.serial_decoder().shot_seeds.empty());
            }
            EXPECT_EQ(decoder.decode({0, 1, 2, 3}).weight, 10);
        });
        SharedComponentDecoder moved(std::move(decoder));
        moved.run_with_team([&] {
            EXPECT_EQ(moved.decode({0, 1, 2, 3}).weight, 10);
            check(moved);
            moved.reset();
            EXPECT_EQ(moved.decode({}).weight, 0);
        });
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
using GroupGeometry = structured::TimePartitionCuts::Geometry;
SharedComponentDecoder make_group(BackendUserGraph& graph, const StaticGridSpec& spec, size_t threads,
        GroupGeometry geometry, bool parallel_import = false, size_t parts = 2,
        size_t threshold = 0, size_t shot_threshold = 0) {
    return SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
        std::vector<int>(threads, -1), parts, threshold, shot_threshold,
        geometry == GroupGeometry::symmetric_width ? 3 : 0, parallel_import,
        SharedComponentDecoder::PrefixSchedule::component_group_owned,
        SharedComponentDecoder::ContinuationPlacement::component_captain, geometry);
}
PreparedRegionSnapshot group_snapshot(const ComponentDecoder& decoder) {
    auto state = snapshot_imported_regions(decoder);
    for (const auto& c : decoder.components()) {
        const auto& p = c.mwpm.predecoder;
        state.values.insert(state.values.end(), {p.result.obs_mask, uint64_t(p.result.weight),
            p.result_pending, p.has_live_cell_state(), c.seed_begin, c.seed_end});
    }
    return state;
}
std::vector<uint64_t> raw_group_seed_state(const ComponentDecoder& decoder) {
    std::vector<uint64_t> raw;
    for (const auto& component : decoder.components()) {
        const auto& pre = component.mwpm.predecoder;
        const auto& view = pre.seed_view;
        raw.insert(raw.end(), {view.storage.size(), view.active_count(), view.active_ranges().size()});
        size_t unmatched = 0;
        for (const auto& r : view.active_ranges()) {
            raw.insert(raw.end(), {r.begin, r.end, r.rank_begin});
            for (size_t slot = r.begin; slot < r.end; ++slot) {
                const auto& seed = view.storage[slot];
                raw.insert(raw.end(), {slot, seed.detector_id, seed.cell, seed.mate, seed.radius,
                    seed.match_slot, seed.grid->predecode.active[seed.cell], seed.grid->predecode.radii[seed.cell]});
                unmatched += seed.mate == structured::PredecodeSeed::UNMATCHED;
            }
        }
#if PM_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(pre.stats.normalized_seeds, view.active_count());
        EXPECT_EQ(pre.stats.residual_seeds, unmatched);
        EXPECT_EQ(pre.stats.residual_seeds + 2 * pre.stats.matched_pairs + pre.stats.matched_boundaries,
                  pre.stats.normalized_seeds);
#endif
    }
    return raw;
}
size_t group_captain(size_t c, size_t threads, size_t count = 2) {
    return PM_SHARED_COMPONENT_PARALLELISM == 1 || threads == 1 ? 0 : c * (threads / count);
}
#if PM_STRUCTURED_PROFILE_TIMELINE
void expect_group_timeline(const SharedComponentDecoder& decoder) {
    const auto& p = decoder.profile;
    const auto count = decoder.serial_decoder().components().size();
    const auto threads = decoder.team_size();
    ASSERT_TRUE(p.group_interval.present);
    EXPECT_FALSE(p.group_interval.repeated);
    EXPECT_LE(p.partition_interval.end_ns, p.group_interval.start_ns);
    EXPECT_LE(p.group_interval.end_ns, p.prefix_interval.end_ns);
    EXPECT_EQ(p.group_dispatch_interval.present, PM_SHARED_COMPONENT_PARALLELISM == 2 && threads > 1);
    EXPECT_EQ(p.group_join_interval.present, p.group_dispatch_interval.present);
    EXPECT_FALSE(p.interior_interval.present);
    EXPECT_FALSE(p.tail_dispatch_interval.present);
    for (const auto& lane : p.lanes) EXPECT_FALSE(lane.owner_scatter_interval.present);
    for (size_t c = 0; c < count; ++c) {
        const auto& cp = p.component[c];
        const auto captain = group_captain(c, threads, count);
        EXPECT_EQ(cp.group_worker, captain);
        EXPECT_EQ(cp.conflict_owner_lane, 0U);
        ASSERT_TRUE(cp.group_prefix_interval.present);
        EXPECT_LE(p.group_interval.start_ns, cp.group_prefix_interval.start_ns);
        EXPECT_LE(cp.group_prefix_interval.end_ns, p.group_interval.end_ns);
        if (c && (PM_SHARED_COMPONENT_PARALLELISM == 1 || threads == 1))
            EXPECT_LE(p.component[c - 1].group_prefix_interval.end_ns, cp.group_prefix_interval.start_ns);
        for (size_t lane = 0; lane < decoder.active_predecode_threads(c); ++lane) {
            const auto& job = cp.lanes[lane].group_lane_interval;
            const auto& fill = cp.lanes[lane].group_seed_scatter_interval;
            const auto& interior = cp.lanes[lane].worker_interval;
            ASSERT_TRUE(job.present);
            ASSERT_TRUE(fill.present);
            ASSERT_TRUE(interior.present);
            EXPECT_FALSE(job.repeated);
            EXPECT_FALSE(fill.repeated);
            EXPECT_FALSE(interior.repeated);
            EXPECT_LE(cp.dispatch_interval.start_ns, job.start_ns);
            EXPECT_LE(job.start_ns, fill.start_ns);
            EXPECT_LE(fill.end_ns, interior.start_ns);
            EXPECT_LE(cp.lanes[0].group_seed_scatter_interval.end_ns, interior.start_ns);
            EXPECT_LE(interior.end_ns, job.end_ns);
            EXPECT_LE(job.end_ns, cp.join_interval.end_ns);
            EXPECT_EQ(cp.lanes[lane].owner_conflict_wait_interval.present, lane != 0);
            if (lane) {
                const auto& wait = cp.lanes[lane].owner_conflict_wait_interval;
                EXPECT_LE(fill.end_ns, wait.start_ns);
                EXPECT_LE(cp.lanes[0].group_seed_scatter_interval.end_ns, wait.end_ns);
                EXPECT_LE(wait.end_ns, interior.start_ns);
            }
            for (size_t b = lane; b < cp.partitions; b += cp.active_threads)
                for (const auto& phase : decoder.partition_timing(c, b).timeline) if (phase.present) {
                    EXPECT_LE(interior.start_ns, phase.start_ns);
                    EXPECT_LE(phase.end_ns, interior.end_ns);
                }
        }
        if (cp.tail_interval.present) {
            EXPECT_EQ(cp.tail_worker, captain);
            EXPECT_LE(cp.join_interval.end_ns, cp.tail_interval.start_ns);
            EXPECT_LE(cp.tail_interval.end_ns, cp.group_prefix_interval.end_ns);
            if (cp.import_interval.present) {
                EXPECT_LE(cp.conflict_interval.end_ns, cp.import_interval.start_ns);
                EXPECT_LE(cp.import_interval.end_ns, cp.tail_interval.end_ns);
                for (const auto& lane : cp.lanes) if (const auto& w = lane.import_worker_interval; w.present) {
                    EXPECT_LE(cp.import_interval.start_ns, w.start_ns);
                    EXPECT_LE(w.end_ns, cp.import_interval.end_ns);
                }
            }
        }
    }
}
#endif
}

TEST(SharedComponents, GroupPreparedStateMatchesAcrossGeometriesThreadsAndImports) {
    for (bool rotated : {false, true}) {
        auto [dem, spec] = model(rotated, 9, 'x');
        auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        ComponentDecoder addresses(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        for (auto geometry : {GroupGeometry::symmetric_width, GroupGeometry::three_planes}) {
            std::vector<std::vector<uint64_t>> shots(8);
            for (size_t c = 0; c < 2; ++c) {
                const auto& component = addresses.components()[c];
                const auto& grid = *component.seed_grid;
                const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, 2,
                    geometry == GroupGeometry::three_planes ? 0 : 3, geometry);
                for (size_t i = 0; i < component.global_detectors.size(); ++i) {
                    const auto g = component.global_detectors[i];
                    shots[0].push_back(g);
                    if (g % 7 == 1) shots[1].push_back(g);
                    if (g % 9 == 2) shots[2 + c].push_back(g);
                    if (cuts.bucket(component.mwpm.flooder.graph.node(i).grid_cell) == 2) shots[4].push_back(g);
                }
            }
            for (auto& shot : shots) std::sort(shot.begin(), shot.end());
            shots[6] = shots[1]; std::reverse(shots[6].begin(), shots[6].end());
            shots[7] = shots[1]; std::mt19937 rng(20261014); std::shuffle(shots[7].begin(), shots[7].end(), rng);
            std::vector<PreparedRegionSnapshot> states(2 * shots.size());
            std::vector<std::vector<uint64_t>> raw_states(states.size());
            std::vector<MatchingResult> results(states.size());
            for (bool import_parallel : {false, true}) for (size_t threads : {1, 2, 4}) {
                auto decoder = make_group(graph, spec, threads, geometry, import_parallel);
                decoder.run_with_team([&] {
                    ASSERT_EQ(decoder.team_size(), threads);
                    for (size_t id = 0; id < threads; ++id) {
                        EXPECT_NE(decoder.thread_snapshots()[id].thread_id, 0U);
                        for (size_t other = 0; other < id; ++other)
                            EXPECT_NE(decoder.thread_snapshots()[id].thread_id, decoder.thread_snapshots()[other].thread_id);
                    }
                    for (size_t k = 0; k < states.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "G=" << int(geometry) << " rotated=" << rotated
                            << " T=" << threads << " I=" << import_parallel << " shot=" << k);
                        decoder.prepare_for_mwpm(shots[k % shots.size()]);
                        const auto actual = group_snapshot(decoder.serial_decoder());
                        const bool reference = !import_parallel && threads == 1;
                        const auto raw = raw_group_seed_state(decoder.serial_decoder());
                        if (reference) { states[k] = actual; raw_states[k] = raw; }
                        else {
                            EXPECT_EQ(raw, raw_states[k]);
                            EXPECT_EQ(actual.values, states[k].values);
                            EXPECT_EQ(actual.generations, states[k].generations);
                        }
                        for (size_t c = 0; c < 2; ++c) {
                            const size_t group_threads = PM_SHARED_COMPONENT_PARALLELISM == 1 || threads == 1 ? threads : threads / 2;
                            EXPECT_EQ(decoder.active_predecode_threads(c), std::min(size_t{2}, group_threads));
                        }
#if PM_STRUCTURED_PROFILE_TIMELINE
                        expect_group_timeline(decoder);
#endif
                        const auto result = decoder.solve_prepared();
                        if (reference) results[k] = result;
                        else EXPECT_EQ(result, results[k]);
#if PM_STRUCTURED_PROFILE_TIMELINE
                        for (size_t c = 0; c < 2; ++c)
                            EXPECT_EQ(decoder.profile.component[c].continuation_worker, group_captain(c, threads));
#endif
                        decoder.reset_after_shot();
                        expect_clean_nodes(decoder.serial_decoder(), false);
                    }
                });
            }
        }
    }
}

TEST(SharedComponents, GroupCanonicalPreparedStateForCrossBuildComparison) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    for (auto geometry : {GroupGeometry::symmetric_width, GroupGeometry::three_planes}) for (size_t parts : {2, 4}) {
        auto decoder = make_group(graph, spec, 4, geometry, true, parts);
        std::vector<uint64_t> shot;
        for (size_t i = 1; i < spec.coordinates.size(); i += 7) shot.push_back(i);
        decoder.run_with_team([&] {
            std::ostringstream raw, sparse;
            for (size_t round = 0; round < 2; ++round) {
                decoder.prepare_for_mwpm(shot);
                const auto state = group_snapshot(decoder.serial_decoder());
                for (auto v : raw_group_seed_state(decoder.serial_decoder())) sparse << v << ',';
                for (auto v : state.values) raw << v << ',';
                raw << '|';
                for (auto v : state.generations) raw << v << ',';
                const auto result = decoder.solve_prepared();
                raw << '|' << result.obs_mask << ',' << result.weight << ';';
                decoder.reset_after_shot();
            }
            // The validation harness compares these complete integer streams
            // between separately compiled CP1 and CP2 sanitizer binaries.
            RecordProperty("canonical_g" + std::to_string(int(geometry)) + "_p" + std::to_string(parts), raw.str());
            RecordProperty("fixed_raw_g" + std::to_string(int(geometry)) + "_p" + std::to_string(parts), sparse.str());
        });
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
std::array<structured::TimePartitionCuts, 2> group_failure_cuts;
using GroupNeighborhood = decltype(std::declval<structured::OneHopPredecoder::Grid>().predecode.neighborhood);
std::array<GroupNeighborhood, 2> group_original_neighborhood;
// Fault injection follows the actual execution entry. The bound kernel no
// longer calls the per-source neighborhood callback. Probe its first eligible
// source on the same owning worker, then invoke the real bound range unchanged.
// A failing probe has the same pre-growth boundary as the legacy callback.
using BoundRangeEntry = void (*)(structured::OneHopBoundRange&);
std::array<BoundRangeEntry, 2> original_bound_probe_entries{};
template <size_t C>
void probe_bound_neighborhood(structured::OneHopBoundRange& range) {
    for (auto i = range.begin; i < range.end; ++i) {
        const auto& seed = range.seeds[i];
        if (seed.mate == structured::PredecodeSeed::UNMATCHED) {
            range.grid.predecode.neighborhood(range.grid, seed.cell, range.grid.boundary_mask[seed.cell]);
            break;
        }
    }
    original_bound_probe_entries[C](range);
}
void install_bound_probe(size_t component, structured::OneHopPredecoder::Grid& grid) {
    original_bound_probe_entries[component] = grid.predecode.run_bound_range;
    if (grid.predecode.run_bound_range)
        grid.predecode.run_bound_range = component == 0 ? probe_bound_neighborhood<0> : probe_bound_neighborhood<1>;
}
void restore_bound_probe(size_t component, structured::OneHopPredecoder::Grid& grid) {
    grid.predecode.run_bound_range = original_bound_probe_entries[component];
}

std::array<std::array<std::atomic<size_t>, 3>, 2> group_visits, group_actors;
std::array<std::array<bool, 3>, 2> group_failures;
std::atomic<uint32_t> group_tail_failed;
size_t group_delayed_component = SIZE_MAX;
const std::array<std::array<const char*, 3>, 2> group_error_messages{{
    {"C0L0 interior", "C0L1 interior", "C0 tail"}, {"C1L0 interior", "C1L1 interior", "C1 tail"}}};
template <size_t C>
uint32_t inspect_group_bucket(const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    const auto bucket = group_failure_cuts[C].bucket(cell);
    group_visits[C][bucket].fetch_add(1, std::memory_order_relaxed);
    group_actors[C][bucket].store(size_t(omp_get_thread_num()), std::memory_order_relaxed);
    if (group_failures[C][bucket]) {
        if (bucket == 0 && group_delayed_component == C) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!(group_tail_failed.load(std::memory_order_acquire) & (1U << (1 - C)))) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("group dependency test timed out");
                std::this_thread::yield();
            }
        }
        if (bucket == 2) group_tail_failed.fetch_or(1U << C, std::memory_order_release);
        throw std::runtime_error(group_error_messages[C][bucket]);
    }
    return group_original_neighborhood[C](grid, cell, boundary);
}
}

TEST(SharedComponents, GroupFourWorkersFillAndJoinAllFailuresBeforeOrderedRecovery) {
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> dense(spec.coordinates.size()), sparse;
    std::iota(dense.begin(), dense.end(), 0);
    for (size_t i = 1; i < dense.size(); i += 7) sparse.push_back(i);
    std::vector<uint32_t> masks;
    for (uint32_t mask = 0; mask < 16; ++mask) masks.push_back(mask);
    for (uint32_t mask : {16, 32, 48, 16 + 4, 32 + 1}) masks.push_back(mask);
    const size_t threads = PM_SHARED_COMPONENT_PARALLELISM == 2 ? 4 : 2;
    for (bool import_parallel : {false, true}) for (const auto mask : masks) {
        SCOPED_TRACE(::testing::Message() << "mask=" << mask << " I=" << import_parallel);
        auto decoder = make_group(graph, spec, threads, GroupGeometry::three_planes, import_parallel);
        group_tail_failed.store(0, std::memory_order_relaxed);
        group_delayed_component = PM_SHARED_COMPONENT_PARALLELISM == 2
            ? (mask == 16 + 4 ? 1 : mask == 32 + 1 ? 0 : SIZE_MAX) : SIZE_MAX;
        const char* expected = nullptr;
        for (size_t c = 0; c < 2; ++c) {
            auto& grid = *decoder.serial_decoder().components()[c].seed_grid;
            group_failure_cuts[c] = structured::TimePartitionCuts(grid.nx * grid.ny, grid.nt, 2, 0, GroupGeometry::three_planes);
            group_original_neighborhood[c] = grid.predecode.neighborhood;
            for (size_t b = 0; b < 3; ++b) {
                group_visits[c][b].store(0, std::memory_order_relaxed);
                group_actors[c][b].store(SIZE_MAX, std::memory_order_relaxed);
                group_failures[c][b] = mask & (1U << (b < 2 ? 2 * c + b : 4 + c));
                if (b < 2 && group_failures[c][b] && !expected) expected = group_error_messages[c][b];
            }
            grid.predecode.neighborhood = c == 0 ? inspect_group_bucket<0> : inspect_group_bucket<1>;
            install_bound_probe(c, grid);
        }
        if (!expected && mask) expected = mask & 16 ? group_error_messages[0][2] : group_error_messages[1][2];
        decoder.run_with_team([&] {
            if (expected) {
                try {
                    decoder.prepare_for_mwpm(dense);
                    FAIL() << "Configured stage must fail";
                } catch (const std::runtime_error& error) {
                    EXPECT_EQ(std::string(error.what()), expected);
                }
            } else {
                EXPECT_NO_THROW(decoder.prepare_for_mwpm(dense));
            }
            ASSERT_EQ(decoder.team_size(), threads);
            for (size_t c = 0; c < 2; ++c) {
                decoder.serial_decoder().components()[c].seed_grid->predecode.neighborhood = group_original_neighborhood[c];
                restore_bound_probe(c, *decoder.serial_decoder().components()[c].seed_grid);
                for (size_t lane = 0; lane < 2; ++lane) {
                    EXPECT_GT(group_visits[c][lane].load(), 0U);
                    EXPECT_EQ(group_actors[c][lane].load(), group_captain(c, threads) + lane);
                }
                const bool skipped = group_failures[c][0] || group_failures[c][1];
                EXPECT_EQ(group_visits[c][2].load() == 0, skipped);
                if (!skipped) EXPECT_EQ(group_actors[c][2].load(), group_captain(c, threads));
            }
            if (group_delayed_component != SIZE_MAX)
                EXPECT_TRUE(group_tail_failed.load() & (1U << (1 - group_delayed_component)));
#if PM_STRUCTURED_PROFILE_TIMELINE
            expect_group_timeline(decoder);
#endif
            if (!expected) { decoder.solve_prepared(); decoder.reset_after_shot(); }
            else {
                const auto& retired = structured::ComponentDecoderTestAccess::retired(decoder.serial_decoder());
                ASSERT_EQ(retired.size(), dense.size());
                for (const auto& seed : retired) {
                    ASSERT_NE(seed.grid, nullptr);
                    EXPECT_EQ(seed.grid->predecode.active[seed.cell], 0U);
                    EXPECT_EQ(seed.grid->predecode.radii[seed.cell], 0);
                }
                expect_clean_nodes(decoder.serial_decoder());
            }
            decoder.prepare_for_mwpm(sparse);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder(), false);
        });
    }
}

TEST(SharedComponents, GroupColdImportFailureJoinsPeerAndRecovers) {
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> shot;
    for (size_t i = 1; i < spec.coordinates.size(); i += 7) shot.push_back(i);
    for (bool import_parallel : {false, true}) for (size_t failed : {0, 1}) {
        auto decoder = make_group(graph, spec, 4, GroupGeometry::three_planes, import_parallel);
        auto& pool = *decoder.serial_decoder().components()[failed].mwpm.flooder.regions;
        structured::RegionPoolTestAccess::set_generation(pool, 1, std::numeric_limits<structured::RegionGeneration>::max());
        decoder.run_with_team([&] {
            EXPECT_THROW(decoder.prepare_for_mwpm(shot), std::overflow_error);
            expect_clean_nodes(decoder.serial_decoder());
            for (const auto& c : decoder.serial_decoder().components()) {
                EXPECT_TRUE(c.mwpm.flooder.regions->empty());
                EXPECT_TRUE(c.mwpm.flooder.queue.empty());
                EXPECT_FALSE(c.mwpm.predecoder.has_live_cell_state());
            }
#if PM_STRUCTURED_PROFILE_TIMELINE
            expect_group_timeline(decoder);
            EXPECT_TRUE(decoder.profile.component[1 - failed].import_interval.present);
#endif
            structured::RegionPoolTestAccess::set_generation(pool, 1, 0);
            decoder.prepare_for_mwpm(shot);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder(), false);
        });
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(SharedComponents, GroupPreservesPriorOwnersAcrossNormalFallbackMoveAndReset) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    std::vector<uint64_t> shot;
    for (size_t i = 1; i < spec.coordinates.size(); i += 7) shot.push_back(i);
    for (bool import_parallel : {false, true}) for (size_t threads : {1, 2, 4}) {
        auto decoder = make_group(graph, spec, threads, GroupGeometry::three_planes, import_parallel, 2, 0, 5);
        MatchingResult expected;
        std::vector<uint64_t> prepared;
        decoder.run_with_team([&] {
            decoder.prepare_for_mwpm(shot);
            prepared = group_snapshot(decoder.serial_decoder()).values;
            expected = decoder.solve_prepared();
            EXPECT_THROW((SharedComponentDecoder(std::move(decoder))), std::logic_error);
            const auto normal = decoder.decode(shot);  // Consume prior group import owners first.
            EXPECT_EQ(normal.weight, expected.weight);
            expect_clean_nodes(decoder.serial_decoder(), false);
            EXPECT_NO_THROW(decoder.decode_to_matches(shot));
            decoder.prepare_for_mwpm(shot);
            EXPECT_EQ(group_snapshot(decoder.serial_decoder()).values, prepared);
            EXPECT_EQ(decoder.solve_prepared(), expected);
        });
        SharedComponentDecoder moved(std::move(decoder));
        EXPECT_THROW(decoder.serial_decoder(), std::logic_error);
        moved.run_with_team([&] {
            moved.prepare_for_mwpm(shot);
            EXPECT_EQ(group_snapshot(moved.serial_decoder()).values, prepared);
            EXPECT_EQ(moved.solve_prepared(), expected);
            std::vector<std::pair<size_t, size_t>> imported;
            for (size_t c = 0; c < 2; ++c) {
                auto& mwpm = moved.serial_decoder().components()[c].mwpm;
                if (mwpm.has_pending_seed_cleanup())
                    for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
                    for (const auto& seed : mwpm.predecoder.seed_view.storage.subspan(range.begin, range.count())) imported.emplace_back(c, seed.detector_id);
            }
            ASSERT_FALSE(imported.empty());
            moved.reset_after_shot();
            expect_clean_nodes(moved.serial_decoder(), false);
            for (auto [c, id] : imported) {
                auto& node = moved.serial_decoder().components()[c].mwpm.flooder.graph.node(id);
                node.observables_crossed_from_source = 123;
                node.set_wrapped_radius(246);
            }
            moved.reset_after_shot();
            for (auto [c, id] : imported) {
                auto& node = moved.serial_decoder().components()[c].mwpm.flooder.graph.node(id);
                EXPECT_EQ(node.observables_crossed_from_source, 123U);
                EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].wrapped_radius_cached, 246);
                node.reset();
            }
            moved.prepare_for_mwpm(shot);
            EXPECT_EQ(moved.solve_prepared(), expected);
            moved.prepare_for_mwpm({0, 1});  // Original whole-shot fallback consumes prior group owners.
            EXPECT_EQ(moved.component_route(0), structured::SharedRoute::whole_serial);
#if PM_STRUCTURED_PROFILE_TIMELINE
            EXPECT_FALSE(moved.profile.group_interval.present);
#endif
            EXPECT_NO_THROW(moved.solve_prepared());
            moved.reset_after_shot();
            moved.prepare_for_mwpm(shot);
            moved.reset_after_shot();  // Abandon only after every helper has joined.
            expect_clean_nodes(moved.serial_decoder());
            moved.prepare_for_mwpm(shot);
            EXPECT_EQ(moved.solve_prepared(), expected);
            moved.reset();
            expect_clean_nodes(moved.serial_decoder());
        });
    }
}

TEST(SharedComponents, GroupOriginalLowComponentP1SignedAndFormalFailureRoutes) {
    for (size_t threads : {1, 2, 4}) {
        auto graph = pair_graph();
        for (size_t parts : {1, 2}) {
            auto decoder = make_group(graph, pair_spec(), threads, GroupGeometry::three_planes, true, parts, 100);
            decoder.run_with_team([&] {
                decoder.prepare_for_mwpm({0, 1});
                EXPECT_EQ(decoder.component_route(0), structured::SharedRoute::serial);
                EXPECT_EQ(decoder.active_predecode_threads(0), 1U);
#if PM_STRUCTURED_PROFILE_TIMELINE
                EXPECT_FALSE(decoder.profile.group_interval.present);
                EXPECT_FALSE(decoder.profile.component[0].bucket_count_interval.present);
#endif
                EXPECT_NO_THROW(decoder.solve_prepared());
                decoder.reset_after_shot();
            });
        }
        auto signed_graph = pair_graph();
        signed_graph.add_or_merge_boundary_edge(2, {}, -1, -1);
        ComponentDecoder serial(signed_graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
        const auto expected = serial.decode({0, 1});
        auto signed_decoder = make_group(signed_graph, pair_spec(), threads, GroupGeometry::three_planes);
        signed_decoder.run_with_team([&] {
            signed_decoder.prepare_for_mwpm({0, 1});
            EXPECT_EQ(signed_decoder.solve_prepared(), expected);
            signed_decoder.reset_after_shot();
        });
        for (size_t failed : {1, 2, 3}) {
            BackendUserGraph failing_graph(4, 2);
            failing_graph.add_or_merge_edge(0, 2, {0}, 2, -1);
            failing_graph.add_or_merge_edge(1, 3, {1}, 3, -1);
            if (!(failed & 1)) failing_graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
            if (!(failed & 2)) failing_graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
            auto failing = make_group(failing_graph, pair_spec(), threads, GroupGeometry::three_planes, true);
            failing.run_with_team([&] {
                failing.prepare_for_mwpm({0, 1});
                EXPECT_THROW(failing.solve_prepared(), std::invalid_argument);
                expect_clean_nodes(failing.serial_decoder());
#if PM_STRUCTURED_PROFILE_TIMELINE
                for (size_t c = 0; c < 2; ++c) {
                    EXPECT_TRUE(failing.profile.component[c].continuation_interval.present);
                    EXPECT_EQ(failing.profile.component[c].continuation_worker, group_captain(c, threads));
                }
#endif
                failing.prepare_for_mwpm({0, 1, 2, 3});
                EXPECT_EQ(failing.solve_prepared().weight, 10);
                failing.reset_after_shot();
            });
        }
    }
}

TEST(SharedComponents, GroupThreePlaneUnrestrictedTargetsKeepDualAndMatchingValid) {
    // A time chain puts one active interior source next to each conflict edge.
    // Both sources must match/grow their conflict target without a target guard.
    for (size_t count : {1, 2}) {
        BackendUserGraph graph(27 * count, count);
        StaticGridSpec spec{9, 27, false, {}, {}, GridSimd::scalar};
        for (size_t t = 0; t < 27; ++t) for (size_t c = 0; c < count; ++c) {
            spec.coordinates.push_back({0, 0, int64_t(t)});
            spec.groups.push_back(c);
            if (t) graph.add_or_merge_edge(count * (t - 1) + c, count * t + c, {}, 2, -1);
            if (t == 0 || t == 26) graph.add_or_merge_boundary_edge(count * t + c, {c}, 8, -1);
        }
        ComponentDecoder addresses(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        std::vector<uint64_t> shot;
        for (const auto& c : addresses.components()) {
            const auto& grid = *c.seed_grid;
            const size_t plane = grid.nx * grid.ny;
            const size_t q = 1 + (grid.nt - 2) / 2;
            for (auto global : c.global_detectors) {
                const auto t = structured::ComponentDecoderTestAccess::addresses(addresses)[global][3] / plane;
                if (t == q - 2 || t == q - 1 || t == q + 1 || t == q + 2) shot.push_back(global);
            }
        }
        ASSERT_EQ(shot.size(), 4 * count);
        std::vector<uint64_t> expected;
        for (size_t threads : {1, 2, 4}) {
            auto decoder = make_group(graph, spec, threads, GroupGeometry::three_planes);
            decoder.run_with_team([&] {
                decoder.prepare_for_mwpm(shot);
                const auto state = group_snapshot(decoder.serial_decoder()).values;
                if (threads == 1) expected = state;
                else EXPECT_EQ(state, expected);
                for (const auto& c : decoder.serial_decoder().components()) {
                    const auto& pre = c.mwpm.predecoder;
                    const auto& grid = *c.seed_grid;
                    const size_t plane = grid.nx * grid.ny;
                    const size_t q = 1 + (grid.nt - 2) / 2;
                    std::vector<uint64_t> radius(c.mwpm.flooder.graph.num_nodes);
                    for (const auto& range : pre.seed_view.active_ranges())
                    for (size_t i = range.begin; i < range.end; ++i) {
                        const auto& seed = pre.seed_view.storage[i];
                        radius[seed.detector_id] = seed.radius;
                        ASSERT_TRUE(pre.seed_view.contains(seed.mate));
                        const auto& mate = pre.seed_view.storage[seed.mate];
                        EXPECT_EQ(mate.mate, i);
                        EXPECT_EQ(uint64_t(seed.radius) + mate.radius, grid.weights[seed.cell][seed.match_slot]);
                        const auto t = seed.cell / plane, mt = mate.cell / plane;
                        if (t == q - 2) EXPECT_EQ(mt, q - 1);
                        if (t == q + 2) EXPECT_EQ(mt, q + 1);
                    }
                    for (size_t i = 0; i < radius.size(); ++i) {
                        const auto& node = c.mwpm.flooder.graph.node(i);
                        for (size_t slot = 0; slot < node.edge_slots(); ++slot) if (node.has_edge(slot)) {
                            const auto* other = node.neighbor(slot);
                            EXPECT_LE(radius[i] + (other ? radius[other->detector_id] : 0), uint64_t(node.edge_weight(slot)));
                        }
                    }
                    for (size_t cell = q * plane; cell < (q + 1) * plane; ++cell)
                        EXPECT_EQ(grid.predecode.radii[cell], 0U);
                }
#if PM_STRUCTURED_PROFILE_TIMELINE
                expect_group_timeline(decoder);
#endif
                EXPECT_NO_THROW(decoder.solve_prepared());
                decoder.reset_after_shot();
                expect_clean_nodes(decoder.serial_decoder());
            });
        }
    }
}

TEST(SharedComponents, GroupFixedComponentMoveKeepsBindingBackingAndScratch) {
    using Access = structured::ComponentDecoderTestAccess;
    auto graph = pair_graph();
    auto original = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
    const auto* backing = Access::retired(original.serial_decoder()).data();
    ComponentDecoder moved(std::move(original.serial_decoder()));
    EXPECT_TRUE(Access::is_fixed(moved));
    EXPECT_EQ(Access::retired(moved).data(), backing);
    EXPECT_THROW(Access::bind(moved, 4, 0, GroupGeometry::three_planes), std::logic_error);
    Access::bind(moved, 2, 0, GroupGeometry::three_planes);
    SharedComponentDecoder target(std::move(moved), {-1, -1, -1, -1}, 2, 0, 0, 0, false,
        SharedComponentDecoder::PrefixSchedule::component_group_owned,
        SharedComponentDecoder::ContinuationPlacement::component_captain, GroupGeometry::three_planes);
    target.run_with_team([&] {
        target.prepare_for_mwpm({0, 1, 2, 3});
        target.solve_prepared(); target.reset_after_shot();
        EXPECT_EQ(Access::retired(target.serial_decoder()).data(), backing);
    });
}

TEST(SharedComponents, GroupFixedConstructionRetiresUsedStandaloneShot) {
    using Access = structured::ComponentDecoderTestAccess;
    auto graph = pair_graph();
    const auto spec = pair_spec();
    for (bool fixed : {false, true}) for (bool prepared : {false, true}) {
        ComponentDecoder input(graph, spec, NUM_DISTINCT_WEIGHTS, false);
        if (fixed) {
            Access::bind(input, 2, 0, GroupGeometry::three_planes);
            Access::fixed(input);
        }
        const auto* backing = Access::retired(input).data();
        if (prepared) input.prepare_for_mwpm({0});
        else input.decode({0});
        SharedComponentDecoder target(std::move(input), {-1, -1, -1, -1}, 2, 0, 0, 0, false,
            SharedComponentDecoder::PrefixSchedule::component_group_owned,
            SharedComponentDecoder::ContinuationPlacement::component_captain, GroupGeometry::three_planes);
        ASSERT_TRUE(Access::is_fixed(target.serial_decoder()));
        EXPECT_EQ(target.serial_decoder().shot_seeds.capacity(), 0U);
        EXPECT_EQ(Access::retired(target.serial_decoder()).size(), 4U);
        if (fixed) EXPECT_EQ(Access::retired(target.serial_decoder()).data(), backing);
        expect_clean_nodes(target.serial_decoder(), false);
        target.run_with_team([&] {
            target.prepare_for_mwpm({0, 1, 2, 3});
            target.solve_prepared(); target.reset_after_shot();
            if (fixed) EXPECT_EQ(Access::retired(target.serial_decoder()).data(), backing);
            expect_clean_nodes(target.serial_decoder(), false);
        });
    }
}

TEST(SharedComponents, GroupFixedStorageHolesAndDenseFallbackKeepObjectLifetimes) {
    using Access = structured::ComponentDecoderTestAccess;
    using Scratch = structured::OneHopPredecoderTestAccess;
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    for (size_t parts : {2, 4, 8}) for (bool import_parallel : {false, true}) {
        auto decoder = make_group(graph, spec, 4, GroupGeometry::three_planes, import_parallel, parts, 0, 3);
        auto& serial = decoder.serial_decoder();
        ASSERT_TRUE(Access::is_fixed(serial));
        const auto* backing = Access::retired(serial).data();
        const auto capacity = Access::retired(serial).size();
        std::array<decltype(Scratch::scratch_storage(serial.components()[0].mwpm.predecoder)), 2> scratch;
        for (size_t c = 0; c < serial.components().size(); ++c) scratch[c] = Scratch::scratch_storage(serial.components()[c].mwpm.predecoder);
        const auto addresses = Access::addresses(serial);
        std::vector<uint64_t> shot;
        for (size_t g = 0; g < addresses.size(); ++g) if (g % 19 == 2) shot.push_back(g);
        ASSERT_GT(shot.size(), 3U);
        std::array<std::array<size_t, 9>, 2> counts{};
        for (auto g : shot) ++counts[addresses[g][0]][addresses[g][1]];
        // Poison every hole before fill. No read, reset or import may touch it.
        struct Poison {
            structured::PredecodeSeed* base;
            size_t count;
            ~Poison() {
#if defined(__SANITIZE_ADDRESS__)
                __asan_unpoison_memory_region(base, count * sizeof(*base));
#endif
            }
        } poison{const_cast<structured::PredecodeSeed*>(backing), capacity};
        for (size_t c = 0; c < serial.components().size(); ++c) for (size_t b = 0; b <= parts; ++b) {
            const auto base = Access::component_bases(serial)[c] + Access::fixed_bases(serial)[c][b];
            const auto end = Access::component_bases(serial)[c] + Access::fixed_bases(serial)[c][b + 1];
            for (size_t slot = base + counts[c][b]; slot < end; ++slot) {
                poison.base[slot].grid = nullptr;
                poison.base[slot].mate = structured::PredecodeSeed::UNMATCHED;
            }
#if defined(__SANITIZE_ADDRESS__)
            __asan_poison_memory_region(poison.base + base + counts[c][b], (end - base - counts[c][b]) * sizeof(*backing));
#endif
        }
        decoder.run_with_team([&] {
            decoder.prepare_for_mwpm(shot);
            EXPECT_EQ(serial.shot_seeds.data(), backing);
            EXPECT_EQ(serial.shot_seeds.size(), capacity);
            for (size_t c = 0; c < serial.components().size(); ++c) {
                const auto& view = serial.components()[c].mwpm.predecoder.seed_view;
                size_t rank = 0;
                for (size_t b = 0; b <= parts; ++b) {
                    const auto& range = view.active_ranges()[b];
                    EXPECT_EQ(range.count(), counts[c][b]);
                    for (size_t slot = range.begin; slot < range.end; ++slot) {
                        EXPECT_EQ(view.rank_of(slot), rank++);
                        EXPECT_TRUE(view.contains(slot));
                    }
                }
                EXPECT_EQ(rank, view.active_count());
            }
            decoder.solve_prepared(); decoder.reset_after_shot();
            EXPECT_EQ(Access::retired(serial).data(), backing);
#if defined(__SANITIZE_ADDRESS__)
            __asan_unpoison_memory_region(poison.base, capacity * sizeof(*backing));
#endif
            // Ordinary decode, public dense partitioned API, whole-shot fallback,
            // then sparse Group again all keep the same constructed allocations.
            decoder.decode(shot);
            serial.decode_partitioned(serial.partition_detection_events(shot));
            const std::vector<uint64_t> small{shot.front()};
            decoder.prepare_for_mwpm(small); decoder.solve_prepared(); decoder.reset_after_shot();
            for (bool partitioned : {false, true}) {
                decoder.prepare_for_mwpm(shot); decoder.solve_prepared();
                if (partitioned) serial.decode_partitioned(serial.partition_detection_events(small));
                else serial.decode(small);
                decoder.prepare_for_mwpm(shot); decoder.solve_prepared(); decoder.reset_after_shot();
            }
            serial.decode(small); decoder.reset_after_shot();
            std::reverse(shot.begin(), shot.end());
            decoder.prepare_for_mwpm(shot); decoder.solve_prepared(); decoder.reset_after_shot();
            EXPECT_EQ(Access::retired(serial).data(), backing);
            EXPECT_EQ(Access::retired(serial).size(), capacity);
            for (size_t c = 0; c < serial.components().size(); ++c)
                EXPECT_EQ(Scratch::scratch_storage(serial.components()[c].mwpm.predecoder), scratch[c]);
            // Dense MWPM clears tracker validity but may retain inactive times
            // on non-seed nodes. Those values are not live state.
            expect_clean_nodes(serial, false);
        });
    }
}
#endif

#if defined(_OPENMP) && !PM_PREDECODE_ENABLED
TEST(SharedComponents, GroupWithoutPredecodeRejectsConstructionAndKeepsSerialEntry) {
    auto graph = pair_graph();
    const auto spec = pair_spec();
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    ComponentDecoder current(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    EXPECT_FALSE(structured::ComponentDecoderTestAccess::is_fixed(current));
    for (const std::vector<uint64_t> shot : {std::vector<uint64_t>{0, 1, 2, 3}, {}, {0, 1}})
        EXPECT_EQ(current.decode(shot), reference.decode_partitioned(reference.partition_detection_events(shot)));
    for (size_t parts : {2, 4}) for (size_t threads : {1, 2, 4}) {
        EXPECT_THROW((SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            std::vector<int>(threads, -1), parts, 0, 0, 0, false,
            SharedComponentDecoder::PrefixSchedule::component_group_owned,
            SharedComponentDecoder::ContinuationPlacement::component_captain,
            structured::TimePartitionCuts::Geometry::three_planes)), std::invalid_argument);
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(DoubleBufferComponents, EmptyOddBatchesKeepParityAndReturnOwnedResults) {
    auto graph = pair_graph();
    for (auto schedule : {DoubleBufferComponentDecoder::Schedule::overlap,
                          DoubleBufferComponentDecoder::Schedule::serial_control}) {
        for (size_t threads : {1, 2, 4}) {
            auto even = make_group(graph, pair_spec(), threads, GroupGeometry::three_planes, true, 2, 64, 32);
            auto odd = make_group(graph, pair_spec(), threads, GroupGeometry::three_planes, true, 2, 64, 32);
            const auto* even_state = &even.serial_decoder();
            const auto* odd_state = &odd.serial_decoder();
            EXPECT_NE(even_state, odd_state);
            EXPECT_NE(even_state->components()[0].mwpm.flooder.regions.get(),
                      odd_state->components()[0].mwpm.flooder.regions.get());
            DoubleBufferComponentDecoder pipeline(std::move(even), std::move(odd), schedule);
            ComponentDecoder reference(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
            size_t sequence = 0;
            std::vector<MatchingResult> results, expected;
            for (size_t count : {0, 1, 2, 5, 0, 64, 3}) {
                pipeline.run_with_team([&] {
                    EXPECT_EQ(pipeline.team_size(), 2 * threads);
                    EXPECT_EQ(pipeline.group_size(), threads);
                    std::set<uint64_t> tids;
                    for (size_t b = 0; b < 2; ++b)
                        for (const auto& thread : pipeline.thread_snapshots(b)) {
                            EXPECT_NE(thread.thread_id, 0U);
                            EXPECT_TRUE(tids.insert(thread.thread_id).second);
                        }
                    for (size_t k = 0; k < count; ++k) {
                        EXPECT_EQ(pipeline.next_buffer(), sequence & 1);
                        std::vector<uint64_t> input;
                        for (size_t n = 0; n < 4; ++n) if ((sequence >> n) & 1) input.push_back(n);
                        expected.push_back(reference.decode(input));
                        results.push_back(pipeline.decode(input));
                        // The caller owns results, including after the remote
                        // group has begun clearing the producing buffer.
                        EXPECT_EQ(results.back(), expected.back());
                        ++sequence;
                    }
                    pipeline.drain();
                    expect_clean_nodes(*even_state, false);
                    expect_clean_nodes(*odd_state, false);
                });
                EXPECT_EQ(pipeline.next_buffer(), sequence & 1);
                EXPECT_EQ(results, expected);
            }
        }
    }
}


TEST(DoubleBufferComponents, SurfaceShotsMatchParentAcrossGroupsAndFallbacks) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto oracle = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
    std::mt19937_64 rng(20261014);
    std::vector<std::vector<uint64_t>> shots(48);
    std::vector<MatchingResult> generic(shots.size()), expected(shots.size());
    for (size_t k = 0; k < shots.size(); ++k) {
        for (size_t n = 0; n < spec.coordinates.size(); ++n)
            if (k % 6 != 0 && rng() % (k % 3 == 0 ? 7 : 71) == 0) shots[k].push_back(n);
        generic[k] = decode_detection_events_for_up_to_64_observables(oracle, shots[k], false);
    }
    for (size_t threads : {1, 2, 4}) for (size_t threshold : {0, 64}) {
        auto reference = make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, threshold, 32);
        reference.run_with_team([&] {
            for (size_t k = 0; k < shots.size(); ++k) {
                reference.prepare_for_mwpm(shots[k]);
                expected[k] = reference.solve_prepared();
                reference.reset_after_shot();
                EXPECT_EQ(expected[k].weight, generic[k].weight);
            }
        });
        for (auto schedule : {DoubleBufferComponentDecoder::Schedule::overlap,
                              DoubleBufferComponentDecoder::Schedule::serial_control,
                              DoubleBufferComponentDecoder::Schedule::single_thread}) {
            if (schedule == DoubleBufferComponentDecoder::Schedule::single_thread && threads != 1) continue;
            DoubleBufferComponentDecoder pipeline(
                make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, threshold, 32),
                make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, threshold, 32), schedule);
            pipeline.run_with_team([&] {
                for (size_t repeat = 0; repeat < 3; ++repeat)
                    for (size_t k = 0; k < shots.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "T=" << threads << " threshold=" << threshold
                                     << " schedule=" << int(schedule) << " shot=" << k);
                        EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
                    }
            });
        }
    }
}

TEST(DoubleBufferComponents, ExpandedPartitionsMatchGenericAcrossRegularAndFixedRoutes) {
    // d=33 leaves nonempty interiors between all seven three-plane guards.
    // P8/T16 has four lanes per component, so each lane visits two interiors.
    auto [dem, spec] = model(true, 33, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto oracle = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
    std::mt19937_64 rng(20261021);
    std::vector<std::vector<uint64_t>> shots(12);
    std::vector<MatchingResult> generic(shots.size()), expected(shots.size());
    for (size_t k = 0; k < shots.size(); ++k) {
        for (size_t n = 0; n < spec.coordinates.size(); ++n)
            if (k % 6 != 0 && rng() % (k % 3 == 0 ? 71 : 137) == 0) shots[k].push_back(n);
        if (k == 5) shots[k].resize(1);  // Whole-shot fallback and subsequent reuse.
        if (k == 7) std::reverse(shots[k].begin(), shots[k].end());
        generic[k] = decode_detection_events_for_up_to_64_observables(oracle, shots[k], false);
    }
    for (size_t parts : {4, 8}) for (size_t threshold : {0, 64}) {
        auto reference = make_group(graph, spec, 1, GroupGeometry::three_planes, true, parts, threshold, 32);
        // Explicitly prove that the fixture covers every interior and conflict bucket.
        for (const auto& component : reference.serial_decoder().components()) {
            const auto& grid = *component.seed_grid;
            const structured::TimePartitionCuts cuts(grid.nx * grid.ny, grid.nt, parts, 0,
                                                     GroupGeometry::three_planes);
            std::array<size_t, 9> capacities{};
            for (size_t n = 0; n < component.global_detectors.size(); ++n)
                ++capacities[cuts.bucket(component.mwpm.flooder.graph.node(n).grid_cell)];
            for (size_t bucket = 0; bucket <= parts; ++bucket) ASSERT_GT(capacities[bucket], 0U);
        }
        reference.run_with_team([&] {
            for (size_t k = 0; k < shots.size(); ++k) {
                reference.prepare_for_mwpm(shots[k]);
                expected[k] = reference.solve_prepared();
                reference.reset_after_shot();
                EXPECT_EQ(expected[k].weight, generic[k].weight);
            }
        });
        for (size_t threads : {4, 8}) {
            // First verify exact fixed-algorithm results without pipeline overlap.
            auto direct = make_group(graph, spec, threads, GroupGeometry::three_planes, true, parts, threshold, 32);
            direct.run_with_team([&] {
                for (size_t k = 0; k < shots.size(); ++k) {
                    direct.prepare_for_mwpm(shots[k]);
                    EXPECT_EQ(direct.solve_prepared(), expected[k]);
                    direct.reset_after_shot();
                    expect_clean_nodes(direct.serial_decoder(), false);
                }
            });
            DoubleBufferComponentDecoder pipeline(
                make_group(graph, spec, threads, GroupGeometry::three_planes, true, parts, threshold, 32),
                make_group(graph, spec, threads, GroupGeometry::three_planes, true, parts, threshold, 32));
            pipeline.run_with_team([&] {
                for (size_t repeat = 0; repeat < 2; ++repeat)
                    for (size_t k = 0; k < shots.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "P=" << parts << " total_threads=" << 2 * threads
                            << " threshold=" << threshold << " repeat=" << repeat << " shot=" << k);
                        EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
                    }
            });
        }
    }
}

TEST(DoubleBufferComponents, Ty2x2MatchesGenericAcrossRegularAndFixedRoutes) {
    struct Fixture { bool rotated; size_t distance; };
    // d=3 exercises guards that consume whole short axes; d=33 exercises all
    // four nonempty quadrants, and the unrotated fixture has a distinct stencil.
    for (const auto fixture : {Fixture{true, 3}, Fixture{false, 5}, Fixture{true, 33}}) {
        auto [dem, spec] = model(fixture.rotated, fixture.distance, 'x');
        auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        auto oracle = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
        std::mt19937_64 rng(20261021);
        std::vector<std::vector<uint64_t>> shots(12);
        std::vector<MatchingResult> generic(shots.size()), expected(shots.size());
        for (size_t k = 0; k < shots.size(); ++k) {
            for (size_t n = 0; n < spec.coordinates.size(); ++n) {
                const size_t divisor = fixture.distance == 33 ? (k == 2 ? 13 : 137) : (k == 2 ? 1 : 7);
                if (k % 6 != 0 && rng() % divisor == 0) shots[k].push_back(n);
            }
            if (k == 5) shots[k] = {0};
            if (k == 7) std::reverse(shots[k].begin(), shots[k].end());
            generic[k] = decode_detection_events_for_up_to_64_observables(oracle, shots[k], false);
        }
        for (size_t threshold : {0, 64}) for (bool parallel_import : {false, true}) {
            auto make = [&](size_t threads) {
                return make_group(graph, spec, threads, GroupGeometry::ty_2x2, parallel_import, 4, threshold, 3);
            };
            auto reference = make(1);
            if (fixture.distance == 33) {
                for (const auto& component : reference.serial_decoder().components()) {
                    const auto& grid = *component.seed_grid;
                    const structured::TimePartitionCuts cuts(grid.nx, grid.ny, grid.nt, 4, 0,
                                                             GroupGeometry::ty_2x2);
                    std::array<size_t, 5> capacities{};
                    for (size_t n = 0; n < component.global_detectors.size(); ++n)
                        ++capacities[cuts.bucket(component.mwpm.flooder.graph.node(n).grid_cell)];
                    for (auto capacity : capacities) ASSERT_GT(capacity, 0U);
                }
            }
            reference.run_with_team([&] {
                for (size_t k = 0; k < shots.size(); ++k) {
                    reference.prepare_for_mwpm(shots[k]);
                    expected[k] = reference.solve_prepared();
                    reference.reset_after_shot();
                    EXPECT_EQ(expected[k].weight, generic[k].weight);
                }
            });
            for (size_t threads : {4, 8}) {
                auto direct = make(threads);
                direct.run_with_team([&] {
                    for (size_t k = 0; k < shots.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "rotated=" << fixture.rotated << " d=" << fixture.distance
                            << " threads=" << threads << " threshold=" << threshold
                            << " import=" << parallel_import << " shot=" << k);
                        direct.prepare_for_mwpm(shots[k]);
                        EXPECT_EQ(direct.solve_prepared(), expected[k]);
                        direct.reset_after_shot();
                        expect_clean_nodes(direct.serial_decoder(), false);
                    }
                    EXPECT_THROW(direct.prepare_for_mwpm({0, 0}), std::invalid_argument);
                    EXPECT_THROW(direct.prepare_for_mwpm({UINT64_MAX}), std::invalid_argument);
                    direct.prepare_for_mwpm(shots[2]);
                    EXPECT_EQ(direct.solve_prepared(), expected[2]);
                    direct.reset_after_shot();
                });
                DoubleBufferComponentDecoder pipeline(make(threads), make(threads));
                pipeline.run_with_team([&] {
                    for (size_t repeat = 0; repeat < 2; ++repeat)
                        for (size_t k = 0; k < shots.size(); ++k) {
                            SCOPED_TRACE(::testing::Message() << "rotated=" << fixture.rotated << " d=" << fixture.distance
                                << " total_threads=" << 2 * threads << " threshold=" << threshold
                                << " import=" << parallel_import << " repeat=" << repeat << " shot=" << k);
                            EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
                        }
                });
            }
        }
    }
}

TEST(DoubleBufferComponents, ExceptionsJoinBothGroupsAndPermitReuse) {
    auto graph = pair_graph();
    for (size_t threads : {1, 2, 4}) {
        DoubleBufferComponentDecoder pipeline(
            make_group(graph, pair_spec(), threads, GroupGeometry::three_planes, true),
            make_group(graph, pair_spec(), threads, GroupGeometry::three_planes, true));
        pipeline.run_with_team([&] {
            for (size_t attempt = 0; attempt < 4; ++attempt) {
                EXPECT_THROW(pipeline.decode({99}), std::invalid_argument);
                EXPECT_EQ(pipeline.decode({0, 1, 2, 3}).weight, 10);
                EXPECT_EQ(pipeline.decode({0, 1, 2, 3}).weight, 10);
            }
        });
        for (size_t count : {1, 2, 3}) {
            EXPECT_THROW(pipeline.run_with_team([&] {
                for (size_t k = 0; k < count; ++k) EXPECT_EQ(pipeline.decode({0, 2}).weight, 4);
                throw std::runtime_error("callback abort");
            }), std::runtime_error);
            pipeline.run_with_team([&] { EXPECT_EQ(pipeline.decode({1, 3}).weight, 6); });
        }
        // Trigger a formal MWPM failure in both parity positions, after
        // preparation has populated the region/seed/queue state.
        BackendUserGraph disconnected(4, 2);
        disconnected.add_or_merge_edge(0, 2, {0}, 2, -1);
        disconnected.add_or_merge_edge(1, 3, {1}, 3, -1);
        DoubleBufferComponentDecoder failing(
            make_group(disconnected, pair_spec(), threads, GroupGeometry::three_planes, true),
            make_group(disconnected, pair_spec(), threads, GroupGeometry::three_planes, true));
        failing.run_with_team([&] {
            EXPECT_THROW(failing.decode({0, 1}), std::invalid_argument);
            EXPECT_THROW(failing.decode({0, 1}), std::invalid_argument);
            EXPECT_EQ(failing.decode({0, 1, 2, 3}).weight, 10);
            EXPECT_EQ(failing.decode({0, 1, 2, 3}).weight, 10);
        });
    }
}

TEST(DoubleBufferComponents, MovesAndCallerGuardsRetainOwnership) {
    auto graph = pair_graph();
    DoubleBufferComponentDecoder original(
        make_group(graph, pair_spec(), 2, GroupGeometry::three_planes),
        make_group(graph, pair_spec(), 2, GroupGeometry::three_planes));
    EXPECT_THROW(original.decode({0}), std::logic_error);
    EXPECT_THROW(original.drain(), std::logic_error);
    original.run_with_team([&] { EXPECT_EQ(original.decode({0, 2}).weight, 4); });
    auto pipeline = std::move(original);
    EXPECT_THROW(original.run_with_team([] {}), std::logic_error);
    EXPECT_EQ(pipeline.next_buffer(), 1U);
    pipeline.run_with_team([&] {
        EXPECT_THROW(pipeline.run_with_team([] {}), std::logic_error);
        EXPECT_THROW(DoubleBufferComponentDecoder invalid(std::move(pipeline)), std::logic_error);
        std::atomic<bool> rejected{false};
        std::thread outsider([&] {
            try { pipeline.decode({0}); }
            catch (const std::logic_error&) { rejected.store(true); }
        });
        outsider.join();
        EXPECT_TRUE(rejected.load());
        EXPECT_EQ(pipeline.decode({1, 3}).weight, 6);
    });
    DoubleBufferComponentDecoder target;
    target = std::move(pipeline);
    target.run_with_team([&] { EXPECT_EQ(target.decode({0, 1, 2, 3}).weight, 10); });
}

TEST(DoubleBufferComponents, RejectsOverlappingExplicitCpuAndUnequalGroups) {
    auto graph = pair_graph();
    auto make = [&](std::vector<int> cpus) {
        return SharedComponentDecoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false), cpus);
    };
    EXPECT_THROW((DoubleBufferComponentDecoder(make({0}), make({0}))), std::invalid_argument);
    EXPECT_THROW((DoubleBufferComponentDecoder(make({-1}), make({-1, -1}))), std::invalid_argument);
    EXPECT_THROW((DoubleBufferComponentDecoder(make({-1, -1}), make({-1, -1}),
        DoubleBufferComponentDecoder::Schedule::single_thread)), std::invalid_argument);
    EXPECT_THROW((DoubleBufferComponentDecoder(make({0}), make({1}),
        DoubleBufferComponentDecoder::Schedule::single_thread)), std::invalid_argument);
}

TEST(DoubleBufferComponents, SingleThreadControlUsesOnePhysicalOwnerForBothBuffers) {
    auto graph = pair_graph();
    DoubleBufferComponentDecoder pipeline(
        make_group(graph, pair_spec(), 1, GroupGeometry::three_planes, true),
        make_group(graph, pair_spec(), 1, GroupGeometry::three_planes, true),
        DoubleBufferComponentDecoder::Schedule::single_thread);
    EXPECT_EQ(pipeline.serial_decoder().num_observables(), 2U);
    for (size_t count : {0, 1, 2, 5}) {
        pipeline.run_with_team([&] {
            EXPECT_EQ(omp_get_num_threads(), 1);
            EXPECT_EQ(pipeline.team_size(), 1U);
            EXPECT_EQ(pipeline.thread_snapshots(0)[0].thread_id, pipeline.thread_snapshots(1)[0].thread_id);
            EXPECT_THROW(pipeline.serial_decoder(), std::logic_error);
            for (size_t k = 0; k < count; ++k) EXPECT_EQ(pipeline.decode({0, 1, 2, 3}).weight, 10);
            EXPECT_THROW(pipeline.decode({99}), std::invalid_argument);
            EXPECT_THROW(pipeline.decode({99}), std::invalid_argument);
            EXPECT_EQ(pipeline.decode({0, 2}).weight, 4);
        });
    }
    EXPECT_THROW(pipeline.run_with_team([&] {
        EXPECT_EQ(pipeline.decode({0, 1, 2, 3}).weight, 10);
        throw std::runtime_error("abort single");
    }), std::runtime_error);
    pipeline.run_with_team([&] { EXPECT_EQ(pipeline.decode({1, 3}).weight, 6); });
}

TEST(DoubleBufferComponents, AffinityStartupFailureStopsEveryGroupBeforeRethrow) {
    auto graph = pair_graph();
    auto make = [&](int cpu) {
        return SharedComponentDecoder(ComponentDecoder(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false), {cpu});
    };
    DoubleBufferComponentDecoder pipeline(make(-1), make(std::numeric_limits<int>::max()));
    bool called = false;
    for (size_t attempt = 0; attempt < 2; ++attempt)
        EXPECT_THROW(pipeline.run_with_team([&] { called = true; }), std::invalid_argument);
    EXPECT_FALSE(called);
    auto moved = std::move(pipeline);
    EXPECT_THROW(moved.run_with_team([&] { called = true; }), std::invalid_argument);
    EXPECT_FALSE(called);
}

#if PM_STRUCTURED_PROFILE_TIMERS
TEST(DoubleBufferComponents, ProfilesPublishCompletedResetsWithoutWorkerStorageAliases) {
    auto graph = pair_graph();
    DoubleBufferComponentDecoder pipeline(
        make_group(graph, pair_spec(), 2, GroupGeometry::three_planes),
        make_group(graph, pair_spec(), 2, GroupGeometry::three_planes));
    pipeline.run_with_team([&] {
        for (size_t k = 0; k < 5; ++k) {
            EXPECT_EQ(pipeline.decode({0, 1, 2, 3}).weight, 10);
            const auto profile = pipeline.shot_profile();
            EXPECT_EQ(profile.shot, k);
            EXPECT_EQ(profile.buffer, k & 1);
            EXPECT_GT(profile.decode_ns, 0U);
#if PM_STRUCTURED_PROFILE_TIMELINE
            EXPECT_TRUE(profile.decode_interval.present);
            if (k & 1) EXPECT_TRUE(profile.result_wait_interval.present);
#endif
        }
        pipeline.drain();
        for (size_t b = 0; b < 2; ++b) {
            const auto profile = pipeline.completed_reset_profile(b);
            EXPECT_EQ(profile.shot, b == 0 ? 4U : 3U);
            EXPECT_EQ(profile.buffer, b);
            EXPECT_GT(profile.reset_ns, 0U);
#if PM_STRUCTURED_PROFILE_TIMELINE
            EXPECT_TRUE(profile.reset_interval.present);
#endif
        }
    });
}
#endif
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
// Tests mutate the process environment only while every worker team is stopped.
// Each decoder must retain its constructor choice for its entire lifetime.
class FixedSeedConstructionEnvironment {
    bool had_value;
    std::string previous;
   public:
    explicit FixedSeedConstructionEnvironment(const char* value) {
        const auto* old = std::getenv("PM_FIXED_SEED_CONSTRUCTION");
        had_value = old != nullptr;
        if (old) previous = old;
        set(value);
    }
    ~FixedSeedConstructionEnvironment() {
        if (had_value) setenv("PM_FIXED_SEED_CONSTRUCTION", previous.c_str(), 1);
        else unsetenv("PM_FIXED_SEED_CONSTRUCTION");
    }
    void set(const char* value) {
        if (value) setenv("PM_FIXED_SEED_CONSTRUCTION", value, 1);
        else unsetenv("PM_FIXED_SEED_CONSTRUCTION");
    }
};

std::vector<std::vector<uint64_t>> fixed_construction_shots(const StaticGridSpec& spec) {
    std::vector<std::vector<uint64_t>> shots(5);
    for (size_t n = 0; n < spec.coordinates.size(); ++n) {
        shots[0].push_back(n);
        if (n % 7 == 1) shots[1].push_back(n);
        if (n % 29 == 3) shots[2].push_back(n);
    }
    shots[3] = shots[1];
    std::reverse(shots[3].begin(), shots[3].end());
    return shots;
}

std::array<structured::TimePartitionCuts, 2> fixed_construction_failure_cuts;
std::array<GroupNeighborhood, 2> fixed_construction_neighborhood;
std::array<std::array<std::atomic<size_t>, 9>, 2> fixed_construction_visits;
std::array<uint32_t, 2> fixed_construction_fail_masks;
template <size_t C>
uint32_t fail_fixed_construction_bucket(
        const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    const auto bucket = fixed_construction_failure_cuts[C].bucket(cell);
    fixed_construction_visits[C][bucket].fetch_add(1, std::memory_order_relaxed);
    if (fixed_construction_fail_masks[C] & (uint32_t{1} << bucket))
        throw std::runtime_error("fixed construction injected bucket failure");
    return fixed_construction_neighborhood[C](grid, cell, boundary);
}
}

TEST(SharedComponents, FixedSeedConstructionFreezesModeAndRejectsInvalid) {
    FixedSeedConstructionEnvironment environment(nullptr);
    auto graph = pair_graph();
    auto fused = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
    EXPECT_STREQ(fused.fixed_seed_construction_name(), "fused");
    EXPECT_TRUE(fused.fixed_seed_construction_enabled());
    environment.set("conflict_first");
    auto split = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
    EXPECT_STREQ(split.fixed_seed_construction_name(), "conflict_first");
    EXPECT_TRUE(split.fixed_seed_construction_enabled());
    environment.set("unrecognized_mode");
    EXPECT_THROW(make_group(graph, pair_spec(), 4, GroupGeometry::three_planes), std::invalid_argument);
    // A now-invalid environment must not be consulted by existing hot paths.
    for (auto* decoder : {&fused, &split}) decoder->run_with_team([&] {
        decoder->prepare_for_mwpm({0, 1, 2, 3});
        EXPECT_EQ(decoder->solve_prepared().weight, 10);
        decoder->reset_after_shot();
        decoder->prepare_for_mwpm({});
        EXPECT_EQ(decoder->solve_prepared().weight, 0);
        decoder->reset_after_shot();
    });
    EXPECT_STREQ(fused.fixed_seed_construction_name(), "fused");
    EXPECT_STREQ(split.fixed_seed_construction_name(), "conflict_first");
    auto moved = std::move(split);
    EXPECT_STREQ(moved.fixed_seed_construction_name(), "conflict_first");
    EXPECT_TRUE(moved.fixed_seed_construction_enabled());
    moved.run_with_team([&] {
        moved.prepare_for_mwpm({0, 1, 2, 3});
        EXPECT_EQ(moved.solve_prepared().weight, 10);
        moved.reset_after_shot();
    });
}

TEST(SharedComponents, FixedSeedConstructionKeepsPreparedStateAcrossYtAndThreads) {
    FixedSeedConstructionEnvironment environment("fused");
    for (bool rotated : {true, false}) {
        auto [dem, spec] = model(rotated, 13, 'x');
        auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        auto oracle = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
        const auto shots = fixed_construction_shots(spec);
        std::vector<MatchingResult> generic;
        for (const auto& shot : shots)
            generic.push_back(decode_detection_events_for_up_to_64_observables(oracle, shot, false));
        for (size_t parts : {2, 4}) {
            const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
            std::vector<PreparedRegionSnapshot> prepared;
            std::vector<std::vector<uint64_t>> raw;
            std::vector<MatchingResult> expected;
            // 4/8 workers per decoder become total 8/16 in the two-buffer benchmark.
            for (const char* mode : {"fused", "conflict_first"}) for (size_t threads : {4, 8}) {
                environment.set(mode);
                auto decoder = make_group(graph, spec, threads, geometry, false, parts);
                ASSERT_TRUE(decoder.fixed_seed_construction_enabled());
                decoder.run_with_team([&] {
                    for (size_t repeat = 0; repeat < 2; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "mode=" << mode << " P=" << parts
                            << " group_threads=" << threads << " rotated=" << rotated
                            << " repeat=" << repeat << " shot=" << k);
                        const size_t index = repeat * shots.size() + k;
                        decoder.prepare_for_mwpm(shots[k]);
                        const auto actual = group_snapshot(decoder.serial_decoder());
                        const auto actual_raw = raw_group_seed_state(decoder.serial_decoder());
                        const bool reference = std::string(mode) == "fused" && threads == 4;
                        if (reference) { prepared.push_back(actual); raw.push_back(actual_raw); }
                        else {
                            EXPECT_EQ(actual.values, prepared[index].values);
                            EXPECT_EQ(actual.generations, prepared[index].generations);
                            EXPECT_EQ(actual_raw, raw[index]);
                        }
                        for (size_t c = 0; c < decoder.serial_decoder().components().size(); ++c) {
                            const auto& component = decoder.serial_decoder().components()[c];
                            const auto& grid = *component.seed_grid;
                            const structured::TimePartitionCuts cuts(grid.nx, grid.ny, grid.nt, parts, 0, geometry);
                            const auto ranges = component.mwpm.predecoder.seed_view.active_ranges();
                            ASSERT_EQ(ranges.size(), parts + 1);
                            for (size_t b = 0; b <= parts; ++b) {
                                if (k == 0) EXPECT_GT(ranges[b].count(), 0U);
                                for (const auto& seed : component.mwpm.predecoder.seed_view.storage.subspan(
                                         ranges[b].begin, ranges[b].count()))
                                    EXPECT_EQ(cuts.bucket(seed.cell), b);
                            }
                        }
                        const auto result = decoder.solve_prepared();
                        EXPECT_EQ(result.weight, generic[k].weight);
                        if (reference) expected.push_back(result);
                        else EXPECT_EQ(result, expected[index]);
                        decoder.reset_after_shot();
                        expect_clean_nodes(decoder.serial_decoder(), false);
                    }
                });
            }
        }
    }
}

TEST(SharedComponents, FixedSeedConstructionPreservesFallbackAndSignedRoutes) {
    FixedSeedConstructionEnvironment environment("conflict_first");
    auto graph = pair_graph();
    ComponentDecoder reference(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
    const std::vector<std::vector<uint64_t>> shots{{0, 1, 2, 3}, {}, {0, 1}, {0, 1, 2, 3}};
    std::vector<MatchingResult> expected;
    for (const auto& shot : shots) expected.push_back(reference.decode(shot));
    struct Config { size_t parts, component_threshold, shot_threshold; bool fixed; };
    for (const auto config : {Config{2, 0, 3, true}, Config{2, 100, 0, true}, Config{1, 0, 0, false}}) {
        auto decoder = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, false,
                                  config.parts, config.component_threshold, config.shot_threshold);
        EXPECT_EQ(decoder.fixed_seed_construction_enabled(), config.fixed);
        decoder.run_with_team([&] {
            for (size_t k = 0; k < shots.size(); ++k) {
                decoder.prepare_for_mwpm(shots[k]);
                if (config.shot_threshold && shots[k].size() < config.shot_threshold)
                    EXPECT_EQ(decoder.component_route(0), structured::SharedRoute::whole_serial);
                if (!config.fixed || config.component_threshold)
                    EXPECT_EQ(decoder.component_route(0), structured::SharedRoute::serial);
#if PM_STRUCTURED_PROFILE_TIMERS
                if (!config.fixed || config.component_threshold || shots[k].size() < config.shot_threshold)
                    EXPECT_FALSE(decoder.profile.component[0].fixed_spans);
#endif
                EXPECT_EQ(decoder.solve_prepared(), expected[k]);
                decoder.reset_after_shot();
                expect_clean_nodes(decoder.serial_decoder(), false);
            }
            EXPECT_THROW(decoder.prepare_for_mwpm({0, 0}), std::invalid_argument);
            EXPECT_THROW(decoder.prepare_for_mwpm({UINT64_MAX}), std::invalid_argument);
            decoder.prepare_for_mwpm(shots[0]);
            EXPECT_EQ(decoder.solve_prepared(), expected[0]);
            decoder.reset_after_shot();
        });
    }
    graph.add_or_merge_boundary_edge(2, {}, -1, -1);
    ComponentDecoder signed_reference(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
    auto signed_decoder = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
    EXPECT_FALSE(signed_decoder.fixed_seed_construction_enabled());
    expected.clear();
    for (const auto& shot : shots) expected.push_back(signed_reference.decode(shot));
    signed_decoder.run_with_team([&] {
        for (size_t k = 0; k < shots.size(); ++k) {
            EXPECT_EQ(signed_decoder.decode(shots[k]), expected[k]);
            expect_clean_nodes(signed_decoder.serial_decoder(), false);
        }
    });
}

TEST(SharedComponents, FixedSeedConstructionJoinsInteriorAndConflictFailures) {
    FixedSeedConstructionEnvironment environment("conflict_first");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shots = fixed_construction_shots(spec);
    const std::array<std::array<uint32_t, 2>, 5> failures{{{1, 0}, {8, 0}, {0, 2}, {0, 16}, {4, 8}}};
    for (size_t threads : {4, 8}) for (const auto failure : failures) {
        SCOPED_TRACE(::testing::Message() << "threads=" << threads << " failures=" << failure[0] << ',' << failure[1]);
        auto decoder = make_group(graph, spec, threads, GroupGeometry::ty_2x2, false, 4);
        fixed_construction_fail_masks = failure;
        for (size_t c = 0; c < 2; ++c) {
            auto& grid = *decoder.serial_decoder().components()[c].seed_grid;
            fixed_construction_failure_cuts[c] = structured::TimePartitionCuts(
                grid.nx, grid.ny, grid.nt, 4, 0, GroupGeometry::ty_2x2);
            fixed_construction_neighborhood[c] = grid.predecode.neighborhood;
            for (auto& visits : fixed_construction_visits[c]) visits.store(0, std::memory_order_relaxed);
            grid.predecode.neighborhood = c == 0 ? fail_fixed_construction_bucket<0> : fail_fixed_construction_bucket<1>;
            install_bound_probe(c, grid);
        }
        decoder.run_with_team([&] {
            EXPECT_THROW(decoder.prepare_for_mwpm(shots[0]), std::runtime_error);
            for (size_t c = 0; c < 2; ++c) {
                decoder.serial_decoder().components()[c].seed_grid->predecode.neighborhood = fixed_construction_neighborhood[c];
                restore_bound_probe(c, *decoder.serial_decoder().components()[c].seed_grid);
                for (size_t b = 0; b <= 4; ++b)
                    if (failure[c] & (1U << b)) EXPECT_GT(fixed_construction_visits[c][b].load(), 0U);
                EXPECT_FALSE(decoder.serial_decoder().components()[c].mwpm.predecoder.has_live_cell_state());
            }
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(shots[1]);
            EXPECT_NO_THROW(decoder.solve_prepared());
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder(), false);
        });
    }
}

TEST(DoubleBufferComponents, FixedSeedConstructionMatchesFusedAcrossBuffersAndThreads) {
    FixedSeedConstructionEnvironment environment("fused");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back({0});  // Whole-shot fallback on the opposite buffer parity.
    for (size_t parts : {2, 4}) {
        const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
        environment.set("fused");
        auto reference = make_group(graph, spec, 4, geometry, false, parts, 0, 3);
        std::vector<MatchingResult> expected;
        reference.run_with_team([&] {
            for (const auto& shot : shots) {
                reference.prepare_for_mwpm(shot);
                expected.push_back(reference.solve_prepared());
                reference.reset_after_shot();
            }
        });
        for (size_t threads : {4, 8}) {
            environment.set("conflict_first");
            DoubleBufferComponentDecoder pipeline(
                make_group(graph, spec, threads, geometry, false, parts, 0, 3),
                make_group(graph, spec, threads, geometry, false, parts, 0, 3));
            for (size_t b = 0; b < 2; ++b) {
                EXPECT_STREQ(pipeline.fixed_seed_construction_name(b), "conflict_first");
                EXPECT_TRUE(pipeline.fixed_seed_construction_enabled(b));
            }
            EXPECT_THROW(pipeline.fixed_seed_construction_name(2), std::out_of_range);
            EXPECT_THROW(pipeline.fixed_seed_construction_enabled(2), std::out_of_range);
            environment.set("invalid_after_construction");
            pipeline.run_with_team([&] {
                for (size_t repeat = 0; repeat < 3; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                    SCOPED_TRACE(::testing::Message() << "total_threads=" << 2 * threads << " P=" << parts
                        << " repeat=" << repeat << " shot=" << k);
                    EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
#if PM_STRUCTURED_PROFILE_TIMERS
                    const auto profile = pipeline.shot_profile();
                    EXPECT_EQ(profile.buffer, (repeat * shots.size() + k) & 1);
                    if (k == 0) for (size_t c = 0; c < profile.component_count; ++c) {
                        EXPECT_TRUE(profile.shared.component[c].fixed_spans);
                        EXPECT_GT(profile.shared.component[c].fixed_bucket_seeds[parts], 0U);
                        EXPECT_GT(profile.partition[c][parts].degree_ns, 0U);
                    }
#endif
                }
                pipeline.drain();
                EXPECT_EQ(pipeline.next_buffer(), (3 * shots.size()) & 1);
            });
        }
    }
}

#if PM_STRUCTURED_PROFILE_TIMERS
TEST(SharedComponents, FixedSeedConstructionProfilesSeparateSharedAndInteriorBuckets) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment environment("fused");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shots = fixed_construction_shots(spec);
    for (const char* mode : {"fused", "conflict_first"}) for (size_t parts : {2, 4}) for (size_t threads : {4, 8}) {
        environment.set(mode);
        const bool split = std::string(mode) == "conflict_first";
        auto decoder = make_group(graph, spec, threads,
            parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes, false, parts);
        const auto addresses = Access::addresses(decoder.serial_decoder());
        decoder.run_with_team([&] {
            for (size_t k : {0, 1, 3}) {
                SCOPED_TRACE(::testing::Message() << "mode=" << mode << " P=" << parts
                    << " group_threads=" << threads << " shot=" << k);
                decoder.prepare_for_mwpm(shots[k]);
                for (size_t c = 0; c < 2; ++c) {
                    const auto& cp = decoder.profile.component[c];
                    ASSERT_TRUE(cp.fixed_spans);
                    EXPECT_GT(cp.fixed_predecode_including_seed_ns, 0U);
                    EXPECT_GE(cp.fixed_predecode_including_seed_ns, cp.conflict_ns);
                    EXPECT_GE(cp.group_prefix_ns, cp.fixed_predecode_including_seed_ns);
                    std::array<size_t, 9> counts{};
                    for (const auto global : shots[k]) if (addresses[global][0] == c) ++counts[addresses[global][1]];
                    EXPECT_EQ(cp.fixed_bucket_seeds, counts);
#if PM_STRUCTURED_PROFILE_COUNTERS
                    EXPECT_EQ(cp.conflict_seeds, counts[parts]);
                    EXPECT_EQ(cp.seeds, std::accumulate(counts.begin(), counts.end(), size_t{0}));
#endif
                    uint64_t degrees = 0, sorts = 0, growth = 0;
                    for (size_t b = 0; b <= parts; ++b) {
                        const auto& timing = decoder.partition_timing(c, b);
                        degrees += timing.degree_ns; sorts += timing.sort_ns; growth += timing.grow_match_ns;
                        if (k == 0) {
                            EXPECT_GT(counts[b], 0U);
                            EXPECT_GT(timing.degree_ns, 0U);
                            EXPECT_GT(timing.sort_ns, 0U);
                            EXPECT_GT(timing.grow_match_ns, 0U);
                        }
                    }
                    const auto& stats = decoder.serial_decoder().components()[c].mwpm.predecoder.stats;
                    EXPECT_EQ(stats.degree_ns, degrees);
                    EXPECT_EQ(stats.sort_ns, sorts);
                    EXPECT_EQ(stats.grow_match_ns, growth);
                    Access::BoundLayout layout;
                    layout.input_ordered = k != 3;
                    layout.offsets = Access::fixed_bases(decoder.serial_decoder())[c];
                    for (size_t lane = 0; lane < cp.active_threads; ++lane) {
                        const auto& lp = cp.lanes[lane];
                        uint32_t interior_mask = 0;
                        for (size_t b = lane; b < parts; b += cp.active_threads) interior_mask |= 1U << b;
                        const uint32_t conflict_mask = lane == 0 ? 1U << parts : 0;
                        const auto visits = [&](uint32_t mask) {
                            return Access::bounded_input(decoder.serial_decoder(), shots[k], layout, c, mask).size();
                        };
                        if (split && lane == 0) {
                            EXPECT_EQ(lp.fused_input_visits, 0U);
                            EXPECT_EQ(lp.interior_input_visits, visits(interior_mask));
                            EXPECT_EQ(lp.conflict_input_visits, visits(conflict_mask));
                            EXPECT_GT(lp.interior_construct_ns, 0U);
                            EXPECT_GT(lp.conflict_construct_ns, 0U);
                            EXPECT_EQ(lp.fused_construct_ns, 0U);
                        } else if (lane == 0) {
                            EXPECT_EQ(lp.fused_input_visits, visits(interior_mask | conflict_mask));
                            EXPECT_EQ(lp.interior_input_visits, 0U);
                            EXPECT_EQ(lp.conflict_input_visits, 0U);
                            EXPECT_GT(lp.fused_construct_ns, 0U);
                            EXPECT_EQ(lp.interior_construct_ns, 0U);
                            EXPECT_EQ(lp.conflict_construct_ns, 0U);
                        } else {
                            EXPECT_EQ(lp.fused_input_visits, 0U);
                            EXPECT_EQ(lp.interior_input_visits, visits(interior_mask));
                            EXPECT_EQ(lp.conflict_input_visits, 0U);
                            EXPECT_EQ(lp.fused_construct_ns, 0U);
                            EXPECT_GT(lp.interior_construct_ns, 0U);
                            EXPECT_EQ(lp.conflict_construct_ns, 0U);
                        }
                        EXPECT_EQ(lp.bounded_input_visits,
                                  lp.fused_input_visits + lp.interior_input_visits + lp.conflict_input_visits);
                        EXPECT_GE(lp.group_seed_scatter_ns,
                                  lp.fused_construct_ns + lp.interior_construct_ns + lp.conflict_construct_ns);
                    }
                }
                EXPECT_NO_THROW(decoder.solve_prepared());
                decoder.reset_after_shot();
            }
        });
    }
}
#endif
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
class FixedSeedInputEnvironment {
    bool had_value;
    std::string previous;
   public:
    explicit FixedSeedInputEnvironment(const char* value) {
        const auto* old = std::getenv("PM_FIXED_SEED_INPUT");
        had_value = old != nullptr;
        if (old) previous = old;
        set(value);
    }
    ~FixedSeedInputEnvironment() {
        if (had_value) setenv("PM_FIXED_SEED_INPUT", previous.c_str(), 1);
        else unsetenv("PM_FIXED_SEED_INPUT");
    }
    void set(const char* value) {
        if (value) setenv("PM_FIXED_SEED_INPUT", value, 1);
        else unsetenv("PM_FIXED_SEED_INPUT");
    }
};
}

TEST(SharedComponents, IndexedSeedInputFreezesModeAndAllocatesOnlyForFixedRoute) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment environment(nullptr);
    auto graph = pair_graph();
    auto scan = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
    EXPECT_STREQ(scan.fixed_seed_input_name(), "scan");
    EXPECT_EQ(scan.fixed_seed_index_entries(), 0U);
    EXPECT_EQ(Access::index_storage(scan.serial_decoder()).capacity(), 0U);
    environment.set("indexed");
    auto indexed = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
    EXPECT_STREQ(indexed.fixed_seed_input_name(), "indexed");
    EXPECT_EQ(indexed.fixed_seed_index_entries(), 4U);
    for (const auto config : {std::pair{size_t{1}, size_t{0}}, std::pair{size_t{2}, size_t{100}}}) {
        auto fallback = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, false,
                                   config.first, config.second);
        EXPECT_STREQ(fallback.fixed_seed_input_name(), "indexed");
        const size_t expected_entries = config.first == 1 ? 0 : 4;
        EXPECT_EQ(fallback.fixed_seed_index_entries(), expected_entries);
        EXPECT_GE(Access::index_storage(fallback.serial_decoder()).capacity(), expected_entries);
        fallback.run_with_team([&] { EXPECT_EQ(fallback.decode({0, 1, 2, 3}).weight, 10); });
    }
    auto signed_graph = pair_graph();
    signed_graph.add_or_merge_boundary_edge(2, {}, -1, -1);
    auto signed_decoder = make_group(signed_graph, pair_spec(), 4, GroupGeometry::three_planes);
    EXPECT_EQ(signed_decoder.fixed_seed_index_entries(), 0U);
    environment.set("invalid_input_mode");
    EXPECT_THROW(make_group(graph, pair_spec(), 4, GroupGeometry::three_planes), std::invalid_argument);
    for (auto* decoder : {&scan, &indexed}) decoder->run_with_team([&] {
        EXPECT_EQ(decoder->decode({0, 1, 2, 3}).weight, 10);
        EXPECT_EQ(decoder->decode({}).weight, 0);
    });
    EXPECT_STREQ(scan.fixed_seed_input_name(), "scan");
    EXPECT_STREQ(indexed.fixed_seed_input_name(), "indexed");
}

TEST(SharedComponents, IndexedSeedInputPreservesPreparedSeedsAcrossGeometryAndThreads) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment environment("scan");
    for (bool rotated : {true, false}) {
        auto [dem, spec] = model(rotated, 13, 'x');
        auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        auto oracle = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
        auto shots = fixed_construction_shots(spec);
        shots.push_back(shots[0]);  // Refill every bucket after the empty shot.
        std::vector<MatchingResult> generic;
        for (const auto& shot : shots)
            generic.push_back(decode_detection_events_for_up_to_64_observables(oracle, shot, false));
        for (size_t parts : {2, 4}) {
            const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
            std::vector<PreparedRegionSnapshot> prepared;
            std::vector<std::vector<uint64_t>> raw;
            std::vector<MatchingResult> expected;
            // Two buffers multiply these group team sizes to total 8/16 threads.
            for (const char* mode : {"scan", "indexed"}) for (size_t threads : {4, 8}) {
                environment.set(mode);
                auto decoder = make_group(graph, spec, threads, geometry, false, parts);
                const bool indexed = std::string(mode) == "indexed";
                const auto& serial = decoder.serial_decoder();
                const auto addresses = Access::addresses(serial);
                const auto* seed_backing = Access::retired(serial).data();
                const auto* index_backing = Access::index_storage(serial).data();
                const auto index_capacity = Access::index_storage(serial).capacity();
                EXPECT_EQ(decoder.fixed_seed_index_entries(), indexed ? Access::retired(serial).size() : 0U);
                decoder.run_with_team([&] {
                    for (size_t k = 0; k < shots.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "mode=" << mode << " P=" << parts
                            << " total_threads=" << 2 * threads << " rotated=" << rotated << " shot=" << k);
                        decoder.prepare_for_mwpm(shots[k]);
                        const auto actual = group_snapshot(serial);
                        const auto actual_raw = raw_group_seed_state(serial);
                        const bool reference = !indexed && threads == 4;
                        if (reference) { prepared.push_back(actual); raw.push_back(actual_raw); }
                        else {
                            EXPECT_EQ(actual.values, prepared[k].values);
                            EXPECT_EQ(actual.generations, prepared[k].generations);
                            EXPECT_EQ(actual_raw, raw[k]);
                        }
                        EXPECT_EQ(serial.shot_seeds.data(), seed_backing);
                        EXPECT_EQ(Access::index_storage(serial).data(), index_backing);
                        EXPECT_EQ(Access::index_storage(serial).capacity(), index_capacity);
                        for (size_t c = 0; c < serial.components().size(); ++c) {
                            const auto& view = serial.components()[c].mwpm.predecoder.seed_view;
                            EXPECT_EQ(view.storage.data(), seed_backing + Access::component_bases(serial)[c]);
                            ASSERT_EQ(view.active_ranges().size(), parts + 1);
                            for (size_t b = 0; b <= parts; ++b) {
                                std::vector<uint32_t> expected_ids;
                                for (auto id : shots[k])
                                    if (addresses[id][0] == c && addresses[id][1] == b)
                                        expected_ids.push_back(uint32_t(id));
                                const auto range = view.active_ranges()[b];
                                ASSERT_EQ(range.count(), expected_ids.size());
                                if (indexed) {
                                    EXPECT_EQ(Access::index_counts(serial)[c][b], expected_ids.size());
                                    const auto offset = Access::component_bases(serial)[c] + Access::fixed_bases(serial)[c][b];
                                    for (size_t i = 0; i < expected_ids.size(); ++i)
                                        EXPECT_EQ(Access::index_storage(serial)[offset + i], expected_ids[i]);
                                }
                                for (size_t i = 0; i < expected_ids.size(); ++i) {
                                    const auto& seed = view.storage[range.begin + i];
                                    EXPECT_EQ(seed.detector_id, addresses[expected_ids[i]][2]);
                                    EXPECT_EQ(seed.cell, addresses[expected_ids[i]][3]);
                                }
                            }
                        }
                        const auto result = decoder.solve_prepared();
                        EXPECT_EQ(result.weight, generic[k].weight);
                        if (reference) expected.push_back(result);
                        else EXPECT_EQ(result, expected[k]);
                        decoder.reset_after_shot();
                        EXPECT_EQ(Access::retired(serial).data(), seed_backing);
                        expect_clean_nodes(serial, false);
                    }
                });
            }
        }
    }
}

TEST(SharedComponents, IndexedSeedInputValidationAndReusePreserveErrorOrder) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment environment("scan");
    auto graph = pair_graph();
    const std::vector<std::pair<std::vector<uint64_t>, std::string>> failures{
        {{0, 0}, "Duplicate detection event in predecoder input."},
        {{3, 1, 3}, "Duplicate detection event in predecoder input."},
        {{0, 0, 9, 4}, "Detection event index 9 is not in the graph."},
        {{3, 2, 4, 99}, "Detection event index 4 is not in the graph."},
        {{99, 98}, "Detection event index 99 is not in the graph."},
        {{UINT64_MAX}, "Detection event index " + std::to_string(UINT64_MAX) + " is not in the graph."}};
    for (const char* mode : {"scan", "indexed"}) {
        environment.set(mode);
        auto decoder = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes);
        decoder.run_with_team([&] {
            for (const auto& [shot, message] : failures) {
                SCOPED_TRACE(::testing::Message() << "mode=" << mode << " expected=" << message);
                try {
                    decoder.prepare_for_mwpm(shot);
                    ADD_FAILURE() << "Invalid shot was accepted";
                    decoder.reset_after_shot();
                } catch (const std::invalid_argument& error) {
                    EXPECT_EQ(error.what(), message);
                }
                expect_clean_nodes(decoder.serial_decoder(), false);
                decoder.prepare_for_mwpm({3, 1, 2, 0});
                EXPECT_EQ(decoder.solve_prepared().weight, 10);
                decoder.reset_after_shot();
                decoder.prepare_for_mwpm({});
                EXPECT_EQ(decoder.solve_prepared().weight, 0);
                decoder.reset_after_shot();
            }
            Access::epoch(decoder.serial_decoder(), UINT32_MAX);
            Access::stamp_all(decoder.serial_decoder(), 1);
            decoder.prepare_for_mwpm({3, 1, 2, 0});
            EXPECT_EQ(Access::epoch(decoder.serial_decoder()), 1U);
            EXPECT_EQ(decoder.solve_prepared().weight, 10);
            decoder.reset_after_shot();
        });
    }
    // Explicit boundary IDs have NO_COMPONENT and may repeat without becoming
    // seeds. Repeated relevant IDs must still fail in the same mixed input.
    graph.nodes.resize(5);
    graph.set_boundary({4});
    auto spec = pair_spec();
    spec.coordinates.push_back({0, 0, 2});
    for (const char* mode : {"scan", "indexed"}) {
        environment.set(mode);
        auto decoder = make_group(graph, spec, 4, GroupGeometry::three_planes);
        EXPECT_EQ(decoder.fixed_seed_index_entries(), std::string(mode) == "indexed" ? 4U : 0U);
        decoder.run_with_team([&] {
            for (const std::vector<uint64_t> shot : {
                     std::vector<uint64_t>{4, 3, 1, 2, 0, 4}, std::vector<uint64_t>{0, 1, 2, 3, 4, 4}}) {
                decoder.prepare_for_mwpm(shot);
                size_t seed_count = 0;
                for (const auto& component : decoder.serial_decoder().components())
                    seed_count += component.mwpm.predecoder.seed_view.active_count();
                EXPECT_EQ(seed_count, 4U);
                EXPECT_EQ(decoder.solve_prepared().weight, 10);
                decoder.reset_after_shot();
            }
            EXPECT_THROW(decoder.prepare_for_mwpm({4, 0, 4, 0}), std::invalid_argument);
            decoder.prepare_for_mwpm({4, 4});
            EXPECT_EQ(decoder.solve_prepared().weight, 0);
            decoder.reset_after_shot();
            expect_clean_nodes(decoder.serial_decoder(), false);
        });
    }
}

TEST(SharedComponents, IndexedSeedInputJoinsWorkerFailuresAndPreservesMoves) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment environment("indexed");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shots = fixed_construction_shots(spec);
    for (size_t threads : {4, 8}) for (const char* mode : {"fused", "conflict_first"}) {
        construction.set(mode);
        auto decoder = make_group(graph, spec, threads, GroupGeometry::ty_2x2, false, 4);
        fixed_construction_fail_masks = {1U, 16U};  // Interior and shared-region failures in different components.
        for (size_t c = 0; c < 2; ++c) {
            auto& grid = *decoder.serial_decoder().components()[c].seed_grid;
            fixed_construction_failure_cuts[c] = structured::TimePartitionCuts(
                grid.nx, grid.ny, grid.nt, 4, 0, GroupGeometry::ty_2x2);
            fixed_construction_neighborhood[c] = grid.predecode.neighborhood;
            for (auto& visits : fixed_construction_visits[c]) visits.store(0, std::memory_order_relaxed);
            grid.predecode.neighborhood = c == 0 ? fail_fixed_construction_bucket<0> : fail_fixed_construction_bucket<1>;
            install_bound_probe(c, grid);
        }
        const auto* index_backing = Access::index_storage(decoder.serial_decoder()).data();
        const auto index_entries = decoder.fixed_seed_index_entries();
        MatchingResult expected;
        std::vector<uint64_t> raw;
        decoder.run_with_team([&] {
            EXPECT_THROW(decoder.prepare_for_mwpm(shots[0]), std::runtime_error);
            for (size_t c = 0; c < 2; ++c) {
                decoder.serial_decoder().components()[c].seed_grid->predecode.neighborhood = fixed_construction_neighborhood[c];
                restore_bound_probe(c, *decoder.serial_decoder().components()[c].seed_grid);
            }
            expect_clean_nodes(decoder.serial_decoder());
            decoder.prepare_for_mwpm(shots[1]);
            raw = raw_group_seed_state(decoder.serial_decoder());
            expected = decoder.solve_prepared();
            decoder.reset_after_shot();
        });
        SharedComponentDecoder moved(std::move(decoder));
        EXPECT_STREQ(moved.fixed_seed_input_name(), "indexed");
        EXPECT_EQ(moved.fixed_seed_index_entries(), index_entries);
        EXPECT_EQ(Access::index_storage(moved.serial_decoder()).data(), index_backing);
        moved.run_with_team([&] {
            moved.prepare_for_mwpm(shots[1]);
            EXPECT_EQ(raw_group_seed_state(moved.serial_decoder()), raw);
            EXPECT_EQ(moved.solve_prepared(), expected);
            moved.reset_after_shot();
            EXPECT_THROW(moved.prepare_for_mwpm({0, 0}), std::invalid_argument);
            EXPECT_EQ(moved.decode(shots[1]), expected);
        });
    }
}

TEST(DoubleBufferComponents, IndexedSeedInputPreservesBuffersAndRecovery) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment environment("scan");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back({0});  // Exercise whole-shot fallback on both parities.
    for (size_t parts : {2, 4}) {
        const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
        environment.set("scan");
        auto reference = make_group(graph, spec, 4, geometry, false, parts, 0, 3);
        std::vector<MatchingResult> expected;
        reference.run_with_team([&] {
            for (const auto& shot : shots) expected.push_back(reference.decode(shot));
        });
        for (size_t threads : {4, 8}) {
            environment.set("indexed");
            DoubleBufferComponentDecoder initial(
                make_group(graph, spec, threads, geometry, false, parts, 0, 3),
                make_group(graph, spec, threads, geometry, false, parts, 0, 3));
            DoubleBufferComponentDecoder pipeline(std::move(initial));
            for (size_t b = 0; b < 2; ++b) {
                EXPECT_STREQ(pipeline.fixed_seed_input_name(b), "indexed");
                EXPECT_GT(pipeline.fixed_seed_index_entries(b), 0U);
            }
            EXPECT_THROW(pipeline.fixed_seed_input_name(2), std::out_of_range);
            EXPECT_THROW(pipeline.fixed_seed_index_entries(2), std::out_of_range);
            environment.set("invalid_after_construction");
            pipeline.run_with_team([&] {
                for (size_t repeat = 0; repeat < 2; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                    SCOPED_TRACE(::testing::Message() << "P=" << parts << " total_threads=" << 2 * threads
                        << " repeat=" << repeat << " shot=" << k);
                    EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
#if PM_STRUCTURED_PROFILE_TIMERS
                    EXPECT_EQ(pipeline.shot_profile().shared.index_input_used, shots[k].size() >= 3);
#endif
                }
                EXPECT_THROW(pipeline.decode({0, 0, UINT64_MAX}), std::invalid_argument);
                EXPECT_EQ(pipeline.decode(shots[1]), expected[1]);
                EXPECT_THROW(pipeline.decode({0, 0, 0}), std::invalid_argument);
                EXPECT_EQ(pipeline.decode(shots[3]), expected[3]);
                pipeline.drain();
            });
        }
    }
}

#if PM_STRUCTURED_PROFILE_TIMERS
TEST(SharedComponents, IndexedSeedInputProfilesOneClassificationAndOneIndexVisit) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment environment("indexed");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shots = fixed_construction_shots(spec);
    for (const char* mode : {"fused", "conflict_first"}) for (size_t parts : {2, 4}) for (size_t threads : {4, 8}) {
        construction.set(mode);
        const bool split = std::string(mode) == "conflict_first";
        auto decoder = make_group(graph, spec, threads,
            parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes, false, parts);
        const auto addresses = Access::addresses(decoder.serial_decoder());
        decoder.run_with_team([&] {
            for (size_t k : {0, 1, 3, 4}) {
                SCOPED_TRACE(::testing::Message() << "mode=" << mode << " P=" << parts
                    << " total_threads=" << 2 * threads << " shot=" << k);
                decoder.prepare_for_mwpm(shots[k]);
                const auto& profile = decoder.profile;
                EXPECT_TRUE(profile.index_input_used);
                size_t valid_seeds = 0, all_lane_visits = 0;
                for (size_t c = 0; c < decoder.serial_decoder().components().size(); ++c) {
                    const auto& cp = profile.component[c];
                    std::array<size_t, 9> counts{};
                    for (auto id : shots[k]) if (addresses[id][0] == c) ++counts[addresses[id][1]];
                    EXPECT_EQ(cp.fixed_bucket_seeds, counts);
                    for (auto count : counts) valid_seeds += count;
                    for (size_t lane = 0; lane < cp.active_threads; ++lane) {
                        const auto& lp = cp.lanes[lane];
                        size_t interior_count = 0;
                        for (size_t b = lane; b < parts; b += cp.active_threads) interior_count += counts[b];
                        const auto conflict_count = lane == 0 ? counts[parts] : 0U;
                        EXPECT_EQ(lp.bounded_input_visits, interior_count + conflict_count);
                        EXPECT_EQ(lp.fused_input_visits, lane == 0 && !split ? interior_count + conflict_count : 0U);
                        EXPECT_EQ(lp.interior_input_visits, lane != 0 || split ? interior_count : 0U);
                        EXPECT_EQ(lp.conflict_input_visits, split ? conflict_count : 0U);
                        all_lane_visits += lp.bounded_input_visits;
                    }
                }
                EXPECT_EQ(all_lane_visits, valid_seeds);
                if (k == 3) {
                    EXPECT_GT(profile.index_classification_visits, shots[k].size());
                    EXPECT_GE(profile.index_entries_written, valid_seeds);
                } else {
                    EXPECT_EQ(profile.index_classification_visits, shots[k].size());
                    EXPECT_EQ(profile.index_entries_written, valid_seeds);
                }
                EXPECT_NO_THROW(decoder.solve_prepared());
                decoder.reset_after_shot();
            }
        });
    }
}
#endif
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
class ImportPreparationEnvironment {
    bool had_value;
    std::string previous;
   public:
    explicit ImportPreparationEnvironment(const char* value) {
        const auto* old = std::getenv("PM_IMPORT_PREPARATION");
        had_value = old != nullptr;
        if (old) previous = old;
        set(value);
    }
    ~ImportPreparationEnvironment() {
        if (had_value) setenv("PM_IMPORT_PREPARATION", previous.c_str(), 1);
        else unsetenv("PM_IMPORT_PREPARATION");
    }
    void set(const char* value) {
        if (value) setenv("PM_IMPORT_PREPARATION", value, 1);
        else unsetenv("PM_IMPORT_PREPARATION");
    }
};
using ImportMatchingNext = decltype(std::declval<structured::OneHopPredecoder::Grid>().matching_next);
std::array<ImportMatchingNext, 2> import_original_matching_next;
std::array<std::atomic<size_t>, 2> import_event_visits;
uint32_t import_event_fail_mask = 0;
template <size_t C>
GridNextEvent fail_import_event_preparation(
        const structured::OneHopPredecoder::Grid& grid,
        const structured::DetectorNode& node, const VaryingCT* radii) {
    import_event_visits[C].fetch_add(1, std::memory_order_relaxed);
    if (import_event_fail_mask & (1U << C))
        throw std::runtime_error(C == 0 ? "C0 import event failure" : "C1 import event failure");
    return import_original_matching_next[C](grid, node, radii);
}
}

TEST(SharedComponents, ImportPreparationFreezesModeAndRetainsFallbacks) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment environment(nullptr);
    auto graph = pair_graph();
    auto serial = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true);
    EXPECT_STREQ(serial.import_preparation_name(), "serial");
    EXPECT_FALSE(serial.import_preparation_enabled());
    environment.set("parallel");
    auto parallel = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true);
    EXPECT_STREQ(parallel.import_preparation_name(), "parallel");
    EXPECT_TRUE(parallel.import_preparation_enabled());
    // Selecting the preparation mode must not force parallel import or fixed
    // storage onto configurations that deliberately use another route.
    auto disabled = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, false);
    EXPECT_FALSE(disabled.import_preparation_enabled());
    for (const auto config : {std::pair{size_t{1}, size_t{0}}, std::pair{size_t{2}, size_t{100}}}) {
        SCOPED_TRACE(::testing::Message() << "partitions=" << config.first << " threshold=" << config.second);
        auto fallback = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true,
                                   config.first, config.second);
        EXPECT_STREQ(fallback.import_preparation_name(), "parallel");
        // P1 retains PC's serial fixed backing without enabling Spatial's
        // fixed-partition preparation, even when its requested mode is parallel.
        if (config.first == 1)
            EXPECT_GT(fallback.serial_decoder().serial_seed_storage_capacity(), 0U);
        EXPECT_EQ(fallback.fixed_seed_construction_enabled(), config.first != 1);
        EXPECT_EQ(fallback.import_preparation_enabled(), config.first != 1);
        fallback.run_with_team([&] {
            EXPECT_EQ(fallback.decode({0, 1, 2, 3}).weight, 10);
            EXPECT_EQ(fallback.decode({}).weight, 0);
        });
    }
    auto signed_graph = pair_graph();
    signed_graph.add_or_merge_boundary_edge(2, {}, -1, -1);
    ComponentDecoder signed_reference(signed_graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
    const auto signed_result = signed_reference.decode({0, 1});
    auto signed_decoder = make_group(signed_graph, pair_spec(), 4, GroupGeometry::three_planes, true);
    EXPECT_FALSE(signed_decoder.import_preparation_enabled());
    signed_decoder.run_with_team([&] { EXPECT_EQ(signed_decoder.decode({0, 1}), signed_result); });
    environment.set("invalid_import_preparation");
    EXPECT_THROW(make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true), std::invalid_argument);
    for (auto* decoder : {&serial, &parallel, &disabled}) decoder->run_with_team([&] {
        EXPECT_EQ(decoder->decode({0, 1, 2, 3}).weight, 10);
        EXPECT_EQ(decoder->decode({}).weight, 0);
    });
    EXPECT_STREQ(serial.import_preparation_name(), "serial");
    EXPECT_STREQ(parallel.import_preparation_name(), "parallel");
    SharedComponentDecoder moved(std::move(parallel));
    EXPECT_STREQ(moved.import_preparation_name(), "parallel");
    EXPECT_TRUE(moved.import_preparation_enabled());
    moved.run_with_team([&] { EXPECT_EQ(moved.decode({0, 1, 2, 3}).weight, 10); });
}

TEST(SharedComponents, ImportPreparationPreservesPreparedStateAcrossLayoutsAndWorkers) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment environment("serial");
    for (bool rotated : {true, false}) {
        auto [dem, spec] = model(rotated, 13, 'x');
        auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
        auto oracle = detector_error_model_to_mwpm(dem, NUM_DISTINCT_WEIGHTS, true);
        auto shots = fixed_construction_shots(spec);
        shots.push_back(shots[2]);
        std::mt19937 rng(2026102202);
        std::shuffle(shots.back().begin(), shots.back().end(), rng);
        std::vector<MatchingResult> generic;
        for (const auto& shot : shots)
            generic.push_back(decode_detection_events_for_up_to_64_observables(oracle, shot, false));
        for (size_t parts : {2, 4}) {
            const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
            std::vector<PreparedRegionSnapshot> prepared;
            std::vector<std::vector<uint64_t>> seeds;
            std::vector<MatchingResult> expected;
            // Compare original I0, existing region-only I1, and extended I1.
            // Four/eight team threads give two/four workers per component;
            // P2 caps active lanes at two, while TY4 exercises four lanes.
            for (size_t variant = 0; variant < 3; ++variant) for (size_t threads : {4, 8}) {
                environment.set(variant == 2 ? "parallel" : "serial");
                auto decoder = make_group(graph, spec, threads, geometry, variant != 0, parts);
                size_t imports = 0, roots = 0, events = 0;
                decoder.run_with_team([&] {
                    for (size_t repeat = 0; repeat < 2; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                        SCOPED_TRACE(::testing::Message() << "variant=" << variant << " P=" << parts
                            << " team_threads=" << threads << " rotated=" << rotated
                            << " repeat=" << repeat << " shot=" << k);
                        const size_t index = repeat * shots.size() + k;
                        decoder.prepare_for_mwpm(shots[k]);
                        const auto actual = group_snapshot(decoder.serial_decoder());
                        const auto raw = raw_group_seed_state(decoder.serial_decoder());
                        const bool reference = variant == 0 && threads == 4;
                        if (reference) { prepared.push_back(actual); seeds.push_back(raw); }
                        else {
                            EXPECT_EQ(actual.values, prepared[index].values);
                            EXPECT_EQ(actual.generations, prepared[index].generations);
                            EXPECT_EQ(raw, seeds[index]);
                        }
                        for (const auto& component : decoder.serial_decoder().components()) {
                            const auto& mwpm = component.mwpm;
                            if (mwpm.predecoder.complete) continue;
                            ++imports;
                            roots += mwpm.active_tree_count();
                            events += mwpm.flooder.queue.size();
                        }
                        const auto result = decoder.solve_prepared();
                        EXPECT_EQ(result.weight, generic[k].weight);
                        if (reference) expected.push_back(result);
                        else EXPECT_EQ(result, expected[index]);
                        decoder.reset_after_shot();
                        expect_clean_nodes(decoder.serial_decoder(), false);
                    }
                });
                // Prevent complete-predecode shots from vacuously passing the
                // prepared tree/queue comparison without entering import.
                EXPECT_GT(imports, 0U);
                EXPECT_GT(roots, 0U);
                EXPECT_GT(events, 0U);
            }
        }
    }
}

TEST(SharedComponents, ImportPreparationJoinsFailuresAndRecovers) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment environment("parallel");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shot = fixed_construction_shots(spec)[2];
    environment.set("serial");
    std::array<MatchingResult, 2> expected;
    for (size_t parts : {2, 4}) {
        auto reference = make_group(graph, spec, 4,
            parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes, false, parts);
        reference.run_with_team([&] { expected[parts / 2 - 1] = reference.decode(shot); });
    }
    environment.set("parallel");
    for (size_t parts : {2, 4}) for (size_t threads : {4, 8}) for (uint32_t failed : {1, 2, 3}) {
        SCOPED_TRACE(::testing::Message() << "P=" << parts << " team_threads=" << threads << " failed=" << failed);
        auto decoder = make_group(graph, spec, threads,
            parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes, true, parts);
        import_event_fail_mask = failed;
        for (size_t c = 0; c < 2; ++c) {
            auto& grid = *decoder.serial_decoder().components()[c].seed_grid;
            import_original_matching_next[c] = grid.matching_next;
            import_event_visits[c].store(0, std::memory_order_relaxed);
            grid.matching_next = c == 0 ? fail_import_event_preparation<0> : fail_import_event_preparation<1>;
        }
        decoder.run_with_team([&] {
            try {
                decoder.prepare_for_mwpm(shot);
                ADD_FAILURE() << "Import event preparation should fail";
                decoder.reset_after_shot();
            } catch (const std::runtime_error& error) {
                EXPECT_STREQ(error.what(), failed & 1 ? "C0 import event failure" : "C1 import event failure");
            }
            // Reaching here requires all helpers to have joined. Every root
            // constructed before a failed event calculation must be retired.
            for (size_t c = 0; c < 2; ++c) {
                auto& component = decoder.serial_decoder().components()[c];
                component.seed_grid->matching_next = import_original_matching_next[c];
                EXPECT_GT(import_event_visits[c].load(std::memory_order_relaxed), 0U);
                EXPECT_TRUE(component.mwpm.flooder.regions->empty());
                EXPECT_TRUE(component.mwpm.flooder.queue.empty());
                EXPECT_EQ(component.mwpm.node_arena.allocated.size(), component.mwpm.node_arena.available.size());
                EXPECT_EQ(component.mwpm.active_tree_count(), 0U);
            }
            expect_clean_nodes(decoder.serial_decoder());
            for (size_t repeat = 0; repeat < 3; ++repeat) {
                EXPECT_EQ(decoder.decode(shot), expected[parts / 2 - 1]);
                EXPECT_EQ(decoder.decode({}).weight, 0);
                EXPECT_THROW(decoder.decode({0, 0, UINT64_MAX}), std::invalid_argument);
            }
        });
    }
    // A captain-side reserve failure must join the other component's two new
    // import jobs before cleanup, just as a worker-side event failure does.
    for (size_t failed : {0, 1}) {
        auto decoder = make_group(graph, spec, 4, GroupGeometry::three_planes, true);
        auto& pool = *decoder.serial_decoder().components()[failed].mwpm.flooder.regions;
        structured::RegionPoolTestAccess::set_generation(pool, 1, std::numeric_limits<structured::RegionGeneration>::max());
        decoder.run_with_team([&] {
            EXPECT_THROW(decoder.prepare_for_mwpm(shot), std::overflow_error);
            expect_clean_nodes(decoder.serial_decoder());
            structured::RegionPoolTestAccess::set_generation(pool, 1, 0);
            EXPECT_EQ(decoder.decode(shot), expected[0]);
        });
    }
}

TEST(DoubleBufferComponents, ImportPreparationPreservesBuffersAndRecovery) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment environment("serial");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back({0});
    shots.push_back(shots[2]);  // Odd sequence length alternates every shot's buffer.
    for (size_t parts : {2, 4}) {
        const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
        environment.set("serial");
        auto reference = make_group(graph, spec, 4, geometry, false, parts, 0, 3);
        std::vector<MatchingResult> expected;
        reference.run_with_team([&] {
            for (const auto& shot : shots) expected.push_back(reference.decode(shot));
        });
        for (size_t threads : {4, 8}) {
            environment.set("parallel");
            DoubleBufferComponentDecoder initial(
                make_group(graph, spec, threads, geometry, true, parts, 0, 3),
                make_group(graph, spec, threads, geometry, true, parts, 0, 3));
            DoubleBufferComponentDecoder pipeline(std::move(initial));
            for (size_t b = 0; b < 2; ++b) {
                EXPECT_STREQ(pipeline.import_preparation_name(b), "parallel");
                EXPECT_TRUE(pipeline.import_preparation_enabled(b));
            }
            EXPECT_THROW(pipeline.import_preparation_name(2), std::out_of_range);
            EXPECT_THROW(pipeline.import_preparation_enabled(2), std::out_of_range);
            environment.set("invalid_after_construction");
            pipeline.run_with_team([&] {
                for (size_t repeat = 0; repeat < 5; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                    SCOPED_TRACE(::testing::Message() << "P=" << parts << " total_threads=" << 2 * threads
                        << " repeat=" << repeat << " shot=" << k);
                    EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
                }
                EXPECT_THROW(pipeline.decode({0, 0, UINT64_MAX}), std::invalid_argument);
                EXPECT_EQ(pipeline.decode(shots[1]), expected[1]);
                EXPECT_THROW(pipeline.decode({0, 0, 0}), std::invalid_argument);
                EXPECT_EQ(pipeline.decode(shots[2]), expected[2]);
                pipeline.drain();
            });
        }
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(SharedComponents, ImportPreparationHandlesCrossLanePairsAndEmptyRootLanes) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment environment("serial");
    BackendUserGraph graph(54, 2);
    StaticGridSpec spec{9, 27, false, {}, {}, GridSimd::scalar};
    for (size_t t = 0; t < 27; ++t) for (size_t c = 0; c < 2; ++c) {
        spec.coordinates.push_back({0, 0, int64_t(t)});
        spec.groups.push_back(c);
        if (t) graph.add_or_merge_edge(2 * (t - 1) + c, 2 * t + c, {}, 2, -1);
        if (t == 0 || t == 26) graph.add_or_merge_boundary_edge(2 * t + c, {c}, 8, -1);
    }
    // In the first shot each component has a low-time matched pair and one
    // high-time root. Dense rank split 3/2 crosses the matched pair; lane zero
    // has no roots, lane one owns the sole root. Fixed storage retains holes
    // between these low/high/conflict bucket prefixes.
    const std::vector<std::vector<uint64_t>> shots{
        {8, 9, 10, 11, 40, 41}, {8, 9, 10, 11}, {40, 41}, {}, {8, 9, 10, 11, 40, 41}};
    std::vector<PreparedRegionSnapshot> states;
    std::vector<MatchingResult> expected;
    for (size_t variant = 0; variant < 3; ++variant) {
        environment.set(variant == 2 ? "parallel" : "serial");
        auto decoder = make_group(graph, spec, 4, GroupGeometry::three_planes, variant != 0);
        decoder.run_with_team([&] {
            for (size_t k = 0; k < shots.size(); ++k) {
                SCOPED_TRACE(::testing::Message() << "variant=" << variant << " shot=" << k);
                decoder.prepare_for_mwpm(shots[k]);
                const auto actual = group_snapshot(decoder.serial_decoder());
                if (variant == 0) states.push_back(actual);
                else {
                    EXPECT_EQ(actual.values, states[k].values);
                    EXPECT_EQ(actual.generations, states[k].generations);
                }
                if (k == 0 || k == 4) for (const auto& component : decoder.serial_decoder().components()) {
                    const auto& pre = component.mwpm.predecoder;
                    ASSERT_FALSE(pre.complete);
                    ASSERT_EQ(pre.seed_view.active_count(), 3U);
                    std::array<size_t, 2> roots{};
                    bool crossed_pair = false;
                    for (const auto& range : pre.seed_view.active_ranges()) {
                        for (size_t slot = range.begin; slot < range.end; ++slot) {
                            const auto& seed = pre.seed_view.storage[slot];
                            const auto rank = pre.seed_view.rank_of(slot);
                            const size_t lane = rank < 1 ? 0 : 1;
                            if (seed.mate == structured::PredecodeSeed::UNMATCHED) ++roots[lane];
                            else if (pre.seed_view.contains(seed.mate))
                                crossed_pair |= lane != (pre.seed_view.rank_of(seed.mate) < 1 ? 0U : 1U);
                        }
                    }
                    EXPECT_TRUE(crossed_pair);
                    EXPECT_EQ(roots[0], 0U);
                    EXPECT_EQ(roots[1], 1U);
                    EXPECT_EQ(component.mwpm.active_tree_count(), 1U);
                    EXPECT_EQ(component.mwpm.flooder.queue.size(), 1U);
                }
                const auto result = decoder.solve_prepared();
                if (variant == 0) expected.push_back(result);
                else EXPECT_EQ(result, expected[k]);
                decoder.reset_after_shot();
                expect_clean_nodes(decoder.serial_decoder(), false);
            }
        });
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
class ImportResetOwnerEnvironment {
    bool had_value;
    std::string previous;
   public:
    explicit ImportResetOwnerEnvironment(const char* value) {
        const auto* old = std::getenv("PM_IMPORT_RESET_OWNER");
        had_value = old != nullptr;
        if (old) previous = old;
        set(value);
    }
    ~ImportResetOwnerEnvironment() {
        if (had_value) setenv("PM_IMPORT_RESET_OWNER", previous.c_str(), 1);
        else unsetenv("PM_IMPORT_RESET_OWNER");
    }
    void set(const char* value) {
        if (value) setenv("PM_IMPORT_RESET_OWNER", value, 1);
        else unsetenv("PM_IMPORT_RESET_OWNER");
    }
};
void expect_import_reset_complete(const ComponentDecoder& decoder) {
    expect_clean_nodes(decoder, false);
    for (const auto& component : decoder.components()) {
        const auto& mwpm = component.mwpm;
        EXPECT_TRUE(mwpm.flooder.regions->empty());
        EXPECT_FALSE(mwpm.flooder.regions->has_pending_neutral_reset());
        EXPECT_TRUE(mwpm.flooder.queue.empty());
        EXPECT_EQ(mwpm.active_tree_count(), 0U);
        EXPECT_EQ(mwpm.node_arena.allocated.size(), mwpm.node_arena.available.size());
        EXPECT_FALSE(mwpm.predecoder.has_live_cell_state());
    }
}
}

TEST(SharedComponents, ImportResetOwnerFreezesMode) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("parallel");
    ImportResetOwnerEnvironment ownership(nullptr);
    auto graph = pair_graph();
    auto importers = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true);
    EXPECT_STREQ(importers.import_reset_owner_name(), "importers");
    ownership.set("captain");
    auto captain = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true);
    EXPECT_STREQ(captain.import_reset_owner_name(), "captain");
    // This policy must remain usable when import has only one writer.
    auto serial = make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, false);
    EXPECT_STREQ(serial.import_reset_owner_name(), "captain");
    ownership.set("invalid_owner");
    EXPECT_THROW(make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true), std::invalid_argument);
    for (auto* decoder : {&importers, &captain, &serial}) decoder->run_with_team([&] {
        for (size_t repeat = 0; repeat < 2; ++repeat) {
            decoder->prepare_for_mwpm({0, 1, 2, 3});
            EXPECT_EQ(decoder->solve_prepared().weight, 10);
            decoder->reset_after_shot();
            expect_import_reset_complete(decoder->serial_decoder());
            EXPECT_EQ(decoder->decode({}).weight, 0);
        }
    });
    EXPECT_STREQ(importers.import_reset_owner_name(), "importers");
    SharedComponentDecoder moved(std::move(captain));
    EXPECT_STREQ(moved.import_reset_owner_name(), "captain");
    moved.run_with_team([&] { EXPECT_EQ(moved.decode({0, 1, 2, 3}).weight, 10); });
}

TEST(SharedComponents, ImportResetOwnerPreservesStateAndReuse) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("serial");
    ImportResetOwnerEnvironment ownership("importers");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back(shots[2]);
    std::reverse(shots.back().begin(), shots.back().end());
    for (size_t parts : {2, 4}) {
        const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
        std::vector<PreparedRegionSnapshot> prepared;
        std::vector<std::vector<uint64_t>> raw_seeds;
        std::vector<MatchingResult> expected;
        // All import routes, including seed-only overlapped events, retain identical
        // imported state, including generations after the previous reset.
        for (const char* policy : {"importers", "captain"}) for (size_t variant = 0; variant < 7; ++variant) {
            ownership.set(policy);
            preparation.set(variant == 6 ? "captained_overlap" : variant == 5 ? "captained_reverse" :
                variant == 4 ? "captained_serial" : variant == 3 ? "captained" : variant == 2 ? "parallel" : "serial");
            auto decoder = make_group(graph, spec, 4, geometry, variant != 0, parts);
            size_t imports = 0, pending_cleanups = 0;
            const bool reference = std::string_view(policy) == "importers" && variant == 0;
            decoder.run_with_team([&] {
                for (size_t repeat = 0; repeat < 3; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                    SCOPED_TRACE(::testing::Message() << "owner=" << policy << " variant=" << variant
                        << " P=" << parts << " repeat=" << repeat << " shot=" << k);
                    const size_t index = repeat * shots.size() + k;
                    decoder.prepare_for_mwpm(shots[k]);
                    const auto actual = group_snapshot(decoder.serial_decoder());
                    const auto raw = raw_group_seed_state(decoder.serial_decoder());
                    if (reference) { prepared.push_back(actual); raw_seeds.push_back(raw); }
                    else {
                        EXPECT_EQ(actual.values, prepared[index].values);
                        EXPECT_EQ(actual.generations, prepared[index].generations);
                        EXPECT_EQ(raw, raw_seeds[index]);
                    }
                    for (const auto& component : decoder.serial_decoder().components())
                        imports += !component.mwpm.predecoder.complete;
                    const auto result = decoder.solve_prepared();
                    if (reference) expected.push_back(result);
                    else EXPECT_EQ(result, expected[index]);
                    for (const auto& component : decoder.serial_decoder().components())
                        pending_cleanups += component.mwpm.has_pending_seed_cleanup();
                    decoder.reset_after_shot();
                    expect_import_reset_complete(decoder.serial_decoder());
                }
                EXPECT_THROW(decoder.decode({0, 0, UINT64_MAX}), std::invalid_argument);
                expect_import_reset_complete(decoder.serial_decoder());
                EXPECT_EQ(decoder.decode(shots[2]), expected[2]);
            });
            // Exercise the imported detector-node reset path, not just a
            // complete-predecode shortcut with no ownership to transfer.
            EXPECT_GT(imports, 0U);
            EXPECT_GT(pending_cleanups, 0U);
        }
    }
}

TEST(DoubleBufferComponents, ImportResetOwnerPreservesBuffersAndRecovery) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("serial");
    ImportResetOwnerEnvironment ownership("importers");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back({0});
    shots.push_back(shots[2]);
    auto reference = make_group(graph, spec, 4, GroupGeometry::three_planes, false);
    std::vector<MatchingResult> expected;
    reference.run_with_team([&] {
        for (const auto& shot : shots) expected.push_back(reference.decode(shot));
    });
    for (const char* policy : {"importers", "captain"}) for (size_t variant = 0; variant < 7; ++variant) {
        ownership.set(policy);
        preparation.set(variant == 6 ? "captained_overlap" : variant == 5 ? "captained_reverse" :
            variant == 4 ? "captained_serial" : variant == 3 ? "captained" : variant == 2 ? "parallel" : "serial");
        auto even = make_group(graph, spec, 4, GroupGeometry::three_planes, variant != 0);
        auto odd = make_group(graph, spec, 4, GroupGeometry::three_planes, variant != 0);
        std::array<structured::OneHopPredecoder::Grid*, 4> grids;
        for (size_t c = 0; c < 2; ++c) {
            grids[c] = even.serial_decoder().components()[c].seed_grid;
            grids[2 + c] = odd.serial_decoder().components()[c].seed_grid;
            import_original_matching_next[c] = grids[c]->matching_next;
        }
        DoubleBufferComponentDecoder initial(std::move(even), std::move(odd));
        DoubleBufferComponentDecoder pipeline(std::move(initial));
        for (size_t b = 0; b < 2; ++b) EXPECT_STREQ(pipeline.import_reset_owner_name(b), policy);
        EXPECT_THROW(pipeline.import_reset_owner_name(2), std::out_of_range);
        // Both buffer states retain their constructor policy after the
        // environment becomes invalid; no worker may read it during reset.
        ownership.set("invalid_after_construction");
        pipeline.run_with_team([&] {
            for (size_t repeat = 0; repeat < 3; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                SCOPED_TRACE(::testing::Message() << "owner=" << policy << " variant=" << variant
                    << " repeat=" << repeat << " shot=" << k);
                EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
            }
            EXPECT_THROW(pipeline.decode({0, 0, UINT64_MAX}), std::invalid_argument);
            EXPECT_EQ(pipeline.decode(shots[2]), expected[2]);
            if (variant >= 2 && variant != 6) {
                // An exception while preparing initial events must join both
                // buffers' workers and allow captain or importer reset reuse.
                pipeline.drain();
                import_event_fail_mask = 3;
                for (size_t c = 0; c < 2; ++c) {
                    import_event_visits[c].store(0, std::memory_order_relaxed);
                    grids[c]->matching_next = c == 0 ? fail_import_event_preparation<0> : fail_import_event_preparation<1>;
                    grids[2 + c]->matching_next = grids[c]->matching_next;
                }
                EXPECT_THROW(pipeline.decode(shots[2]), std::runtime_error);
                for (size_t c = 0; c < 2; ++c) {
                    EXPECT_GT(import_event_visits[c].load(std::memory_order_relaxed), 0U);
                    grids[c]->matching_next = import_original_matching_next[c];
                    grids[2 + c]->matching_next = import_original_matching_next[c];
                }
                import_event_fail_mask = 0;
                EXPECT_EQ(pipeline.decode(shots[2]), expected[2]);
                EXPECT_EQ(pipeline.decode(shots[2]), expected[2]);
            }
            pipeline.drain();
        });
        for (size_t b = 0; b < 2; ++b) expect_import_reset_complete(pipeline.serial_decoder(b));
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
std::array<std::atomic<size_t>, 8> captained_event_thread_visits;
size_t captained_event_fail_thread = SIZE_MAX;
template <size_t C>
GridNextEvent fail_captained_event_thread(
        const structured::OneHopPredecoder::Grid& grid,
        const structured::DetectorNode& node, const VaryingCT* radii) {
    const size_t thread = omp_get_thread_num();
    captained_event_thread_visits.at(thread).fetch_add(1, std::memory_order_relaxed);
    if (thread == captained_event_fail_thread)
        throw std::runtime_error("captained import event failure");
    return import_original_matching_next[C](grid, node, radii);
}
}

TEST(SharedComponents, CaptainedImportFreezesModeAndJoinsEventFailures) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("serial");
    ImportResetOwnerEnvironment ownership("captain");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shot = fixed_construction_shots(spec)[2];
    auto reference = make_group(graph, spec, 4, GroupGeometry::three_planes, false);
    MatchingResult expected;
    PreparedRegionSnapshot expected_state;
    reference.run_with_team([&] {
        reference.prepare_for_mwpm(shot);
        expected_state = group_snapshot(reference.serial_decoder());
        for (const auto& component : reference.serial_decoder().components())
            ASSERT_GT(component.mwpm.active_tree_count(), 1U);
        expected = reference.solve_prepared();
        reference.reset_after_shot();
    });
    for (const char* mode : {"captained", "captained_serial", "captained_reverse"}) {
        preparation.set(mode);
        ownership.set("captain");
        auto decoder = make_group(graph, spec, 4, GroupGeometry::three_planes, true);
        EXPECT_STREQ(decoder.import_preparation_name(), mode);
        EXPECT_TRUE(decoder.import_preparation_enabled());
        auto disabled = make_group(graph, spec, 4, GroupGeometry::three_planes, false);
        EXPECT_STREQ(disabled.import_preparation_name(), mode);
        EXPECT_FALSE(disabled.import_preparation_enabled());
        preparation.set("invalid_after_construction");
        ownership.set("invalid_after_construction");
        for (size_t c = 0; c < 2; ++c) {
            auto& grid = *decoder.serial_decoder().components()[c].seed_grid;
            import_original_matching_next[c] = grid.matching_next;
            grid.matching_next = c == 0 ? fail_captained_event_thread<0> : fail_captained_event_thread<1>;
        }
        decoder.run_with_team([&] {
            for (auto& visits : captained_event_thread_visits) visits.store(0, std::memory_order_relaxed);
            decoder.prepare_for_mwpm(shot);
            const auto actual = group_snapshot(decoder.serial_decoder());
            EXPECT_EQ(actual.values, expected_state.values);
            EXPECT_EQ(actual.generations, expected_state.generations);
            EXPECT_EQ(decoder.solve_prepared(), expected);
            decoder.reset_after_shot();
            expect_import_reset_complete(decoder.serial_decoder());
            // Fail only workers that actually calculate events. Parallel event
            // preparation includes helpers; captained_serial must exclude them.
            std::vector<size_t> participating;
            for (size_t thread = 0; thread < decoder.team_size(); ++thread)
                if (captained_event_thread_visits[thread].load(std::memory_order_relaxed)) participating.push_back(thread);
            if (std::string_view(mode) == "captained_serial") {
                EXPECT_EQ(participating.size(), PM_SHARED_COMPONENT_PARALLELISM == 2 ? 2U : 1U);
                for (const size_t thread : participating)
                    EXPECT_TRUE(thread == group_captain(0, 4) || thread == group_captain(1, 4));
            } else {
                EXPECT_GE(participating.size(), 2U);
            }
            for (const size_t thread : participating) {
                SCOPED_TRACE(::testing::Message() << "failed event worker=" << thread);
                captained_event_fail_thread = thread;
                EXPECT_THROW(decoder.prepare_for_mwpm(shot), std::runtime_error);
                expect_import_reset_complete(decoder.serial_decoder());
                captained_event_fail_thread = SIZE_MAX;
                EXPECT_EQ(decoder.decode(shot), expected);
                EXPECT_EQ(decoder.decode({}).weight, 0);
            }
        });
        disabled.run_with_team([&] { EXPECT_EQ(disabled.decode(shot), expected); });
    }
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(SharedComponents, PositiveThresholdAlternatesFixedLazyAndPrepartitionFallback) {
    using Access = structured::ComponentDecoderTestAccess;
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("captained_serial");
    ImportResetOwnerEnvironment ownership("captain");
    auto graph = pair_graph();
    ComponentDecoder reference(graph, pair_spec(), NUM_DISTINCT_WEIGHTS, false);
    // Threshold 2 keeps the complete two-endpoint match eligible while each
    // one-endpoint component must retain its original serial preparation.
    const std::vector<std::vector<uint64_t>> shots{
        {0, 1, 2, 3}, {0, 1}, {0, 1, 2}, {0, 1, 3}, {0}, {},
        {0, 1, 2, 3}, {3, 2, 1, 0}, {0, 1}, {0, 1, 2, 3}};
    std::vector<MatchingResult> expected;
    for (const auto& shot : shots) expected.push_back(reference.decode(shot));
    for (size_t threads : {1, 2, 4}) {
        auto decoder = make_group(graph, pair_spec(), threads, GroupGeometry::three_planes, true, 2, 2, 2);
        ASSERT_TRUE(decoder.fixed_seed_construction_enabled());
        ASSERT_TRUE(decoder.import_preparation_enabled());
        ASSERT_EQ(decoder.fixed_seed_index_entries(), 4U);
        const auto addresses = Access::addresses(decoder.serial_decoder());
        decoder.run_with_team([&] {
            for (size_t repeat = 0; repeat < 2; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                SCOPED_TRACE(::testing::Message() << "threads=" << threads << " repeat=" << repeat << " shot=" << k);
                std::array<size_t, 2> counts{};
                for (auto id : shots[k]) ++counts.at(addresses.at(id)[0]);
                const bool whole_serial = shots[k].size() < 2;
                const bool fixed = counts[0] >= 2 && counts[1] >= 2;
                decoder.prepare_for_mwpm(shots[k]);
                size_t deferred = 0;
                bool imported = false;
                for (size_t c = 0; c < 2; ++c) {
                    const auto route = whole_serial ? structured::SharedRoute::whole_serial
                        : counts[c] < 2 ? structured::SharedRoute::serial : structured::SharedRoute::partitioned;
                    EXPECT_EQ(decoder.component_route(c), route);
                    if (route != structured::SharedRoute::partitioned)
                        EXPECT_EQ(decoder.active_predecode_threads(c), 1U);
                    const auto& component = decoder.serial_decoder().components()[c];
                    const auto& seeds = component.mwpm.predecoder.seed_view;
                    const bool captained = fixed && !component.mwpm.predecoder.complete;
                    imported |= captained;
                    EXPECT_EQ(seeds.active_ranges().size(), fixed ? 3U : 1U);
                    const auto& pool = *component.mwpm.flooder.regions;
                    for (structured::RegionId id = 1; id < structured::RegionPoolTestAccess::next(pool); ++id)
                        deferred += pool.is_seed_node_deferred(id);
#if PM_STRUCTURED_PROFILE_TIMERS
                    EXPECT_EQ(decoder.profile.component[c].fixed_spans, fixed);
                    EXPECT_EQ(decoder.profile.component[c].import_preparation_captained, captained);
#if PM_SHARED_COMPONENT_PARALLELISM == 2
                    if (fixed && threads > 1)
                        EXPECT_EQ(decoder.profile.component[c].group_worker, c * (threads / 2));
#endif
#endif
                }
                if (imported) EXPECT_GT(deferred, 0U);
                EXPECT_EQ(decoder.solve_prepared(), expected[k]);
                decoder.reset_after_shot();
                expect_import_reset_complete(decoder.serial_decoder());
            }
            EXPECT_THROW(decoder.prepare_for_mwpm({0, 0, 1, 3}), std::invalid_argument);
            EXPECT_THROW(decoder.prepare_for_mwpm({UINT64_MAX, 0, 1, 2}), std::invalid_argument);
            // These errors occur after both components have met the gate.
            EXPECT_THROW(decoder.prepare_for_mwpm({0, 1, 2, 3, UINT64_MAX}), std::invalid_argument);
            EXPECT_THROW(decoder.prepare_for_mwpm({0, 1, 2, 3, 0}), std::invalid_argument);
            decoder.prepare_for_mwpm(shots.front());
            EXPECT_EQ(decoder.solve_prepared(), expected.front());
            decoder.reset_after_shot();
        });
    }
    DoubleBufferComponentDecoder pipeline(
        make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true, 2, 2, 2),
        make_group(graph, pair_spec(), 4, GroupGeometry::three_planes, true, 2, 2, 2));
    pipeline.run_with_team([&] {
        // Rotate the second traversal so each route reaches the other buffer.
        for (size_t repeat = 0; repeat < 2; ++repeat)
            for (size_t k = 0; k < shots.size(); ++k) {
                const size_t index = (k + repeat) % shots.size();
                EXPECT_EQ(pipeline.decode(shots[index]), expected[index]);
            }
        pipeline.drain();
    });
}

TEST(SharedComponents, PositiveThresholdImportsLazyMatchesWithResidualRoots) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("captained_serial");
    ImportResetOwnerEnvironment ownership("captain");
    BackendUserGraph graph(6, 2);
    graph.add_or_merge_edge(0, 2, {0}, 2, -1);
    graph.add_or_merge_edge(2, 4, {}, 2, -1);
    graph.add_or_merge_edge(1, 3, {1}, 3, -1);
    graph.add_or_merge_edge(3, 5, {}, 3, -1);
    graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
    graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
    auto spec = pair_spec();
    spec.coordinates.push_back({4, 0, 0});
    spec.coordinates.push_back({4, 0, 0});
    const std::vector<uint64_t> high{0, 1, 2, 3, 4, 5};
    const std::vector<uint64_t> mixed{0, 1, 2, 4};
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    const auto expected_high = reference.decode(high), expected_mixed = reference.decode(mixed);
    for (size_t threads : {1, 4}) {
        SCOPED_TRACE(::testing::Message() << "threads=" << threads);
        auto decoder = make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, 2, 2);
        decoder.run_with_team([&] {
            for (size_t repeat = 0; repeat < 3; ++repeat) {
                decoder.prepare_for_mwpm(high);
                size_t deferred = 0, roots = 0;
                for (size_t c = 0; c < 2; ++c) {
                    const auto& mwpm = decoder.serial_decoder().components()[c].mwpm;
                    EXPECT_FALSE(mwpm.predecoder.complete);
                    for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
                        for (const auto& seed : mwpm.predecoder.seed_view.storage.subspan(range.begin, range.count()))
                            roots += seed.mate == structured::PredecodeSeed::UNMATCHED;
                    const auto& pool = *mwpm.flooder.regions;
                    for (structured::RegionId id = 1; id < structured::RegionPoolTestAccess::next(pool); ++id)
                        deferred += pool.is_seed_node_deferred(id);
#if PM_STRUCTURED_PROFILE_TIMERS
                    EXPECT_TRUE(decoder.profile.component[c].fixed_spans);
                    EXPECT_TRUE(decoder.profile.component[c].import_preparation_captained);
#endif
                }
                EXPECT_GT(roots, 0U);
                EXPECT_GT(deferred, 0U);
                EXPECT_EQ(decoder.solve_prepared(), expected_high);
                decoder.reset_after_shot();
                expect_import_reset_complete(decoder.serial_decoder());
                decoder.prepare_for_mwpm(mixed);
                EXPECT_EQ(decoder.component_route(0), structured::SharedRoute::partitioned);
                EXPECT_EQ(decoder.component_route(1), structured::SharedRoute::serial);
                EXPECT_EQ(decoder.solve_prepared(), expected_mixed);
                decoder.reset_after_shot();
                expect_import_reset_complete(decoder.serial_decoder());
            }
        });
    }
}

TEST(SharedComponents, SeedOnlyImportEventsMatchMwpmAcrossFixedHolesAndLanes) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("captained_serial");
    ImportResetOwnerEnvironment ownership("captain");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back({});
    shots.push_back({0});
    bool saw_holes = false, saw_roots = false, saw_empty_lane = false;
    std::vector<GridSimd> backends{GridSimd::scalar};
    if (grid_avx512vl_available()) backends.push_back(GridSimd::avx512vl);
    for (const auto backend : backends) for (size_t parts : {2, 4, 8}) {
        spec.simd = backend;
        auto decoder = make_group(graph, spec, 4,
            parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes, true, parts);
        decoder.run_with_team([&] {
            for (const auto& shot : shots) {
                decoder.prepare_for_mwpm(shot);
                for (auto& component : decoder.serial_decoder().components()) {
                    auto& mwpm = component.mwpm;
                    const auto& seeds = mwpm.predecoder.seed_view;
                    const size_t count = seeds.active_count();
                    if (seeds.active_ranges().size() != parts + 1) continue;
                    saw_holes |= seeds.storage.size() > count;
                    for (size_t lanes : {1, 2, 3}) {
                        for (size_t lane = 0; lane < lanes; ++lane) {
                            const size_t begin = count * lane / lanes, end = count * (lane + 1) / lanes;
                            std::vector<structured::PredecodeImportRoot> expected;
                            seeds.for_rank_range(begin, end, [&](size_t lo, size_t hi, size_t rank) {
                                for (size_t slot = lo; slot < hi; ++slot) {
                                    const auto& seed = seeds.storage[slot];
                                    if (seed.mate != structured::PredecodeSeed::UNMATCHED) continue;
                                    auto& node = mwpm.flooder.graph.node(seed.detector_id);
                                    const auto event = mwpm.flooder.prepare_initial_event_at_detector_node(node);
                                    expected.push_back({&node, structured::RegionId(1 + rank + slot - lo),
                                        event.second, event.first != SIZE_MAX});
                                }
                            });
                            saw_roots |= !expected.empty();
                            saw_empty_lane |= expected.empty();
                            structured::PredecodeImportScratch scratch;
                            scratch.roots.reserve(end - begin);
                            // Make the post-import representation unusable while
                            // retaining the immutable predecode representation.
                            // A seed-only helper must not dereference these node
                            // fields or read any collision state being constructed.
                            auto& grid = *component.seed_grid;
                            const auto original_states = grid.collision_states;
                            std::vector<std::pair<structured::DetectorNode*, size_t>> node_cells;
                            for (const auto& root : expected) {
                                node_cells.emplace_back(root.node, root.node->grid_cell);
                                root.node->grid_cell = SIZE_MAX;
                            }
                            for (auto& state : grid.collision_states) {
                                state.top_region_id = std::numeric_limits<structured::RegionId>::max();
                                state.wrapped_radius_cached = std::numeric_limits<int32_t>::min();
                            }
                            mwpm.prepare_fixed_predecode_events_from_seeds(seeds, 1, begin, end, scratch);
                            grid.collision_states = original_states;
                            for (const auto& [node, cell] : node_cells) node->grid_cell = cell;
                            ASSERT_EQ(scratch.roots.size(), expected.size());
                            for (size_t k = 0; k < expected.size(); ++k) {
                                EXPECT_EQ(scratch.roots[k].node, expected[k].node);
                                EXPECT_EQ(scratch.roots[k].region, expected[k].region);
                                EXPECT_EQ(scratch.roots[k].has_event, expected[k].has_event);
                                EXPECT_EQ(scratch.roots[k].event_time, expected[k].event_time);
                            }
                        }
                    }
                }
                decoder.reset_after_shot();
                expect_import_reset_complete(decoder.serial_decoder());
            }
        });
    }
    EXPECT_TRUE(saw_holes);
    EXPECT_TRUE(saw_roots);
    EXPECT_TRUE(saw_empty_lane);
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
void spatial_use_owned_import(ComponentDecoder& decoder) {
    for (auto& component : decoder.components())
        structured::SerialImportTestAccess::use_owned_packed_import(component.mwpm);
}
void expect_spatial_borrowed_extraction(const ComponentDecoder& decoder, bool used) {
    for (const auto& component : decoder.components()) {
        const auto& mwpm = component.mwpm;
        EXPECT_EQ(structured::SerialImportTestAccess::used_borrowed_extraction(mwpm), used);
        EXPECT_FALSE(mwpm.flooder.regions->has_borrowed_seeds());
        EXPECT_TRUE(mwpm.flooder.regions->changed_initial_seeds().empty());
        EXPECT_TRUE(mwpm.flooder.regions->empty());
    }
    expect_import_reset_complete(decoder);
}
#if PM_COMPONENT_OWNER_AUDIT
void expect_spatial_owner(const std::array<uint64_t, 3>& owner) {
    EXPECT_NE(owner[0], 0U);
    EXPECT_EQ(owner[0], owner[1]);
    EXPECT_EQ(owner[1], owner[2]);
}
#endif
}

TEST(SharedComponents, CaptainedBorrowedEntriesMatchOwnedAndKeepRankHoles) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportResetOwnerEnvironment ownership("captain");
    ImportPreparationEnvironment preparation("captained");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    // One interior detector per component stays unmatched after one hop and
    // guarantees empty fixed buckets without relying on sampling accidents.
    ComponentDecoder addresses(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<uint64_t> isolated;
    for (const auto& component : addresses.components()) {
        for (size_t id = 0; id < component.global_detectors.size(); ++id) {
            const auto& node = component.mwpm.flooder.graph.node(id);
            if (node.grid_component->boundary_mask[node.grid_cell] == 0) {
                isolated.push_back(component.global_detectors[id]);
                break;
            }
        }
    }
    ASSERT_EQ(isolated.size(), addresses.components().size());
    shots.push_back(isolated);
    shots.push_back(shots[2]);
    std::reverse(shots.back().begin(), shots.back().end());
    for (size_t parts : {2, 4, 8}) for (const char* mode : {"captained", "captained_serial", "captained_reverse", "captained_overlap"}) {
        preparation.set(mode);
        const auto geometry = parts == 4 ? GroupGeometry::ty_2x2 : GroupGeometry::three_planes;
        auto owned = make_group(graph, spec, 4, geometry, true, parts);
        spatial_use_owned_import(owned.serial_decoder());
        std::vector<MatchingResult> expected;
        owned.run_with_team([&] {
            for (const auto& shot : shots) {
                owned.prepare_for_mwpm(shot);
                for (const auto& component : owned.serial_decoder().components())
                    EXPECT_FALSE(component.mwpm.flooder.regions->has_borrowed_seeds());
                expected.push_back(owned.solve_prepared());
                owned.reset_after_shot();
            }
        });
        expect_spatial_borrowed_extraction(owned.serial_decoder(), false);
        auto candidate = make_group(graph, spec, 4, geometry, true, parts);
        size_t imports = 0, holes = 0, empty_buckets = 0;
        candidate.run_with_team([&] {
            for (size_t repeat = 0; repeat < 2; ++repeat) for (size_t k = 0; k < shots.size(); ++k) {
                SCOPED_TRACE(::testing::Message() << "P=" << parts << " mode=" << mode << " shot=" << k);
                candidate.prepare_for_mwpm(shots[k]);
                for (const auto& component : candidate.serial_decoder().components()) {
                    const auto& mwpm = component.mwpm;
                    const auto& view = mwpm.predecoder.seed_view;
                    if (mwpm.predecoder.complete || view.empty()) continue;
                    ASSERT_TRUE(mwpm.flooder.regions->has_borrowed_seeds());
                    ++imports;
                    holes += view.storage.size() > view.active_count();
                    for (const auto& range : view.active_ranges()) empty_buckets += range.count() == 0;
                    for (size_t rank = 0; rank < view.active_count(); ++rank)
                        EXPECT_EQ(&mwpm.flooder.regions->borrowed_seed(rank + 1), &view.storage[view.slot_of(rank)]);
                }
                EXPECT_EQ(candidate.solve_prepared(), expected[k]);
                candidate.reset_after_shot();
                expect_import_reset_complete(candidate.serial_decoder());
#if PM_COMPONENT_OWNER_AUDIT
                for (size_t c = 0; c < candidate.serial_decoder().components().size(); ++c)
                    expect_spatial_owner(candidate.lifecycle_thread_ids(c));
#endif
            }
            EXPECT_THROW(candidate.decode({0, 0, UINT64_MAX}), std::invalid_argument);
            candidate.prepare_for_mwpm(shots[2]);
            candidate.reset();
            candidate.prepare_for_mwpm(shots[2]);
            EXPECT_EQ(candidate.solve_prepared(), expected[2]);
            candidate.reset_after_shot();
        });
        EXPECT_GT(imports, 0U);
        EXPECT_GT(holes, 0U);
        EXPECT_GT(empty_buckets, 0U);
        expect_spatial_borrowed_extraction(candidate.serial_decoder(), true);
    }
}

TEST(DoubleBufferComponents, CaptainedBorrowedBuffersMatchOwnedAndRecover) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportResetOwnerEnvironment ownership("captain");
    ImportPreparationEnvironment preparation("captained");
    auto [dem, spec] = model(true, 13, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto shots = fixed_construction_shots(spec);
    shots.push_back(shots[2]);
    shots.push_back(shots[2]);
    for (const char* mode : {"captained", "captained_overlap"}) {
        preparation.set(mode);
        auto make = [&] { return make_group(graph, spec, 4, GroupGeometry::three_planes, true, 2); };
        auto a = make(), b = make();
        spatial_use_owned_import(a.serial_decoder());
        spatial_use_owned_import(b.serial_decoder());
        DoubleBufferComponentDecoder owned(std::move(a), std::move(b));
        std::vector<MatchingResult> expected;
        owned.run_with_team([&] {
            for (const auto& shot : shots) expected.push_back(owned.decode(shot));
            owned.drain();
        });
        DoubleBufferComponentDecoder candidate(make(), make());
        for (size_t repeat = 0; repeat < 2; ++repeat) {
            candidate.run_with_team([&] {
                for (size_t k = 0; k < shots.size(); ++k) EXPECT_EQ(candidate.decode(shots[k]), expected[k]);
                EXPECT_THROW(candidate.decode({0, 0, UINT64_MAX}), std::invalid_argument);
                EXPECT_EQ(candidate.decode(shots[2]), expected[2]);
                EXPECT_EQ(candidate.decode(shots[2]), expected[2]);
                candidate.drain();
            });
            for (size_t buffer = 0; buffer < 2; ++buffer) {
                expect_spatial_borrowed_extraction(candidate.serial_decoder(buffer), true);
                expect_spatial_borrowed_extraction(owned.serial_decoder(buffer), false);
#if PM_COMPONENT_OWNER_AUDIT
                for (size_t c = 0; c < candidate.serial_decoder(buffer).components().size(); ++c)
                    expect_spatial_owner(candidate.lifecycle_thread_ids(buffer, c));
#endif
            }
        }
        DoubleBufferComponentDecoder moved(std::move(candidate));
        moved.run_with_team([&] {
            EXPECT_EQ(moved.decode(shots[2]), expected[2]);
            moved.drain();
        });
        for (size_t buffer = 0; buffer < 2; ++buffer)
            expect_spatial_borrowed_extraction(moved.serial_decoder(buffer), true);
    }
}
#endif

// Retained P1 owner and borrowed-storage regressions from the 1103 PC/TailReset family.
#if defined(_OPENMP) && PM_PREDECODE_ENABLED
TEST(SharedComponents, P1FixedStorageAndLifecycleOwnersAcrossFallbacksAndErrors) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    SharedComponentDecoder shared(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
        {-1, -1}, 1, 64, 4, 2, false);
    ASSERT_GT(shared.serial_decoder().serial_seed_storage_capacity(), 0U);
    std::array<const structured::PredecodeSeed*, 2> bases{};
    std::mt19937_64 rng(2026110101);
    std::vector<std::vector<uint64_t>> shots{{}, {0}, {1}};
    for (size_t shot = 0; shot < 32; ++shot) {
        std::vector<uint64_t> events;
        for (size_t i = 0; i < graph.nodes.size(); ++i)
            if (rng() % 7 == 0) events.push_back(i);
        if (shot % 2) std::reverse(events.begin(), events.end());
        shots.push_back(events);
    }
    shared.run_with_team([&] {
        size_t imported = 0;
        for (const auto& events : shots) {
            shared.prepare_for_mwpm(events);
            for (size_t c = 0; c < 2; ++c) {
                const auto& pre = shared.serial_decoder().components()[c].mwpm.predecoder;
                if (!bases[c]) bases[c] = pre.seed_view.storage.data();
                EXPECT_EQ(pre.seed_view.storage.data(), bases[c]);
                for (const auto& layout : pre.scratch_layout()) {
                    EXPECT_EQ(layout.mask_address % 64, 0U);
                    EXPECT_EQ(layout.degree_address % 64, 0U);
                    EXPECT_EQ(layout.order_address % 64, 0U);
                }
                imported += !pre.complete;
            }
            EXPECT_EQ(shared.solve_prepared(), reference.decode(events));
            shared.reset_after_shot();
            expect_clean_nodes(shared.serial_decoder(), false);
#if PM_COMPONENT_OWNER_AUDIT && (defined(__linux__) || defined(__APPLE__)) && PM_SHARED_COMPONENT_PARALLELISM == 2
            const auto a = shared.lifecycle_thread_ids(0);
            const auto b = shared.lifecycle_thread_ids(1);
            for (auto ids : {a, b}) {
                EXPECT_NE(ids[0], 0U);
                EXPECT_EQ(ids[0], ids[1]);
                EXPECT_EQ(ids[0], ids[2]);
            }
            if (events.size() < 4) EXPECT_EQ(a[0], b[0]);
            else EXPECT_NE(a[0], b[0]);
#endif
        }
        EXPECT_GT(imported, 0U);
        // Implicit next-shot cleanup, unsolved prefix abandonment, and failed
        // validation must leave the fixed backing and every owner reusable.
        for (const auto& events : shots) EXPECT_EQ(shared.decode(events), reference.decode(events));
        shared.prepare_for_mwpm(shots.back());
        shared.reset_after_shot();
        expect_clean_nodes(shared.serial_decoder(), false);
        EXPECT_THROW(shared.prepare_for_mwpm({0, 0}), std::invalid_argument);
        EXPECT_THROW(shared.prepare_for_mwpm({uint64_t(graph.nodes.size())}), std::invalid_argument);
        EXPECT_EQ(shared.decode(shots.back()), reference.decode(shots.back()));
        shared.reset_after_shot();
        shared.reset_after_shot();
        expect_clean_nodes(shared.serial_decoder(), false);
    });
}

TEST(DoubleBufferComponents, P1OwnersRetainLifecycleAndStorageAcrossBothBuffers) {
    auto [dem, spec] = model(true, 5, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    auto make = [&] {
        return SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            {-1, -1}, 1, 64, 4, 2, false);
    };
    auto even = make(), odd = make();
    const auto* even_core = &even.serial_decoder();
    const auto* odd_core = &odd.serial_decoder();
    ASSERT_GT(even_core->serial_seed_storage_capacity(), 0U);
    ASSERT_EQ(even_core->serial_seed_storage_capacity(), odd_core->serial_seed_storage_capacity());
    DoubleBufferComponentDecoder pipeline(std::move(even), std::move(odd));
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<uint64_t> dense;
    for (size_t i = 0; i < graph.nodes.size(); ++i)
        if (i % 3 == 0) dense.push_back(i);
    std::vector<std::vector<uint64_t>> shots{{}, {0}, dense, {1}, dense, dense, dense};
    pipeline.run_with_team([&] {
        for (const auto& events : shots)
            EXPECT_EQ(pipeline.decode(events), reference.decode(events));
        pipeline.drain();
        expect_clean_nodes(*even_core, false);
        expect_clean_nodes(*odd_core, false);
    });
#if PM_COMPONENT_OWNER_AUDIT && (defined(__linux__) || defined(__APPLE__)) && PM_SHARED_COMPONENT_PARALLELISM == 2
    std::set<uint64_t> owners;
    for (size_t buffer = 0; buffer < 2; ++buffer)
        for (size_t c = 0; c < 2; ++c) {
            const auto ids = pipeline.lifecycle_thread_ids(buffer, c);
            EXPECT_NE(ids[0], 0U);
            EXPECT_EQ(ids[0], ids[1]);
            EXPECT_EQ(ids[0], ids[2]);
            owners.insert(ids[0]);
        }
    EXPECT_EQ(owners.size(), 4U);
#endif
}

#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED
namespace {
void use_owned_component_import(ComponentDecoder& decoder) {
    for (auto& component : decoder.components())
        structured::SerialImportTestAccess::use_owned_packed_import(component.mwpm);
}
void expect_borrowed_component_extraction(const ComponentDecoder& decoder, bool used) {
    for (const auto& component : decoder.components()) {
        const auto& mwpm = component.mwpm;
        EXPECT_EQ(structured::SerialImportTestAccess::used_borrowed_extraction(mwpm), used);
        EXPECT_FALSE(mwpm.flooder.regions->has_borrowed_seeds());
        EXPECT_TRUE(mwpm.flooder.regions->changed_initial_seeds().empty());
        EXPECT_TRUE(mwpm.flooder.regions->empty());
    }
    expect_clean_nodes(decoder, false);
}
std::vector<std::vector<uint64_t>> borrowed_surface_shots(size_t nodes) {
    std::mt19937_64 rng(2026110403);
    std::vector<std::vector<uint64_t>> shots(24);
    for (size_t k = 0; k < shots.size(); ++k) {
        for (size_t id = 0; id < nodes; ++id)
            if (rng() % (k % 2 ? 7 : 19) == 0) shots[k].push_back(id);
        if (k % 3 == 1) std::reverse(shots[k].begin(), shots[k].end());
    }
    shots.push_back({});
    shots.push_back({0});
    return shots;
}
}

TEST(SharedComponents, P1BorrowedNormalAndSplitEntriesMatchOwnedAfterRecovery) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shots = borrowed_surface_shots(spec.coordinates.size());
    for (size_t shot_threshold : {0, 32}) {
        auto make = [&] {
            return SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                {-1, -1}, 1, 64, shot_threshold, 2, false);
        };
        auto owned = make();
        use_owned_component_import(owned.serial_decoder());
        std::vector<MatchingResult> expected;
        owned.run_with_team([&] {
            for (const auto& shot : shots) expected.push_back(owned.decode(shot));
        });
        expect_borrowed_component_extraction(owned.serial_decoder(), false);
        auto candidate = make();
        candidate.run_with_team([&] {
            for (size_t k = 0; k < shots.size(); ++k)
                EXPECT_EQ(candidate.decode(shots[k]), expected[k]);
            // First prove the ordinary shared decode entry used borrowed import
            // on both owners before exercising its explicit split lifecycle.
            expect_borrowed_component_extraction(candidate.serial_decoder(), true);
            EXPECT_THROW(candidate.decode({0, 0}), std::invalid_argument);
            for (size_t k = 0; k < shots.size(); ++k) {
                candidate.prepare_for_mwpm(shots[k]);
                for (const auto& component : candidate.serial_decoder().components()) {
                    const auto& mwpm = component.mwpm;
                    if (!mwpm.predecoder.complete && !mwpm.predecoder.seed_view.empty())
                        EXPECT_TRUE(mwpm.flooder.regions->has_borrowed_seeds());
                }
                EXPECT_EQ(candidate.solve_prepared(), expected[k]);
                candidate.reset_after_shot();
            }
            candidate.reset();
            EXPECT_EQ(candidate.decode(shots[1]), expected[1]);
            // Ordinary decode retains this shot's seed backing until retirement.
            // Complete that lifecycle before reading all three owner phases;
            // otherwise phase 2 still describes the preceding hard reset.
            candidate.reset_after_shot();
#if PM_COMPONENT_OWNER_AUDIT && (defined(__linux__) || defined(__APPLE__)) && PM_SHARED_COMPONENT_PARALLELISM == 2
            std::set<uint64_t> owners;
            for (size_t c = 0; c < candidate.serial_decoder().components().size(); ++c) {
                const auto ids = candidate.lifecycle_thread_ids(c);
                EXPECT_NE(ids[0], 0U);
                EXPECT_EQ(ids[0], ids[1]);
                EXPECT_EQ(ids[0], ids[2]);
                owners.insert(ids[0]);
            }
            EXPECT_EQ(owners.size(), 2U);
#endif
        });
        expect_borrowed_component_extraction(candidate.serial_decoder(), true);
    }
}

TEST(DoubleBufferComponents, P1BorrowedExtractionRunsOnBothBuffersAndSurvivesErrorsAndMoves) {
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    const auto shots = borrowed_surface_shots(spec.coordinates.size());
    ComponentDecoder owned(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    use_owned_component_import(owned);
    std::vector<MatchingResult> expected;
    for (const auto& shot : shots) expected.push_back(owned.decode(shot));
    auto make = [&] {
        return SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
            {-1, -1}, 1, 64, 32, 2, false);
    };
    DoubleBufferComponentDecoder pipeline(make(), make());
    pipeline.run_with_team([&] {
        for (size_t k = 0; k < shots.size(); ++k)
            EXPECT_EQ(pipeline.decode(shots[k]), expected[k]);
        // Finish on a non-fallback input in each parity position so the
        // audit observes all four independent component owners.
        EXPECT_EQ(pipeline.decode(shots[1]), expected[1]);
        EXPECT_EQ(pipeline.decode(shots[1]), expected[1]);
        pipeline.drain();
    });
    for (size_t buffer = 0; buffer < 2; ++buffer)
        expect_borrowed_component_extraction(pipeline.serial_decoder(buffer), true);
#if PM_COMPONENT_OWNER_AUDIT && (defined(__linux__) || defined(__APPLE__)) && PM_SHARED_COMPONENT_PARALLELISM == 2
    std::set<uint64_t> owners;
    for (size_t buffer = 0; buffer < 2; ++buffer)
        for (size_t c = 0; c < 2; ++c) {
            const auto ids = pipeline.lifecycle_thread_ids(buffer, c);
            EXPECT_NE(ids[0], 0U);
            EXPECT_EQ(ids[0], ids[1]);
            EXPECT_EQ(ids[0], ids[2]);
            owners.insert(ids[0]);
        }
    EXPECT_EQ(owners.size(), 4U);
#endif
    DoubleBufferComponentDecoder moved(std::move(pipeline));
    // Odd-sized batches retain parity. Each failure drains both groups before
    // subsequent inputs reuse their immutable seed backing.
    for (size_t count : {1, 3, 7}) {
        moved.run_with_team([&] {
            EXPECT_THROW(moved.decode({0, 0}), std::invalid_argument);
            for (size_t k = 0; k < count; ++k)
                EXPECT_EQ(moved.decode(shots[k]), expected[k]);
            moved.drain();
        });
        for (size_t buffer = 0; buffer < 2; ++buffer)
            expect_borrowed_component_extraction(moved.serial_decoder(buffer), true);
    }
    expect_borrowed_component_extraction(owned, false);
}
#endif


#if defined(_OPENMP) && PM_PREDECODE_ENABLED && PM_SHARED_COMPONENT_PARALLELISM == 2
namespace {
// All synchronization in these probes uses atomics. A probe inspects only its
// own component; the peer never reads live MWPM/predecoder/seed fields.
struct ChainProbe {
    GroupNeighborhood neighborhood = nullptr;
    BoundRangeEntry bound = nullptr;
    structured::SerialImportTestAccess::TimelineEntry process = nullptr;
};
std::array<std::array<ChainProbe, 2>, 2> chain_probes;
std::atomic<bool> chain_wait_for_peer{false};
std::array<std::atomic<bool>, 2> chain_solved;
std::atomic<size_t> chain_wait_component{1};
std::atomic<int> chain_failure{0};  // 1 = C0 prefix, 2 = C0 solve
std::array<std::atomic<size_t>, 2> chain_process_visits;
std::atomic<size_t> chain_residual_roots, chain_wait_visits;

template <size_t B, size_t C>
uint32_t chain_neighborhood(const structured::OneHopPredecoder::Grid& grid, size_t cell, uint16_t boundary) {
    if constexpr (C == 0) {
        if (chain_failure.load(std::memory_order_relaxed) == 1)
            throw std::runtime_error("chain C0 prefix failure");
    }
    if (chain_wait_for_peer.load(std::memory_order_relaxed) && chain_wait_component.load() == C) {
        chain_wait_visits.fetch_add(1, std::memory_order_relaxed);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!chain_solved[1 - C].load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Component preparation waited for peer MWPM past the deadline");
            std::this_thread::yield();
        }
    }
    return chain_probes[B][C].neighborhood(grid, cell, boundary);
}
template <size_t B, size_t C>
void chain_bound(structured::OneHopBoundRange& range) {
    for (auto i = range.begin; i < range.end; ++i) {
        const auto& seed = range.seeds[i];
        if (seed.mate == structured::PredecodeSeed::UNMATCHED) {
            chain_neighborhood<B, C>(range.grid, seed.cell, range.grid.boundary_mask[seed.cell]);
            break;
        }
    }
    chain_probes[B][C].bound(range);
}
template <size_t B, size_t C>
void chain_process(structured::Mwpm& mwpm, bool predecode, bool imported, bool packed) {
    chain_process_visits[C].fetch_add(1, std::memory_order_relaxed);
    if constexpr (C == 0) {
        chain_residual_roots.fetch_add(mwpm.active_tree_count(), std::memory_order_relaxed);
        if (chain_failure.load(std::memory_order_relaxed) == 2)
            throw std::runtime_error("chain C0 solve failure");
    }
    chain_probes[B][C].process(mwpm, predecode, imported, packed);
    chain_solved[C].store(true, std::memory_order_release);
}
template <size_t B, size_t C>
void install_chain_probe(ComponentDecoder& decoder) {
    auto& mwpm = decoder.components()[C].mwpm;
    auto& grid = *decoder.components()[C].seed_grid;
    auto& probe = chain_probes[B][C];
    probe.neighborhood = grid.predecode.neighborhood;
    probe.bound = grid.predecode.run_bound_range;
    probe.process = structured::SerialImportTestAccess::exchange_process_entry(mwpm, chain_process<B, C>);
    grid.predecode.neighborhood = chain_neighborhood<B, C>;
    if (probe.bound) grid.predecode.run_bound_range = chain_bound<B, C>;
}
void reset_chain_observation(bool wait, int failure = 0) {
    chain_wait_for_peer.store(wait, std::memory_order_relaxed);
    chain_failure.store(failure, std::memory_order_relaxed);
    for (auto& solved : chain_solved) solved.store(false, std::memory_order_relaxed);
    chain_residual_roots.store(0, std::memory_order_relaxed);
    chain_wait_visits.store(0, std::memory_order_relaxed);
    for (auto& count : chain_process_visits) count.store(0, std::memory_order_relaxed);
}
std::pair<BackendUserGraph, StaticGridSpec> chain_fixture() {
    BackendUserGraph graph(6, 2);
    graph.add_or_merge_edge(0, 2, {0}, 2, -1);
    graph.add_or_merge_edge(2, 4, {}, 2, -1);
    graph.add_or_merge_edge(1, 3, {1}, 3, -1);
    graph.add_or_merge_edge(3, 5, {}, 3, -1);
    graph.add_or_merge_boundary_edge(0, {1}, 7, -1);
    graph.add_or_merge_boundary_edge(1, {0}, 11, -1);
    auto spec = pair_spec();
    spec.coordinates.push_back({4, 0, 0});
    spec.coordinates.push_back({4, 0, 0});
    return {std::move(graph), std::move(spec)};
}
}

TEST(DoubleBufferComponents, ComponentChainsSolveBeforePeerPreparationCompletes) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("captained_serial");
    ImportResetOwnerEnvironment ownership("captain");
    auto [graph, spec] = chain_fixture();
    const std::vector<std::vector<uint64_t>> shots{{0, 1, 2, 3, 4, 5}, {0, 1, 2, 4}};
    for (size_t parts : {1, 2}) {
        const size_t threads = parts == 1 ? 2 : 4;
        auto make = [&] {
            return parts == 1
                ? SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                    std::vector<int>(threads, -1), 1, 64, 2, 2, false)
                : make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, 2, 2);
        };
        auto reference = make();
        std::vector<MatchingResult> expected;
        reference.run_with_team([&] {
            for (const auto& shot : shots) {
                reference.prepare_for_mwpm(shot);
                expected.push_back(reference.solve_prepared());
                reference.reset_after_shot();
            }
        });
        for (auto schedule : {DoubleBufferComponentDecoder::Schedule::overlap,
                              DoubleBufferComponentDecoder::Schedule::serial_control}) {
            auto even = make(), odd = make();
            install_chain_probe<0, 0>(even.serial_decoder()); install_chain_probe<0, 1>(even.serial_decoder());
            install_chain_probe<1, 0>(odd.serial_decoder()); install_chain_probe<1, 1>(odd.serial_decoder());
            DoubleBufferComponentDecoder pipeline(std::move(even), std::move(odd), schedule);
            pipeline.run_with_team([&] {
                // Each fixed and mixed-fallback route visits both buffers.
                for (size_t waiting : {0, 1}) for (size_t repeat = 0; repeat < 3; ++repeat) for (size_t i : {0, 0, 1, 1}) {
                    SCOPED_TRACE(::testing::Message() << "parts=" << parts << " schedule=" << int(schedule)
                                 << " repeat=" << repeat << " input=" << i);
                    chain_wait_component.store(waiting);
                    reset_chain_observation(true);
                    EXPECT_EQ(pipeline.decode(shots[i]), expected[i]);
                    EXPECT_GT(chain_wait_visits.load(), 0U);
                    EXPECT_GT(chain_residual_roots.load(), 0U);
                    EXPECT_EQ(chain_process_visits[0].load(), 1U);
                    EXPECT_EQ(chain_process_visits[1].load(), 1U);
                    EXPECT_TRUE(chain_solved[0].load());
                    EXPECT_TRUE(chain_solved[1].load());
                }
                pipeline.drain();
            });
            for (size_t b = 0; b < 2; ++b) expect_import_reset_complete(pipeline.serial_decoder(b));
        }
    }
    reset_chain_observation(false);
}

TEST(DoubleBufferComponents, ComponentChainFailuresJoinPeersAndPermitBothBufferReuse) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("captained_serial");
    ImportResetOwnerEnvironment ownership("captain");
    auto [graph, spec] = chain_fixture();
    const std::vector<std::vector<uint64_t>> shots{{0, 1, 2, 3, 4, 5}, {0, 1, 2, 4}, {0}, {}};
    ComponentDecoder reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    std::vector<MatchingResult> expected;
    for (const auto& shot : shots) expected.push_back(reference.decode(shot));
    for (size_t parts : {1, 2}) for (size_t threads : {1, 2, 4}) {
        auto make = [&] {
            return parts == 1
                ? SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                    std::vector<int>(threads, -1), 1, 64, 2, 2, false)
                : make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, 2, 2);
        };
        auto even = make(), odd = make();
        install_chain_probe<0, 0>(even.serial_decoder()); install_chain_probe<0, 1>(even.serial_decoder());
        install_chain_probe<1, 0>(odd.serial_decoder()); install_chain_probe<1, 1>(odd.serial_decoder());
        DoubleBufferComponentDecoder pipeline(std::move(even), std::move(odd), threads == 1
            ? DoubleBufferComponentDecoder::Schedule::single_thread : DoubleBufferComponentDecoder::Schedule::overlap);
        for (int failure : {1, 2}) for (size_t parity : {0, 1}) {
            pipeline.run_with_team([&] {
                reset_chain_observation(false);
                if (pipeline.next_buffer() != parity) EXPECT_EQ(pipeline.decode(shots[0]), expected[0]);
                reset_chain_observation(false, failure);
                EXPECT_THROW(pipeline.decode(shots[0]), std::runtime_error);
                EXPECT_EQ(chain_process_visits[0].load(), failure == 1 ? 0U : 1U);
                EXPECT_EQ(chain_process_visits[1].load(), 1U);
                reset_chain_observation(false);
                EXPECT_THROW(pipeline.decode({0, 1, 2, 3, 4, 5, UINT64_MAX}), std::invalid_argument);
                EXPECT_THROW(pipeline.decode({0, 1, 2, 3, 4, 5, 0}), std::invalid_argument);
                // Odd sequences plus repeated drains retain parity and exercise
                // fixed -> mixed -> whole-shot serial -> fixed transitions.
                for (size_t i : {0, 1, 2, 3, 0, 1, 0}) EXPECT_EQ(pipeline.decode(shots[i]), expected[i]);
                pipeline.drain(); pipeline.drain();
            });
            for (size_t b = 0; b < 2; ++b) expect_import_reset_complete(pipeline.serial_decoder(b));
        }
    }
    reset_chain_observation(false);
}
#endif

#if defined(_OPENMP) && PM_PREDECODE_ENABLED && PM_TAILRESET_SYNDROME_STATS
TEST(DoubleBufferComponents, SyndromeSnapshotsMatchIndependentPrefixesAndSurviveReclaim) {
    FixedSeedConstructionEnvironment construction("fused");
    FixedSeedInputEnvironment input("indexed");
    ImportPreparationEnvironment preparation("captained_serial");
    ImportResetOwnerEnvironment ownership("captain");
    auto [dem, spec] = model(true, 9, 'x');
    auto graph = detector_error_model_to_backend_user_graph(dem, false, NUM_DISTINCT_WEIGHTS);
    using Counts = std::array<structured::TailResetSyndromeCounts, 2>;
    ComponentDecoder graph_reference(graph, spec, NUM_DISTINCT_WEIGHTS, false);
    ASSERT_EQ(graph_reference.components().size(), 2U);
    std::array<std::vector<uint64_t>, 2> dense;
    for (size_t c = 0; c < 2; ++c) {
        const auto& ids = graph_reference.components()[c].global_detectors;
        for (size_t k = 0; k < ids.size(); k += 5) dense[c].push_back(ids[k]);
        ASSERT_GT(dense[c].size(), 2U);
#if PM_TAILRESET_PREDECODE_MODE != 0
        EXPECT_EQ(graph_reference.components()[c].seed_grid->predecode.run_bound_range, nullptr);
#endif
    }
    std::vector<std::vector<uint64_t>> shots(6);
    shots[1] = dense[0]; shots[1].insert(shots[1].end(), dense[1].begin(), dense[1].end());
    shots[2] = dense[0]; shots[2].push_back(dense[1].front());
    shots[3] = dense[1]; shots[3].push_back(dense[0].front());
    shots[4] = {dense[0].front()};
    // Both empty inputs and fixed/mixed/whole-shot fallbacks visit each buffer.
    const std::array<size_t, 16> sequence{0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 1, 1, 0, 0};
    const auto expect_counts = [](const Counts& actual, const Counts& expected) {
        for (size_t c = 0; c < 2; ++c) {
            SCOPED_TRACE(::testing::Message() << "component=" << c);
            EXPECT_EQ(actual[c].normalized, expected[c].normalized);
            EXPECT_EQ(actual[c].residual, expected[c].residual);
            EXPECT_EQ(actual[c].pairs, expected[c].pairs);
            EXPECT_EQ(actual[c].boundaries, expected[c].boundaries);
            EXPECT_EQ(actual[c].normalized, actual[c].residual + 2 * actual[c].pairs + actual[c].boundaries);
        }
    };
    for (size_t parts : {1, 2}) for (auto schedule : {
            DoubleBufferComponentDecoder::Schedule::overlap,
            DoubleBufferComponentDecoder::Schedule::serial_control,
            DoubleBufferComponentDecoder::Schedule::single_thread}) {
        const size_t threads = schedule == DoubleBufferComponentDecoder::Schedule::single_thread ? 1 : 2 * parts;
        auto make = [&] {
            return parts == 1
                ? SharedComponentDecoder(ComponentDecoder(graph, spec, NUM_DISTINCT_WEIGHTS, false),
                    std::vector<int>(threads, -1), 1, 2, 2, 2, false)
                : make_group(graph, spec, threads, GroupGeometry::three_planes, true, 2, 2, 2);
        };
        auto reference = make();
        std::vector<Counts> expected_counts(shots.size());
        std::vector<MatchingResult> expected_results(shots.size());
        reference.run_with_team([&] {
            for (size_t k = 0; k < shots.size(); ++k) {
                reference.prepare_for_mwpm(shots[k]);
                for (size_t c = 0; c < 2; ++c) {
                    const auto& pre = reference.serial_decoder().components()[c].mwpm.predecoder;
                    const auto& view = pre.seed_view;
                    auto& count = expected_counts[k][c];
                    count.normalized = view.active_count();
                    size_t paired_endpoints = 0;
                    for (const auto& range : view.active_ranges())
                        for (const auto& seed : view.storage.subspan(range.begin, range.count())) {
                            if (seed.mate == structured::PredecodeSeed::UNMATCHED) ++count.residual;
                            else if (seed.mate == structured::PredecodeSeed::BOUNDARY) ++count.boundaries;
                            else ++paired_endpoints;
                        }
                    EXPECT_EQ(paired_endpoints % 2, 0U);
                    count.pairs = paired_endpoints / 2;
#if PM_TAILRESET_PREDECODE_MODE == 1
                    EXPECT_EQ(count.residual, count.normalized);
                    EXPECT_EQ(count.pairs, 0U);
                    EXPECT_EQ(count.boundaries, 0U);
#endif
                    if (parts == 1) {
                        const auto layout = pre.scratch_layout();
                        ASSERT_EQ(layout.size(), 1U);
                        EXPECT_EQ(layout[0].seed_begin, 0U);
                        EXPECT_EQ(layout[0].seed_end, view.active_count());
                    } else if (k == 1 || k == 2 || k == 3) {
                        const bool partitioned = k == 1 || c == (k == 2 ? 0 : 1);
                        EXPECT_EQ(reference.component_route(c), partitioned
                            ? structured::SharedRoute::partitioned : structured::SharedRoute::serial);
                        if (k == 1) {
                            EXPECT_EQ(view.active_ranges().size(), 3U);
                            EXPECT_LT(view.active_count(), view.storage.size());
                        } else {
                            // A low component selects the original dense
                            // preparation before any fixed bucket fill.
                            EXPECT_EQ(view.active_ranges().size(), 1U);
                            EXPECT_EQ(view.active_count(), view.storage.size());
                        }
                    }
                }
                uint64_t normalized = 0;
                for (const auto& count : expected_counts[k]) normalized += count.normalized;
                EXPECT_EQ(normalized, shots[k].size());
                expected_results[k] = reference.solve_prepared();
                reference.reset_after_shot();
            }
        });
        DoubleBufferComponentDecoder pipeline(make(), make(), schedule);
        EXPECT_THROW(pipeline.syndrome_statistics(), std::logic_error);
        pipeline.run_with_team([&] {
            EXPECT_THROW(pipeline.syndrome_statistics(), std::logic_error);
            for (size_t step = 0; step < sequence.size(); ++step) {
                const auto k = sequence[step];
                SCOPED_TRACE(::testing::Message() << "parts=" << parts << " schedule=" << int(schedule)
                             << " step=" << step << " shot=" << k);
                EXPECT_EQ(pipeline.next_buffer(), step & 1);
                EXPECT_EQ(pipeline.decode(shots[k]), expected_results[k]);
                const auto snapshot = pipeline.syndrome_statistics();
                expect_counts(snapshot, expected_counts[k]);
                // Leave most shots undrained to overlap the previous buffer's
                // reset. The snapshot must also survive joined retirement.
                if (step % 3 == 0) {
                    pipeline.drain();
                    expect_counts(pipeline.syndrome_statistics(), expected_counts[k]);
                }
                expect_counts(snapshot, expected_counts[k]);
            }
        });
        // Metadata access requires a stopped team. Reclamation has retired
        // both live views while the caller's snapshots survived each drain.
        for (size_t b = 0; b < 2; ++b)
            for (const auto& component : pipeline.serial_decoder(b).components())
                EXPECT_TRUE(component.mwpm.predecoder.seed_view.empty());
        EXPECT_THROW(pipeline.syndrome_statistics(), std::logic_error);
    }
}
#endif
