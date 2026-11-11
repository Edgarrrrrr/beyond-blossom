// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include "gtest/gtest.h"

#include "beyond_blossom/structured/flooder/graph_flooder.h"
#include "beyond_blossom/structured/flooder/region_pool.test.h"

using beyond_blossom::cyclic_time_int;
using beyond_blossom::VaryingCT;
using namespace beyond_blossom::structured;

static_assert(sizeof(RegionId) == 4);
static_assert(sizeof(void*) != 8 || sizeof(GraphFillRegion) == 128);
static_assert(sizeof(RegionGeneration) == 4);
static_assert(sizeof(GraphFillRegion::blossom_parent) == 4);
static_assert(sizeof(GraphFillRegion::blossom_parent_top) == 4);
static_assert(sizeof(Match::region) == 4);
static_assert(sizeof(RegionEdge::region) == 4);
static_assert(sizeof(DetectorNode::region_that_arrived) == 4);

TEST(StructuredRegionPool, DirectObjectAndRadiusIndices) {
    EXPECT_THROW(RegionPool(UINT32_MAX), std::length_error);
    RegionPool pool(4);
    EXPECT_EQ(pool.get(NO_REGION), nullptr);
    EXPECT_EQ(pool.radii.data()[NO_REGION], VaryingCT{0});
    auto ids = pool.reserve_ids(4);
    ASSERT_EQ(ids, (std::vector<RegionId>{1, 2, 3, 4}));
    EXPECT_FALSE(pool.empty());
    for (auto id : ids) {
        auto& region = pool.construct_reserved(id);
        EXPECT_EQ(&region, &pool[id]);
        EXPECT_EQ(region.region_id, id);
        EXPECT_EQ(&region.radius_value(), &pool.radii.data()[id]);
        EXPECT_EQ(region.blossom_parent_top, id);
        EXPECT_EQ(region.blossom_parent, NO_REGION);
        EXPECT_TRUE(pool.is_live(id, pool.generation(id)));
    }
    EXPECT_EQ(&pool[4] - &pool[1], 3);
    EXPECT_THROW(pool.allocate(), std::length_error);
    EXPECT_THROW(pool.reserve_ids(1), std::length_error);
    EXPECT_FALSE(pool.is_live(NO_REGION, 0));
    EXPECT_FALSE(pool.is_live(UINT32_MAX, 0));
    for (auto id : ids)
        pool.release(id);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.radii.data()[NO_REGION], VaryingCT{0});
}

TEST(StructuredRegionPool, ReuseReconstructsVectorsAndAdvancesGeneration) {
    RegionPool pool(1);
    auto* first = pool.allocate();
    const auto id = first->region_id;
    const auto generation = pool.generation(id);
    for (size_t k = 0; k < 10; ++k)
        first->shell_area.push_back(nullptr);
    first->blossom_children.resize(3);
    first->match.region = id;
    first->radius_value() = VaryingCT{44};
    pool.release(id);
    EXPECT_FALSE(pool.is_live(id, generation));
    EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
    auto* second = pool.allocate();
    EXPECT_EQ(first, second);
    EXPECT_EQ(second->region_id, id);
    EXPECT_GT(pool.generation(id), generation);
    EXPECT_FALSE(pool.is_live(id, generation));
    EXPECT_TRUE(second->shell_area.empty());
    EXPECT_TRUE(second->blossom_children.empty());
    EXPECT_EQ(second->match.region, NO_REGION);
    EXPECT_EQ(second->radius_value(), VaryingCT{1});
    // The pool's destructor owns destruction of this still-live object.
    for (size_t k = 0; k < 12; ++k)
        second->shell_area.push_back(nullptr);
}

TEST(StructuredRegionPool, ResetAndReservedRollbackPreserveLifetimeIdentity) {
    RegionPool pool(3);
    auto ids = pool.reserve_ids(3);
    pool.construct_reserved(ids[0]).blossom_children.resize(3);
    auto generation = pool.generation(ids[0]);
    pool.release(ids[1]);  // A reserved but unconstructed slot has no destructor.
    pool.reset();          // Includes the other unconstructed reservation.
    EXPECT_TRUE(pool.empty());
    for (auto id : ids) {
        EXPECT_FALSE(pool.is_live(id, generation));
        EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
    }
    auto reused = pool.reserve_ids(3);
    for (auto id : reused) {
        pool.construct_reserved(id);
        EXPECT_GT(pool.generation(id), generation);
    }
    EXPECT_THROW(pool.reserve_ids(1), std::length_error);
    pool.reset();
    EXPECT_TRUE(pool.empty());
}

TEST(StructuredRegionPool, SeededConstructionCanGrowAndReturnToGeneralAllocation) {
    RegionPool pool(2);
    DetectorNode nodes[3];
    const auto first = pool.reserve_contiguous_ids(2);
    const Match match{first, {&nodes[1], &nodes[0], 5}};
    const auto radius = VaryingCT::from_base_and_growth(12, 0);
    // The partner remains reserved when this endpoint is constructed.
    auto& region = pool.construct_reserved_seed(first + 1, radius, &nodes[1], match);
    EXPECT_EQ(region.match, match);
    EXPECT_EQ(region.radius_value(), radius);
    ASSERT_EQ(region.shell_area.size(), 1);
    EXPECT_EQ(region.shell_area[0], &nodes[1]);
    EXPECT_TRUE(region.blossom_children.empty());
    EXPECT_EQ(region.alt_tree_node, nullptr);
    EXPECT_FALSE(region.shrink_event_tracker.has_queued_time);
    EXPECT_FALSE(region.shrink_event_tracker.has_desired_time);
    EXPECT_TRUE(pool.is_live(first + 1, pool.generation(first + 1)));
    // Formal MWPM may expand the imported singleton into a heap-backed shell.
    region.shell_area.push_back(&nodes[2]);
    EXPECT_EQ(region.shell_area[0], &nodes[1]);
    EXPECT_EQ(region.shell_area[1], &nodes[2]);
    region.blossom_children.resize(3);
    const auto generation = pool.generation(first + 1);
    pool.reset();  // Reclaims the grown object and its still-reserved partner.
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.is_live(first + 1, generation));
    auto* reused = pool.allocate();
    EXPECT_EQ(reused->region_id, first + 1);
    EXPECT_GT(pool.generation(reused->region_id), generation);
    EXPECT_TRUE(reused->shell_area.empty());
    EXPECT_TRUE(reused->blossom_children.empty());
    EXPECT_EQ(reused->radius_value(), VaryingCT{1});
    EXPECT_EQ(reused->match, (Match{NO_REGION, {nullptr, nullptr, 0}}));
}

TEST(StructuredRegionPool, StaleShrinkCannotConsumeReusedRegionsEvent) {
    for (bool reset : {false, true}) {
        GraphFlooder flooder;
        flooder.regions = std::make_unique<RegionPool>(1);
        flooder.radius_store = &flooder.regions->radii;
        auto* first = flooder.allocate_region();
        auto id = first->region_id;
        FloodCheckEvent old_event{id, flooder.regions->generation(id), cyclic_time_int{10}};
        first->shrink_event_tracker.set_desired_event(old_event, flooder.queue);
        if (reset)
            flooder.regions->reset();
        else
            flooder.regions->release(id);
        auto* second = flooder.allocate_region();
        FloodCheckEvent current_event{id, flooder.regions->generation(id), cyclic_time_int{10}};
        ASSERT_NE(current_event, old_event);
        second->shrink_event_tracker.set_desired_event(current_event, flooder.queue);
        EXPECT_FALSE(flooder.dequeue_decision(old_event));
        EXPECT_TRUE(second->shrink_event_tracker.has_queued_time);
        EXPECT_EQ(flooder.dequeue_valid(), current_event);
        EXPECT_EQ(flooder.dequeue_valid().tentative_event_type, NO_FLOOD_CHECK_EVENT);
    }
}

TEST(StructuredRegionPool, ReleasedShrinkIsDiscardedWithoutObjectAccess) {
    GraphFlooder flooder;
    flooder.regions = std::make_unique<RegionPool>(1);
    auto* first = flooder.allocate_region();
    auto id = first->region_id;
    FloodCheckEvent old_event{id, flooder.regions->generation(id), cyclic_time_int{10}};
    first->shrink_event_tracker.set_desired_event(old_event, flooder.queue);
    flooder.regions->release(id);
    EXPECT_EQ(flooder.dequeue_valid().tentative_event_type, NO_FLOOD_CHECK_EVENT);
}

TEST(StructuredRegionPool, ActualStaleShrinkPopsConserveDiagnosticCounts) {
    for (bool reset : {false, true}) {
        for (bool reuse : {false, true}) {
            beyond_blossom::profile::ShotStats stats;
            beyond_blossom::profile::Session session{beyond_blossom::profile::TraceLevel::off, &stats};
            beyond_blossom::profile::ActiveSessionScope active(&session);
            GraphFlooder flooder;
            flooder.regions = std::make_unique<RegionPool>(1);
            auto* first = flooder.allocate_region();
            const auto id = first->region_id;
            FloodCheckEvent old_event{id, flooder.regions->generation(id), cyclic_time_int{10}};
            first->shrink_event_tracker.set_desired_event(old_event, flooder.queue);
            if (reset)
                flooder.regions->reset();
            else
                flooder.regions->release(id);
            if (reuse) {
                auto* second = flooder.allocate_region();
                ASSERT_EQ(second->region_id, id);
                FloodCheckEvent current_event{id, flooder.regions->generation(id), cyclic_time_int{11}};
                second->shrink_event_tracker.set_desired_event(current_event, flooder.queue);
                // The old event must be popped first and cannot consume the new tracker.
                EXPECT_EQ(flooder.dequeue_valid(), current_event);
            }
            EXPECT_EQ(flooder.dequeue_valid().tentative_event_type, NO_FLOOD_CHECK_EVENT);
#if BB_STRUCTURED_PROFILE_COUNTERS
            const auto& q = flooder.queue.profile;
            EXPECT_EQ(q.pops, 1U + reuse);
            EXPECT_EQ(q.valid, static_cast<uint64_t>(reuse));
            EXPECT_EQ(q.stale_cancelled, 1U);
            EXPECT_EQ(q.pops, q.valid + q.stale_superseded + q.stale_cancelled + q.deferred_requeues);
            EXPECT_EQ(q.pushes, q.pops + q.cleared + flooder.queue.size());
#endif
#if BB_ENABLE_PROFILE
            EXPECT_EQ(stats.queue_dequeue_region, 1U + reuse);
            EXPECT_EQ(stats.queue_valid_region, static_cast<uint64_t>(reuse));
            EXPECT_EQ(stats.queue_stale_region, 1U);
            EXPECT_EQ(stats.queue_dequeue, stats.queue_valid + stats.queue_stale);
#endif
        }
    }
}

TEST(StructuredRegionPool, ContiguousReservationAfterSmallerShotPreservesGeneration) {
    RegionPool pool(5);
    EXPECT_EQ(pool.reserve_contiguous_ids(5), 1U);
    for (RegionId id = 1; id <= 5; ++id)
        pool.construct_reserved(id);
    auto old_generation = pool.generation(5);
    EXPECT_THROW(pool.reserve_contiguous_ids(1), std::logic_error);
    pool.reset();
    EXPECT_EQ(pool.reserve_contiguous_ids(2), 1U);
    for (RegionId id = 1; id <= 2; ++id)
        pool.construct_reserved(id);
    for (RegionId id = 3; id <= 5; ++id)
        EXPECT_EQ(pool.allocate()->region_id, id);
    EXPECT_GT(pool.generation(5), old_generation);
    EXPECT_FALSE(pool.is_live(5, old_generation));
    pool.reset();
    EXPECT_THROW(pool.reserve_contiguous_ids(6), std::length_error);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.reserve_contiguous_ids(0), 1U);
    EXPECT_TRUE(pool.empty());
}

TEST(StructuredRegionPool, ShellStoresSingletonInlineAndPreservesOrderThroughGrowth) {
    DetectorNode nodes[65];
    RegionShell shell;
    EXPECT_TRUE(shell.empty());
    EXPECT_EQ(shell.begin(), shell.end());
    shell.push_back(&nodes[0]);
    const auto inline_address = reinterpret_cast<uintptr_t>(shell.begin());
    const auto object_address = reinterpret_cast<uintptr_t>(&shell);
    EXPECT_GE(inline_address, object_address);
    EXPECT_LT(inline_address, object_address + sizeof(shell));
    EXPECT_EQ(shell.back(), &nodes[0]);
    for (size_t k = 1; k < 65; ++k) {
        shell.push_back(&nodes[k]);
        ASSERT_EQ(shell.size(), k + 1);
        EXPECT_EQ(shell.back(), &nodes[k]);
        for (size_t i = 0; i <= k; ++i)
            EXPECT_EQ(shell[i], &nodes[i]);
    }
    EXPECT_NE(reinterpret_cast<uintptr_t>(shell.begin()), inline_address);
    const RegionShell& read_only = shell;
    size_t index = 0;
    for (auto* node : read_only)
        EXPECT_EQ(node, &nodes[index++]);
    EXPECT_EQ(index, 65);
    auto* allocation = shell.begin();
    for (size_t k = 65; k > 0; --k) {
        EXPECT_EQ(shell.back(), &nodes[k - 1]);
        EXPECT_EQ(read_only[k - 1], &nodes[k - 1]);
        shell.pop_back();
    }
    EXPECT_TRUE(shell.empty());
    shell.push_back(&nodes[64]);
    EXPECT_EQ(shell.begin(), allocation);
    EXPECT_EQ(shell.back(), &nodes[64]);
}

TEST(StructuredRegionPool, ShellEqualityIgnoresInlineOrHeapStorage) {
    DetectorNode nodes[2];
    RegionShell small, large;
    EXPECT_EQ(small, large);
    small.push_back(&nodes[0]);
    large.push_back(&nodes[0]);
    large.push_back(&nodes[1]);
    EXPECT_NE(small, large);
    large.pop_back();
    EXPECT_EQ(small, large);
    small[0] = &nodes[1];
    EXPECT_NE(small, large);
    small.pop_back();
    large.pop_back();
    EXPECT_EQ(small, large);
}

TEST(StructuredRegionPool, NeutralSeedsSurviveSmallerShotsAndOrdinaryAllocation) {
    RegionPool pool(6);
    DetectorNode nodes[4];
    pool.reserve_contiguous_ids(4);
    for (RegionId id = 1; id <= 4; ++id) {
        auto& region = pool.construct_reserved_seed(id, VaryingCT{12}, &nodes[id - 1], Match{});
        region.shell_area.push_back(nullptr);
        region.blossom_children.resize(3);
        pool.release(id);
    }
    const auto generation4 = pool.generation(4);
    pool.mark_completed_shot();
    EXPECT_TRUE(pool.has_pending_neutral_reset());
    const auto end = pool.take_neutral_reset_end();
    ASSERT_EQ(end, 5U);
    pool.prepare_neutral_range(1, 3);
    pool.prepare_neutral_range(3, end);
    EXPECT_EQ(pool.take_neutral_reset_end(), 0U);
    // Repeating an already-neutral range leaves all objects and identities idle.
    pool.prepare_neutral_range(1, end);
    for (RegionId id = 1; id <= 4; ++id) {
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 3U);
        EXPECT_FALSE(pool.is_live(id, pool.generation(id)));
        EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
    }
    pool.reserve_contiguous_ids(2);
    EXPECT_EQ(RegionPoolTestAccess::next(pool), 3U);
    EXPECT_EQ(RegionPoolTestAccess::lifetime_end(pool), 5U);
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 4), 3U);
    const Match match{2, {&nodes[0], &nodes[1], 7}};
    auto& seed = pool.construct_reserved_seed(1, VaryingCT{24}, &nodes[0], match);
    EXPECT_EQ(seed.match, match);
    EXPECT_EQ(seed.shell_area.size(), 1U);
    EXPECT_EQ(seed.shell_area[0], &nodes[0]);
    EXPECT_EQ(seed.blossom_parent, NO_REGION);
    EXPECT_EQ(seed.blossom_parent_top, 1U);
    EXPECT_EQ(seed.alt_tree_node, nullptr);
    EXPECT_TRUE(seed.blossom_children.empty());
    EXPECT_FALSE(seed.shrink_event_tracker.has_desired_time);
    EXPECT_FALSE(seed.shrink_event_tracker.has_queued_time);
    auto& ordinary = pool.construct_reserved(2);
    EXPECT_TRUE(ordinary.shell_area.empty());
    EXPECT_EQ(ordinary.match, (Match{NO_REGION, {nullptr, nullptr, 0}}));
    EXPECT_EQ(ordinary.radius_value(), VaryingCT{1});
    EXPECT_EQ(pool.allocate()->region_id, 3U);
    EXPECT_EQ(pool.allocate()->region_id, 4U);
    EXPECT_GT(pool.generation(4), generation4);
    EXPECT_FALSE(pool.is_live(4, generation4));
    for (RegionId id = 1; id <= 4; ++id)
        pool.release(id);
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    // Shrinking to zero leaves four real, dormant object lifetimes to destroy.
    pool.reserve_contiguous_ids(0);
    EXPECT_EQ(RegionPoolTestAccess::next(pool), 1U);
    pool.reset();
    for (RegionId id = 1; id <= 4; ++id)
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 0U);
    EXPECT_EQ(RegionPoolTestAccess::lifetime_end(pool), 1U);
    EXPECT_EQ(pool.generation(4), generation4 + 1);
}

TEST(StructuredRegionPool, NeutralMixedReservationRollbackAndHardReset) {
    RegionPool pool(6);
    pool.reserve_contiguous_ids(3);
    for (RegionId id = 1; id <= 3; ++id) {
        pool.construct_reserved(id);
        pool.release(id);
    }
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    pool.reserve_contiguous_ids(5);
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 2), 4U);
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 5), 1U);
    pool.construct_reserved(1).shell_area.push_back(nullptr);
    pool.construct_reserved(4).blossom_children.resize(3);
    pool.release(2);  // Reserved-neutral rollback owns a real destructor.
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 2), 0U);
    const auto generation = pool.generation(3);
    pool.reset();  // live2, reserved4, reserved1 and free0 coexist.
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.has_pending_neutral_reset());
    for (RegionId id = 1; id <= 6; ++id) {
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 0U);
        EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
    }
    EXPECT_EQ(pool.generation(3), generation);
    // An ordinary-only allocation after this abort must not neutralize holes.
    const auto id = pool.allocate()->region_id;
    pool.release(id);
    pool.mark_completed_shot();
    EXPECT_EQ(pool.take_neutral_reset_end(), 0U);
    for (RegionId k = 1; k <= 6; ++k)
        EXPECT_EQ(RegionPoolTestAccess::state(pool, k), 0U);
}

TEST(StructuredRegionPool, NeutralGenerationExhaustionAndSuccessTokenAreTransactional) {
    RegionPool pool(3);
    pool.reserve_contiguous_ids(3);
    for (RegionId id = 1; id <= 3; ++id) {
        pool.construct_reserved(id);
        pool.release(id);
    }
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    pool.mark_completed_shot();  // The successful-shot eligibility is consumed.
    EXPECT_FALSE(pool.has_pending_neutral_reset());
    RegionPoolTestAccess::set_generation(pool, 2, UINT32_MAX);
    const auto generation1 = pool.generation(1), generation3 = pool.generation(3);
    EXPECT_THROW(pool.reserve_contiguous_ids(2), std::overflow_error);
    EXPECT_EQ(RegionPoolTestAccess::next(pool), 4U);
    EXPECT_EQ(pool.generation(1), generation1);
    for (RegionId id = 1; id <= 3; ++id)
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 3U);
    // ID3 reserves successfully; ID2 overflows; rollback destroys reserved4.
    EXPECT_THROW(pool.reserve_ids(2), std::overflow_error);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 3), 0U);
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 2), 3U);
    EXPECT_EQ(pool.generation(3), generation3 + 1);
    auto* region = pool.allocate();
    EXPECT_EQ(region->region_id, 3U);
    EXPECT_EQ(pool.generation(3), generation3 + 2);
    pool.release(3);
    pool.reset();
    EXPECT_EQ(pool.generation(2), UINT32_MAX);
    EXPECT_THROW(pool.reserve_contiguous_ids(3), std::overflow_error);
}

TEST(StructuredRegionPool, StaleShrinkEventCannotEnterNeutralOrReusedLifetime) {
    GraphFlooder flooder;
    flooder.regions = std::make_unique<RegionPool>(1);
    auto& pool = *flooder.regions;
    flooder.radius_store = &pool.radii;
    pool.reserve_contiguous_ids(1);
    auto& first = pool.construct_reserved(1);
    const FloodCheckEvent old_event{1, pool.generation(1), cyclic_time_int{10}};
    first.shrink_event_tracker.set_desired_event(old_event, flooder.queue);
    pool.release(1);
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    EXPECT_EQ(flooder.dequeue_valid().tentative_event_type, NO_FLOOD_CHECK_EVENT);
    pool.reserve_contiguous_ids(1);
    EXPECT_FALSE(pool.is_live(1, pool.generation(1)));
    auto& second = pool.construct_reserved(1);
    const FloodCheckEvent current_event{1, pool.generation(1), cyclic_time_int{11}};
    flooder.queue.enqueue(old_event);
    second.shrink_event_tracker.set_desired_event(current_event, flooder.queue);
    EXPECT_EQ(flooder.dequeue_valid(), current_event);
}

TEST(StructuredRegionPool, UntouchedSeedRetirementReuseRollbackAndInvalidation) {
    RegionPool pool(4);
    DetectorNode nodes[4];
    auto initial = [&] {
        auto first = pool.reserve_contiguous_ids(2);
        pool.construct_reserved_seed(first, VaryingCT::from_base_and_growth(2,0), &nodes[0],
            {first+1, {&nodes[0], &nodes[1], 5}});
        pool.construct_reserved_seed(first+1, VaryingCT::from_base_and_growth(2,0), &nodes[1],
            {first, {&nodes[1], &nodes[0], 5}});
        return first;
    };
    auto first = initial();
    const auto generation = pool.generation(first);
    ASSERT_TRUE(pool.is_unchanged_seed(first));
    EXPECT_EQ(pool.seed_extraction(first).obs_mask, 5U);
    EXPECT_EQ(pool.seed_extraction(first).radius, 2);
    pool.release_unchanged_seed(first);
    pool.release_unchanged_seed(first+1);
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.is_live(first, generation));
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    EXPECT_TRUE(pool.is_cached_seed_slot(first));
    // Reuse an intact singleton as an ordinary empty blossom allocation.
    auto* general = pool.allocate();
    EXPECT_TRUE(general->shell_area.empty());
    EXPECT_TRUE(general->blossom_children.empty());
    EXPECT_EQ(general->match.edge.loc_from, nullptr);
    pool.release(general->region_id);
    first = initial();
    EXPECT_GT(pool.generation(first), generation);
    pool.get(first)->add_match(pool.get(first+1), {&nodes[0], &nodes[1], 5});
    // Even restoring the same match does not restore the untouched certificate.
    EXPECT_FALSE(pool.is_unchanged_seed(first));
    EXPECT_FALSE(pool.is_unchanged_seed(first+1));
    pool.release(first); pool.release(first+1);
    first = initial();
    pool.release_unchanged_seed(first); pool.release_unchanged_seed(first+1);
    auto ids = pool.reserve_ids(2);
    for (auto id : ids) pool.release(id); // reserved cached singleton rollback
    EXPECT_TRUE(pool.empty());
    first = initial();
    pool.release_unchanged_seed(first); // live plus cached hard reset
    pool.reset();
    EXPECT_TRUE(pool.empty());
    for (RegionId id = 1; id <= pool.capacity(); ++id)
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 0U);
}

TEST(StructuredRegionPool, LazySeedsStayRawAcrossExtractionResetAndOrdinaryReuse) {
    RegionPool pool(3);
    DetectorNode nodes[3];
    RegionGeneration prior_generation = 0;
    for (size_t shot = 0; shot < 3; ++shot) {
        ASSERT_EQ(pool.reserve_contiguous_ids(3), 1U);
        // Publishing a mate first is valid. Parallel workers defer object
        // reads until publication finishes, but serial inspection also works.
        pool.publish_reserved_matched_seed(2, VaryingCT::from_base_and_growth(4, 0), &nodes[1],
            {1, {&nodes[1], &nodes[0], 5}});
        pool.publish_reserved_matched_seed(1, VaryingCT::from_base_and_growth(2, 0), &nodes[0],
            {2, {&nodes[0], &nodes[1], 5}});
        pool.publish_reserved_matched_seed(3, VaryingCT::from_base_and_growth(8, 0), &nodes[2],
            {NO_REGION, {&nodes[2], nullptr, 7}});
        EXPECT_GT(pool.generation(1), prior_generation);
        EXPECT_FALSE(pool.is_live(1, prior_generation));
        prior_generation = pool.generation(1);
        for (RegionId id = 1; id <= 3; ++id) {
            EXPECT_TRUE(pool.is_lazy_seed(id));
            EXPECT_TRUE(pool.is_unchanged_seed(id));
            EXPECT_TRUE(pool.is_live(id, pool.generation(id)));
            EXPECT_EQ(pool.seed_node(id), &nodes[id - 1]);
        }
        EXPECT_EQ(pool.seed_extraction(1).mate, 2U);
        EXPECT_EQ(pool.seed_extraction(2).radius, 4);
        EXPECT_EQ(pool.seed_extraction(3).obs_mask, 7U);
        EXPECT_EQ(pool.radii.data()[2], VaryingCT::from_base_and_growth(4, 0));
        for (RegionId id = 1; id <= 3; ++id) {
            pool.release_unchanged_seed(id);
            EXPECT_TRUE(pool.is_deferred_seed_slot(id));
            EXPECT_FALSE(pool.is_lazy_seed(id));
            EXPECT_FALSE(pool.is_live(id, pool.generation(id)));
            EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
            EXPECT_EQ(pool.seed_node(id), &nodes[id - 1]);
        }
        EXPECT_TRUE(pool.empty());
        pool.mark_completed_shot();
        const auto end = pool.take_neutral_reset_end();
        ASSERT_EQ(end, 4U);
        pool.prepare_neutral_range(1, 2);
        pool.prepare_neutral_range(2, end);
        // Reset does not eagerly construct the objects avoided by import.
        for (RegionId id = 1; id <= 3; ++id)
            EXPECT_TRUE(pool.is_deferred_seed_slot(id));
    }
    auto* ordinary = pool.allocate();
    EXPECT_EQ(ordinary->region_id, 3U);
    EXPECT_FALSE(pool.is_deferred_seed_slot(ordinary->region_id));
    EXPECT_TRUE(ordinary->shell_area.empty());
    EXPECT_TRUE(ordinary->blossom_children.empty());
    EXPECT_EQ(ordinary->match, (Match{NO_REGION, {nullptr, nullptr, 0}}));
    pool.release(ordinary->region_id);
    EXPECT_TRUE(pool.empty());
    pool.reset();
    for (RegionId id = 1; id <= 3; ++id)
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 0U);
}

TEST(StructuredRegionPool, LazyLookupMaterializesOnlyRequestedEndpointAcrossStorageKinds) {
    for (unsigned kind = 0; kind < 3; ++kind) {
        RegionPool pool(2);
        DetectorNode nodes[4];
        if (kind != 0) {
            pool.reserve_contiguous_ids(2);
            for (RegionId id = 1; id <= 2; ++id) {
                if (kind == 1) {
                    pool.construct_reserved(id);
                    pool.release(id);
                } else {
                    pool.construct_reserved_seed(id, VaryingCT::from_base_and_growth(2, 0), &nodes[id - 1],
                        {NO_REGION, {&nodes[id - 1], nullptr, 1}});
                    pool.release_unchanged_seed(id);
                }
            }
            pool.mark_completed_shot();
            pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
        }
        pool.reserve_contiguous_ids(2);
        pool.publish_reserved_matched_seed(1, VaryingCT::from_base_and_growth(6, 0), &nodes[2],
            {2, {&nodes[2], &nodes[3], 9}});
        pool.publish_reserved_matched_seed(2, VaryingCT::from_base_and_growth(10, 0), &nodes[3],
            {1, {&nodes[3], &nodes[2], 9}});
        const auto generation = pool.generation(2);
        const RegionPool& view = pool;
        EXPECT_EQ(view.get(NO_REGION), nullptr);
        const auto& region = view[2];
        EXPECT_FALSE(pool.is_lazy_seed(2));
        EXPECT_TRUE(pool.is_lazy_seed(1));
        EXPECT_TRUE(pool.is_unchanged_seed(2));
        EXPECT_EQ(pool.generation(2), generation);
        EXPECT_EQ(view.get(2), &region);
        EXPECT_EQ(region.match, (Match{1, {&nodes[3], &nodes[2], 9}}));
        EXPECT_EQ(region.radius_value(), VaryingCT::from_base_and_growth(10, 0));
        EXPECT_EQ(region.region_id, 2U);
        EXPECT_EQ(region.pool, &pool);
        EXPECT_EQ(region.blossom_parent, NO_REGION);
        EXPECT_EQ(region.blossom_parent_top, 2U);
        EXPECT_EQ(region.alt_tree_node, nullptr);
        ASSERT_EQ(region.shell_area.size(), 1U);
        EXPECT_EQ(region.shell_area[0], &nodes[3]);
        EXPECT_TRUE(region.blossom_children.empty());
        EXPECT_FALSE(region.shrink_event_tracker.has_desired_time);
        EXPECT_FALSE(region.shrink_event_tracker.has_queued_time);
        // Invalidation itself must not expose unconstructed storage, even if
        // called before another code path requests the full region.
        pool.invalidate_seed_match(1);
        EXPECT_FALSE(pool.is_lazy_seed(1));
        EXPECT_FALSE(pool.is_unchanged_seed(1));
        EXPECT_TRUE(pool.is_unchanged_seed(2));
        pool[1].add_match(pool.get(2), {&nodes[2], &nodes[3], 9});
        EXPECT_FALSE(pool.is_unchanged_seed(2));
        EXPECT_EQ(pool[1].match.edge.loc_to, &nodes[3]);
        pool.release(1);
        pool.release(2);
        EXPECT_TRUE(pool.empty());
    }
}

TEST(StructuredRegionPool, LazyEndpointCanBeInspectedBeforePartnerImport) {
    RegionPool pool(2);
    DetectorNode nodes[6];
    for (size_t shot = 0; shot < 3; ++shot) {
        pool.reserve_contiguous_ids(2);
        auto* from = &nodes[2 * shot];
        auto* to = &nodes[2 * shot + 1];
        const Match match{1, {from, to, 11}};
        pool.publish_reserved_matched_seed(2, VaryingCT::from_base_and_growth(4, 0), from, match);
        ASSERT_TRUE(pool.is_lazy_seed(2));
        EXPECT_FALSE(pool.is_live(1, pool.generation(1)));
        const RegionPool& view = pool;
        // The first shot has no mate descriptor. Later shots leave an old
        // node pointer there, which must not be mistaken for this shot's mate.
        EXPECT_EQ(view[2].match, match);
        EXPECT_EQ(view.get(2)->match.edge.loc_to, to);
        EXPECT_TRUE(pool.is_unchanged_seed(2));
        EXPECT_FALSE(pool.is_live(1, pool.generation(1)));
        pool.publish_reserved_matched_seed(1, VaryingCT::from_base_and_growth(6, 0), to,
            {2, {to, from, 11}});
        EXPECT_TRUE(pool.is_lazy_seed(1));
        EXPECT_EQ(view[1].match, (Match{2, {to, from, 11}}));
        EXPECT_EQ(pool.seed_node(1), to);
        EXPECT_EQ(pool.seed_node(2), from);
        pool.release_unchanged_seed(1);
        pool.release_unchanged_seed(2);
        EXPECT_TRUE(pool.empty());
    }
}

TEST(StructuredRegionPool, LazyMixedLifetimesSurviveRollbackHardResetAndSmallerShots) {
    RegionPool pool(6);
    DetectorNode nodes[6];
    pool.reserve_contiguous_ids(4);
    pool.construct_reserved(1);
    pool.release(1);
    pool.construct_reserved_seed(2, VaryingCT::from_base_and_growth(4, 0), &nodes[1],
        {NO_REGION, {&nodes[1], nullptr, 2}});
    pool.release_unchanged_seed(2);
    pool.publish_reserved_matched_seed(3, VaryingCT::from_base_and_growth(6, 0), &nodes[2],
        {NO_REGION, {&nodes[2], nullptr, 3}});
    pool.release_unchanged_seed(3);
    pool.release(4);
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    EXPECT_EQ(RegionPoolTestAccess::state(pool, 1), 3U);
    EXPECT_TRUE(pool.is_cached_seed_slot(2));
    EXPECT_TRUE(pool.is_deferred_seed_slot(3));
    const auto old_generation = pool.generation(3);
    pool.reserve_contiguous_ids(6);
    for (RegionId id : {1U, 2U, 3U, 5U})
        pool.publish_reserved_matched_seed(id, VaryingCT::from_base_and_growth(2 * id, 0), &nodes[id - 1],
            {NO_REGION, {&nodes[id - 1], nullptr, id}});
    EXPECT_FALSE(pool.is_live(3, old_generation));
    EXPECT_TRUE(pool.is_live(3, pool.generation(3)));
    pool.release(4);  // Reserved-neutral rollback must destroy its real object.
    pool.invalidate_seed_match(5);
    // lazy-neutral, lazy-cached, lazy-raw, free-raw, live and reserved-raw coexist.
    pool.reset();
    EXPECT_TRUE(pool.empty());
    for (RegionId id = 1; id <= 6; ++id) {
        EXPECT_EQ(RegionPoolTestAccess::state(pool, id), 0U);
        EXPECT_EQ(pool.radii.data()[id], VaryingCT{0});
    }
    pool.reserve_contiguous_ids(4);
    for (RegionId id = 1; id <= 4; ++id) {
        pool.publish_reserved_matched_seed(id, VaryingCT::from_base_and_growth(2, 0), &nodes[id - 1],
            {NO_REGION, {&nodes[id - 1], nullptr, id}});
        if (id == 2) pool.get(id);  // Preserve a real cached object above next_.
        pool.release_unchanged_seed(id);
    }
    pool.reserve_contiguous_ids(0);
    EXPECT_EQ(RegionPoolTestAccess::next(pool), 1U);
    EXPECT_EQ(RegionPoolTestAccess::lifetime_end(pool), 5U);
    EXPECT_TRUE(pool.is_cached_seed_slot(2));
    EXPECT_TRUE(pool.is_deferred_seed_slot(4));
    // Destruction must still visit the physical lifetime above the new next_.
}

TEST(StructuredRegionPool, DeferredSeedNodesRemainCleanAcrossRetirementAndResetOwners) {
    RegionPool pool(2);
    DetectorNode nodes[3];
    pool.reserve_contiguous_ids(2);
    pool.publish_reserved_matched_seed(1, VaryingCT::from_base_and_growth(2, 0), &nodes[0],
        {2, {&nodes[0], &nodes[1], 7}}, true);
    pool.publish_reserved_matched_seed(2, VaryingCT::from_base_and_growth(4, 0), &nodes[1],
        {1, {&nodes[1], &nodes[0], 7}}, true);
    for (RegionId id = 1; id <= 2; ++id) {
        EXPECT_TRUE(pool.is_seed_node_deferred(id));
        const auto& node = nodes[id - 1];
        EXPECT_EQ(node.region_that_arrived, NO_REGION);
        EXPECT_EQ(node.reached_from_source, nullptr);
        EXPECT_EQ(node.radius_of_arrival, 0);
        EXPECT_EQ(node.observables_crossed_from_source, 0U);
        EXPECT_FALSE(node.node_event_tracker.has_desired_time);
        EXPECT_FALSE(node.node_event_tracker.has_queued_time);
        pool.release_unchanged_seed(id);
        EXPECT_TRUE(pool.is_seed_node_deferred(id));
    }
    pool.mark_completed_shot();
    pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
    EXPECT_TRUE(pool.is_seed_node_deferred(1));
    EXPECT_TRUE(pool.is_seed_node_deferred(2));
    // An ordinary allocation must not inherit the retired node's skip-reset flag.
    auto* ordinary = pool.allocate();
    ASSERT_EQ(ordinary->region_id, 2U);
    EXPECT_FALSE(pool.is_seed_node_deferred(2));
    EXPECT_TRUE(pool.is_seed_node_deferred(1));
    pool.release(2);
    pool.reserve_contiguous_ids(2);
    pool.construct_reserved_seed(1, VaryingCT::from_base_and_growth(0, 1), &nodes[2], Match{});
    EXPECT_FALSE(pool.is_seed_node_deferred(1));
    pool.publish_reserved_matched_seed(2, VaryingCT::from_base_and_growth(4, 0), &nodes[1],
        {NO_REGION, {&nodes[1], nullptr, 5}}, true);
    EXPECT_TRUE(pool.is_seed_node_deferred(2));
    pool.reset();
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.is_seed_node_deferred(1));
    EXPECT_FALSE(pool.is_seed_node_deferred(2));
    EXPECT_EQ(nodes[1].region_that_arrived, NO_REGION);
    EXPECT_EQ(nodes[1].reached_from_source, nullptr);
}

TEST(StructuredRegionPool, DeferredSeedNodeConstLookupRestoresNodeAcrossStorageKinds) {
    for (unsigned kind = 0; kind < 3; ++kind) {
        RegionPool pool(1);
        DetectorNode nodes[2];
        if (kind != 0) {
            pool.reserve_contiguous_ids(1);
            if (kind == 1) {
                pool.construct_reserved(1);
                pool.release(1);
            } else {
                pool.construct_reserved_seed(1, VaryingCT::from_base_and_growth(2, 0), &nodes[0],
                    {NO_REGION, {&nodes[0], nullptr, 3}});
                pool.release_unchanged_seed(1);
            }
            pool.mark_completed_shot();
            pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
        }
        pool.reserve_contiguous_ids(1);
        pool.publish_reserved_matched_seed(1, VaryingCT::from_base_and_growth(8, 0), &nodes[1],
            {NO_REGION, {&nodes[1], nullptr, 9}}, true);
        EXPECT_TRUE(pool.is_lazy_seed(1));
        EXPECT_TRUE(pool.is_seed_node_deferred(1));
        EXPECT_EQ(nodes[1].region_that_arrived, NO_REGION);
        const RegionPool& view = pool;
        const auto* region = view.get(1);
        EXPECT_EQ(region->match, (Match{NO_REGION, {&nodes[1], nullptr, 9}}));
        EXPECT_EQ(nodes[1].region_that_arrived, 1U);
        EXPECT_EQ(nodes[1].reached_from_source, &nodes[1]);
        EXPECT_EQ(nodes[1].radius_of_arrival, 0);
        EXPECT_EQ(nodes[1].observables_crossed_from_source, 0U);
        EXPECT_FALSE(nodes[1].node_event_tracker.has_desired_time);
        EXPECT_FALSE(nodes[1].node_event_tracker.has_queued_time);
        EXPECT_FALSE(pool.is_seed_node_deferred(1));
        EXPECT_FALSE(pool.is_lazy_seed(1));
        EXPECT_TRUE(pool.is_unchanged_seed(1));
        EXPECT_EQ(view.get(1), region);
        pool.invalidate_seed_match(1);
        EXPECT_FALSE(pool.is_unchanged_seed(1));
        pool.release(1);
        EXPECT_FALSE(pool.is_seed_node_deferred(1));
    }
}

TEST(StructuredRegionPool, DeferredSeedNodeInvalidationRestoresNodeBeforeMutation) {
    RegionPool pool(1);
    DetectorNode node;
    pool.reserve_contiguous_ids(1);
    pool.publish_reserved_matched_seed(1, VaryingCT::from_base_and_growth(4, 0), &node,
        {NO_REGION, {&node, nullptr, 7}}, true);
    pool.invalidate_seed_match(1);
    EXPECT_FALSE(pool.is_seed_node_deferred(1));
    EXPECT_FALSE(pool.is_lazy_seed(1));
    EXPECT_FALSE(pool.is_unchanged_seed(1));
    EXPECT_EQ(node.region_that_arrived, 1U);
    EXPECT_EQ(node.reached_from_source, &node);
    EXPECT_EQ(pool[1].shell_area[0], &node);
    pool.release(1);
}

TEST(StructuredRegionPool, CompactEndpointsMaterializeBeforeMatePublicationAndReuseAllStorageKinds) {
    for (unsigned kind = 0; kind < 3; ++kind) {
        RegionPool pool(2);
        DetectorNode nodes[4];
        pool.bind_compact_seed_nodes(nodes, 4);
        if (kind != 0) {
            pool.reserve_contiguous_ids(2);
            for (RegionId id : {1U, 2U}) {
                if (kind == 1) {
                    pool.construct_reserved(id);
                    pool.release(id);
                } else {
                    auto* node = &nodes[id - 1];
                    pool.construct_reserved_seed(id, VaryingCT::from_base_and_growth(2, 0), node,
                        {NO_REGION, {node, nullptr, 5}});
                    pool.release_unchanged_seed(id);
                }
            }
            pool.mark_completed_shot();
            pool.prepare_neutral_range(1, pool.take_neutral_reset_end());
        }
        pool.reserve_contiguous_ids(2);
        const auto generation = pool.generation(1);
        // Only endpoint 2 has been published. Its own descriptor contains the
        // complete other endpoint, independent of the reserved mate slot.
        pool.publish_reserved_compact_seed(2, VaryingCT::from_base_and_growth(4, 0), 3, 2, 1, 9);
        EXPECT_TRUE(RegionPoolTestAccess::compact_endpoints(pool, 2));
        EXPECT_EQ(pool.seed_node(2), &nodes[3]);
        EXPECT_TRUE(RegionPoolTestAccess::compact_endpoints(pool, 2));
        const RegionPool& view = pool;
        const auto* region = view.get(2);
        EXPECT_EQ(region->match, (Match{1, {&nodes[3], &nodes[2], 9}}));
        EXPECT_EQ(nodes[3].region_that_arrived, 2U);
        EXPECT_FALSE(pool.is_seed_node_deferred(2));
        EXPECT_FALSE(RegionPoolTestAccess::compact_endpoints(pool, 2));
        EXPECT_TRUE(pool.is_unchanged_seed(2));
        pool.publish_reserved_compact_seed(1, VaryingCT::from_base_and_growth(2, 0), 2, 3, 2, 9);
        pool.invalidate_seed_match(2);
        EXPECT_FALSE(pool.is_unchanged_seed(2));
        EXPECT_EQ(pool[1].match, (Match{2, {&nodes[2], &nodes[3], 9}}));
        pool.release(2);
        pool.release_unchanged_seed(1);
        pool.reserve_contiguous_ids(2);
        EXPECT_GT(pool.generation(1), generation);
        pool.publish_reserved_compact_seed(1, VaryingCT::from_base_and_growth(6, 0), 0, UINT32_MAX, NO_REGION, 13);
        EXPECT_EQ(pool.seed_node(1), &nodes[0]);
        EXPECT_TRUE(pool.is_seed_node_deferred(1));
        // Hard reset includes one unpublished reservation and one compact seed.
        pool.reset();
        EXPECT_TRUE(pool.empty());
        EXPECT_FALSE(pool.is_seed_node_deferred(1));
        EXPECT_FALSE(RegionPoolTestAccess::compact_endpoints(pool, 1));
        auto* ordinary = pool.allocate();
        EXPECT_FALSE(pool.is_unchanged_seed(ordinary->region_id));
        EXPECT_FALSE(RegionPoolTestAccess::compact_endpoints(pool, ordinary->region_id));
        pool.release(ordinary->region_id);
    }
}
