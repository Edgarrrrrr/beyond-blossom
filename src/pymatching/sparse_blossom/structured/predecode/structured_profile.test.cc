// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include "gtest/gtest.h"

#include "pymatching/sparse_blossom/driver/backend_user_graph.h"
#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/predecode/one_hop_predecoder.h"

namespace {
pm::structured::Mwpm profile_square(bool negative = false) {
    pm::structured::MatchingGraph graph(4, 1);
    graph.add_edge(0, 1, negative ? -2 : 2, {0});
    graph.add_edge(0, 2, 2, {});
    graph.add_edge(1, 3, 2, {});
    graph.add_edge(2, 3, 10, {});
    pm::StaticGridSpec spec{3, 3, false, {}, {}, pm::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{2, 0, 0}}, {{0, 2, 0}}, {{2, 2, 0}}};
    graph.configure_grid(spec);
    pm::structured::GraphFlooder flooder(std::move(graph));
    flooder.sync_negative_weight_observables_and_detection_events();
    return pm::structured::Mwpm(std::move(flooder));
}
#if PM_STRUCTURED_PROFILE_COUNTERS
void check_queue(const pm::structured::radix_heap_queue<false>& q) {
    const auto& s = q.profile;
    EXPECT_EQ(s.pushes, s.pops + s.cleared + q.size());
    EXPECT_EQ(s.pops, s.valid + s.stale_superseded + s.stale_cancelled + s.deferred_requeues);
    EXPECT_GE(s.peak_size, q.size());
}
#endif
}  // namespace

#if PM_STRUCTURED_PROFILE_TIMELINE
TEST(StructuredProfile, TimelinePreservesResetNestedWindowsAndMarksRepeatedEnvelopes) {
    using namespace pm::structured;
    PredecodeTimingStats stats;
    {
        PredecodePhaseScope<PredecodePhase::Prepare> prepare(stats);
        stats = {};
        PredecodePhaseScope<PredecodePhase::Reset> reset(stats);
    }
    const auto& prepare = stats.timeline[static_cast<size_t>(PredecodePhase::Prepare)];
    const auto& reset = stats.timeline[static_cast<size_t>(PredecodePhase::Reset)];
    ASSERT_TRUE(prepare.present);
    ASSERT_TRUE(reset.present);
    EXPECT_FALSE(prepare.repeated);
    EXPECT_LE(prepare.start_ns, reset.start_ns);
    EXPECT_LE(reset.end_ns, prepare.end_ns);
    EXPECT_EQ(prepare.end_ns - prepare.start_ns, stats.prepare_ns);
    EXPECT_EQ(reset.end_ns - reset.start_ns, stats.reset_ns);

    PhaseInterval earlier = reset;
    earlier.start_ns = prepare.start_ns;
    earlier.end_ns = prepare.end_ns;
    PhaseInterval envelope = reset;
    envelope.merge(earlier);
    EXPECT_TRUE(envelope.repeated);
    EXPECT_EQ(envelope.start_ns, prepare.start_ns);
    EXPECT_EQ(envelope.end_ns, prepare.end_ns);
}
#endif

TEST(StructuredProfile, CorrectnessResetAndPartialHandoff) {
    auto decoder = profile_square();
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto r = pm::structured::decode_detection_events_for_up_to_64_observables(decoder, {0, 1, 2, 3}, false);
        EXPECT_EQ(r.weight, 4);
        EXPECT_EQ(r.obs_mask, 0);
#if PM_STRUCTURED_PROFILE_COUNTERS
        const auto& s = decoder.predecoder.stats;
        EXPECT_EQ(s.input_events, 4);
        EXPECT_EQ(s.normalized_seeds, 4);
        EXPECT_EQ(s.normalized_seeds, 2 * s.matched_pairs + s.matched_boundaries + s.residual_seeds);
        EXPECT_EQ(s.mwpm_entered, 1);
#if PM_PREDECODE_ENABLED
        EXPECT_EQ(s.matched_pairs, 1);
        EXPECT_EQ(s.residual_seeds, 2);
        EXPECT_EQ(s.cap_requests, 5);
        EXPECT_EQ(s.cap_executions, PM_PREDECODE_CAP_CACHE ? 3 : 5);
        EXPECT_EQ(s.cap_repeat_finite_requests, 2);
        EXPECT_EQ(s.cap_finite_executions, PM_PREDECODE_CAP_CACHE ? 3 : 5);
        EXPECT_EQ(s.cap_radius_increases, 2);
        EXPECT_EQ(s.cap_unchanged, 3);
        EXPECT_EQ(s.import_regions, 4);
        EXPECT_EQ(s.import_roots, 2);
        EXPECT_EQ(s.import_pairs, 1);
        EXPECT_EQ(s.import_scheduled, 2);
#else
        EXPECT_EQ(s.matched_pairs, 0);
        EXPECT_EQ(s.residual_seeds, 4);
        EXPECT_EQ(s.cap_requests, 0);
        EXPECT_EQ(s.import_regions, 0);
#endif
        check_queue(decoder.flooder.queue);
        EXPECT_GT(decoder.flooder.queue.profile.pushes, 0);
#endif
#if PM_STRUCTURED_PROFILE_TIMERS
        const auto& t = decoder.predecoder.stats;
        EXPECT_GT(t.extract_ns, 0);
        EXPECT_GT(t.mwpm_ns, 0);
#if PM_PREDECODE_ENABLED
        EXPECT_GE(
            t.prepare_ns, t.reset_ns + t.seed_build_ns + t.normalize_ns + t.degree_ns + t.sort_ns + t.grow_match_ns);
        EXPECT_GE(t.handoff_ns, t.handoff_regions_ns + t.handoff_matches_ns + t.handoff_schedule_ns);
        EXPECT_EQ(t.initialization_ns, 0);
#else
        EXPECT_GT(t.initialization_ns, 0);
        EXPECT_EQ(t.prepare_ns, 0);
        EXPECT_EQ(t.handoff_ns, 0);
#endif
#if PM_STRUCTURED_PROFILE_TIMELINE
#if !PM_STRUCTURED_PROFILE_COUNTERS
        for (const auto count : {t.input_events, t.normalized_seeds, t.residual_seeds,
                                 t.matched_pairs, t.matched_boundaries, t.mwpm_entered,
                                 t.cap_requests, t.cap_executions, t.cap_finite_executions,
                                 t.cap_repeat_finite_requests, t.cap_radius_increases, t.cap_unchanged,
                                 t.import_regions, t.import_roots, t.import_pairs,
                                 t.import_boundaries, t.import_scheduled})
            EXPECT_EQ(count, 0);
#endif
        for (const auto phase : {
#if PM_PREDECODE_ENABLED
                                 pm::structured::PredecodePhase::Handoff,
#else
                                 pm::structured::PredecodePhase::Initialization,
#endif
                                 pm::structured::PredecodePhase::Mwpm,
                                 pm::structured::PredecodePhase::Extract}) {
            const auto& interval = t.timeline[static_cast<size_t>(phase)];
            EXPECT_TRUE(interval.present);
            EXPECT_FALSE(interval.repeated);
            EXPECT_LE(interval.start_ns, interval.end_ns);
        }
#if PM_PREDECODE_ENABLED
        // The raw single-component API prepares seeds and later runs the
        // predecoder in two separate Prepare scopes. Their envelope has a gap.
        const auto& prepare_interval = t.timeline[static_cast<size_t>(pm::structured::PredecodePhase::Prepare)];
        EXPECT_TRUE(prepare_interval.present);
        EXPECT_TRUE(prepare_interval.repeated);
#endif
#endif
#endif
        r = pm::structured::decode_detection_events_for_up_to_64_observables(decoder, {}, false);
        EXPECT_EQ(r.weight, 0);
#if PM_STRUCTURED_PROFILE_COUNTERS
        const auto& empty = decoder.predecoder.stats;
        EXPECT_EQ(empty.normalized_seeds, 0);
        EXPECT_EQ(empty.residual_seeds, 0);
        EXPECT_EQ(empty.cap_requests, 0);
        EXPECT_EQ(empty.import_regions, 0);
        EXPECT_EQ(empty.mwpm_entered, PM_PREDECODE_ENABLED ? 0 : 1);
        EXPECT_EQ(decoder.flooder.queue.profile.pushes, 0);
        EXPECT_EQ(decoder.flooder.queue.profile.pops, 0);
        EXPECT_EQ(decoder.flooder.queue.profile.empty_pops, PM_PREDECODE_ENABLED ? 0 : 1);
        check_queue(decoder.flooder.queue);
#endif
    }
}

TEST(StructuredProfile, NegativeNormalizationAndReuse) {
    auto decoder = profile_square(true);
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto result = pm::structured::decode_detection_events_for_up_to_64_observables(decoder, {0, 1}, false);
        EXPECT_EQ(result.weight, -2);
        EXPECT_EQ(result.obs_mask, 1);
#if PM_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(decoder.predecoder.stats.input_events, 2);
        EXPECT_EQ(decoder.predecoder.stats.normalized_seeds, 0);
        EXPECT_EQ(decoder.predecoder.stats.residual_seeds, 0);
        EXPECT_EQ(decoder.predecoder.stats.cap_requests, 0);
        check_queue(decoder.flooder.queue);
#endif
        result = pm::structured::decode_detection_events_for_up_to_64_observables(decoder, {}, false);
        EXPECT_EQ(result.weight, 0);
#if PM_STRUCTURED_PROFILE_COUNTERS
        const auto& s = decoder.predecoder.stats;
        EXPECT_EQ(s.input_events, 0);
        EXPECT_EQ(s.normalized_seeds, 2);
        EXPECT_EQ(s.normalized_seeds, 2 * s.matched_pairs + s.matched_boundaries + s.residual_seeds);
        check_queue(decoder.flooder.queue);
#endif
    }
}

TEST(StructuredProfile, ComponentPartitionTimingAndExceptionReuse) {
    pm::BackendUserGraph graph(2, 1);
    graph.add_or_merge_edge(0, 1, {0}, 1, 0.1);
    pm::StaticGridSpec spec{3, 3, false, {}, {}, pm::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{2, 0, 0}}};
    graph.configure_static_grid(spec);
    auto& decoder = graph.get_structured_mwpm();
    for (int repeat = 0; repeat < 3; ++repeat) {
        EXPECT_THROW(decoder.decode({UINT64_MAX}), std::invalid_argument);
        auto r = decoder.decode({0, 1});
        EXPECT_EQ(r.weight, 2);
        EXPECT_EQ(r.obs_mask, 1);
#if PM_STRUCTURED_PROFILE_TIMERS
        EXPECT_GT(decoder.profile.partition_ns, 0);
        EXPECT_GT(decoder.profile.component_decode_ns, 0);
        EXPECT_GT(decoder.profile.merge_ns, 0);
#endif
#if PM_STRUCTURED_PROFILE_TIMELINE
        const auto& p = decoder.profile;
        EXPECT_TRUE(p.decode_interval.present);
        EXPECT_EQ(p.decode_interval.end_ns - p.decode_interval.start_ns, p.decode_ns);
        EXPECT_TRUE(p.partition_interval.present);
        EXPECT_EQ(p.partition_interval.end_ns - p.partition_interval.start_ns, p.partition_ns);
        EXPECT_LE(p.decode_interval.start_ns, p.partition_interval.start_ns);
        EXPECT_LE(p.partition_interval.end_ns, p.component_intervals[0].start_ns);
        EXPECT_LE(p.component_intervals[0].end_ns, p.merge_intervals[0].start_ns);
        EXPECT_LE(p.merge_intervals[0].end_ns, p.decode_interval.end_ns);
#endif
        r = decoder.decode_partitioned(decoder.partition_detection_events({}));
        EXPECT_EQ(r.weight, 0);
#if PM_STRUCTURED_PROFILE_TIMERS
        EXPECT_EQ(decoder.profile.partition_ns, 0);
#endif
    }
}

#if PM_STRUCTURED_PROFILE_COUNTERS
TEST(StructuredProfile, QueueRedistributionCountsLogicalMoves) {
    pm::structured::radix_heap_queue<false> q;
    pm::structured::DetectorNode node;
    for (int t : {1, 4, 5, 7})
        q.enqueue({&node, pm::cyclic_time_int{t}});
    for (int t : {1, 4, 5, 7})
        EXPECT_EQ(q.dequeue().time, pm::cyclic_time_int{t});
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.profile.pushes, 4);
    EXPECT_EQ(q.profile.pops, 4);
    EXPECT_EQ(q.profile.peak_size, 4);
    EXPECT_EQ(q.profile.rebucket_passes, 4);
    EXPECT_EQ(q.profile.rebucket_swap_passes, 2);
    EXPECT_EQ(q.profile.rebucket_source_events, 6);
    EXPECT_EQ(q.profile.rebucket_moves, 4);
    EXPECT_EQ(q.profile.min_scan_comparisons, 2);
}
TEST(StructuredProfile, QueueStaleReasonsClearAndResetConserveEvents) {
    pm::structured::radix_heap_queue<false> q;
    pm::structured::DetectorNode node;
    auto& t = node.node_event_tracker;
    t.set_desired_event({&node, pm::cyclic_time_int{10}}, q);
    t.set_desired_event({&node, pm::cyclic_time_int{6}}, q);
    t.set_desired_event({&node, pm::cyclic_time_int{8}}, q);
    EXPECT_FALSE(t.dequeue_decision(q.dequeue(), q));  // Deferred 6 -> 8.
    EXPECT_TRUE(t.dequeue_decision(q.dequeue(), q));
    EXPECT_FALSE(t.dequeue_decision(q.dequeue(), q));  // Superseded 10.
    t.set_desired_event({&node, pm::cyclic_time_int{12}}, q);
    t.set_no_desired_event();
    EXPECT_FALSE(t.dequeue_decision(q.dequeue(), q));  // Cancelled 12.
    q.dequeue();
    EXPECT_EQ(q.profile.pushes, 4);
    EXPECT_EQ(q.profile.pops, 4);
    EXPECT_EQ(q.profile.empty_pops, 1);
    EXPECT_EQ(q.profile.valid, 1);
    EXPECT_EQ(q.profile.stale_superseded, 1);
    EXPECT_EQ(q.profile.stale_cancelled, 1);
    EXPECT_EQ(q.profile.deferred_requeues, 1);
    q.enqueue({&node, pm::cyclic_time_int{15}});
    q.enqueue({&node, pm::cyclic_time_int{20}});
    q.clear();
    EXPECT_EQ(q.profile.cleared, 2);
    check_queue(q);
    q.profile = {};
    EXPECT_EQ(q.profile.pushes, 0);
    EXPECT_EQ(q.profile.peak_size, 0);
}
#endif
