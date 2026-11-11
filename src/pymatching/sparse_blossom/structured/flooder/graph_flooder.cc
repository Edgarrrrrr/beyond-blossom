// Copyright 2022 PyMatching Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pymatching/sparse_blossom/structured/flooder/graph_flooder.h"

#include "pymatching/sparse_blossom/flooder_matcher_interop/varying.h"
#include "pymatching/sparse_blossom/profile/profile.h"
#include "pymatching/sparse_blossom/structured/flooder/graph.h"
#include "pymatching/sparse_blossom/structured/flooder/graph_fill_region.h"
#include "pymatching/sparse_blossom/structured/matcher/alternating_tree.h"

namespace pm::structured {

namespace {
// Collision ownership is published even when an untouched matched singleton
// has left its node fields clean. Complete them before constructing paths or
// writing event trackers; the matching kernels themselves only need grid state.
void materialize_deferred_seed_node(RegionPool& regions, DetectorNode& node) {
    if (node.region_that_arrived == NO_REGION) {
        const auto id = node.top_region_id();
        if (id != NO_REGION) {
            regions.get(id);
            assert(node.region_that_arrived != NO_REGION);
        }
    }
}
}  // namespace

GraphFlooder::GraphFlooder(MatchingGraph graph)
    : graph(std::move(graph)), negative_weight_obs_mask(0), negative_weight_sum(0) {
    if (this->graph.num_nodes > (std::numeric_limits<RegionId>::max() - size_t{2}) / 2)
        throw std::length_error("Too many detector nodes for the region object pool.");
    regions = std::make_unique<RegionPool>(2 * this->graph.num_nodes + 1);
    radius_store = &regions->radii;
    if (this->graph.grid)
        for (auto &component : this->graph.grid->components)
            component->region_pool = regions.get();
}

GraphFillRegion *GraphFlooder::allocate_region() {
    return regions->allocate();
}

GraphFlooder::GraphFlooder(GraphFlooder &&flooder) noexcept
    : graph(std::move(flooder.graph)),
      queue(std::move(flooder.queue)),
      regions(std::move(flooder.regions)),
      radius_store(flooder.radius_store),
      match_edges(std::move(flooder.match_edges)),
      negative_weight_detection_events(std::move(flooder.negative_weight_detection_events)),
      negative_weight_observables(std::move(flooder.negative_weight_observables)),
      negative_weight_obs_mask(flooder.negative_weight_obs_mask),
      negative_weight_sum(flooder.negative_weight_sum) {
}

void GraphFlooder::initialize_region_at_empty_detector_node(GraphFillRegion &region, DetectorNode &detector_node) {
    pm::profile::record_counter(&pm::profile::ShotStats::detection_regions_created);
    pm::profile::record_region_created();
    detector_node.reached_from_source = &detector_node;
    detector_node.radius_of_arrival = 0;
    detector_node.region_that_arrived = region.region_id;
    detector_node.set_top_region(&region);
    detector_node.set_wrapped_radius(0);
    region.shell_area.push_back(&detector_node);
    pm::profile::record_region_shell_area(region.shell_area.size());
}

void GraphFlooder::do_region_created_at_empty_detector_node(GraphFillRegion &region, DetectorNode &detector_node) {
    initialize_region_at_empty_detector_node(region, detector_node);
    reschedule_events_at_detector_node(detector_node);
}

std::pair<size_t, cumulative_time_int> GraphFlooder::find_next_event_at_node_returning_neighbor_index_and_time(
    const DetectorNode &detector_node) const {
    pm::profile::ScopedRange range(pm::profile::RangeId::NodeNext);
    pm::profile::record_counter(
        &pm::profile::ShotStats::neighbor_scans,
        std::popcount(detector_node.grid_component->present[detector_node.grid_cell]));
    return detector_node.grid_component->matching_next(
        *detector_node.grid_component, detector_node, radius_store->data());
}

std::pair<size_t, cumulative_time_int> GraphFlooder::prepare_initial_event_at_detector_node(
    const DetectorNode &detector_node) const {
    return detector_node.grid_component->matching_next(
        *detector_node.grid_component, detector_node, radius_store->data());
}

void GraphFlooder::reschedule_events_at_detector_node(DetectorNode &detector_node) {
    materialize_deferred_seed_node(*regions, detector_node);
    pm::profile::record_counter(&pm::profile::ShotStats::node_reschedules);
    auto x = find_next_event_at_node_returning_neighbor_index_and_time(detector_node);
    if (x.first == SIZE_MAX) {
        detector_node.node_event_tracker.set_no_desired_event();
    } else {
        detector_node.node_event_tracker.set_desired_event(
            {
                &detector_node,
                cyclic_time_int{x.second},
            },
            queue);
    }
}

void GraphFlooder::schedule_tentative_shrink_event(GraphFillRegion &region) {
    pm::profile::record_counter(&pm::profile::ShotStats::shrink_event_schedules);
    cumulative_time_int t;
    if (region.shell_area.empty()) {
        t = region.radius_value().time_of_x_intercept_for_shrinking();
    } else {
        t = region.shell_area.back()->local_radius().time_of_x_intercept_for_shrinking();
    }
    region.shrink_event_tracker.set_desired_event(
        {
            region.region_id,
            regions->generation(region.region_id),
            cyclic_time_int{t},
        },
        queue);
}

void GraphFlooder::do_region_arriving_at_empty_detector_node(
    GraphFillRegion &region, DetectorNode &empty_node, const DetectorNode &from_node, size_t from_to_empty_index) {
    auto range = pm::profile::counted_range(pm::profile::RangeId::NodeArrive, &pm::profile::ShotStats::flooder_arrive);
    empty_node.observables_crossed_from_source =
        (from_node.observables_crossed_from_source ^ from_node.edge_observables(from_to_empty_index));
    empty_node.reached_from_source = from_node.reached_from_source;
    empty_node.radius_of_arrival = region.radius_value().get_distance_at_time(queue.cur_time);
    empty_node.region_that_arrived = region.region_id;
    empty_node.set_top_region(regions->get(region.blossom_parent_top));
    empty_node.set_wrapped_radius(empty_node.compute_wrapped_radius());
    regions->invalidate_seed_match(region.region_id);
    region.shell_area.push_back(&empty_node);
    pm::profile::record_region_shell_area(region.shell_area.size());
    reschedule_events_at_detector_node(empty_node);
}

MwpmEvent GraphFlooder::do_region_shrinking(GraphFillRegion &region) {
    if (region.shell_area.empty()) {
        auto range = pm::profile::counted_range(
            pm::profile::RangeId::BlossomImplode, &pm::profile::ShotStats::flooder_implode_blossom);
        return do_blossom_shattering(region);
    } else if (region.shell_area.size() == 1 && region.blossom_children.empty()) {
        auto range = pm::profile::counted_range(
            pm::profile::RangeId::RegionImplode, &pm::profile::ShotStats::flooder_implode_trivial);
        return do_degenerate_implosion(region);
    } else {
        auto range =
            pm::profile::counted_range(pm::profile::RangeId::RegionLeave, &pm::profile::ShotStats::flooder_leave);
        auto leaving_node = region.shell_area.back();
        region.shell_area.pop_back();
        pm::profile::record_region_shell_area(region.shell_area.size());
        leaving_node->region_that_arrived = NO_REGION;
        leaving_node->set_top_region(nullptr);
        leaving_node->set_wrapped_radius(0);
        leaving_node->reached_from_source = nullptr;
        leaving_node->radius_of_arrival = 0;
        leaving_node->observables_crossed_from_source = 0;
        reschedule_events_at_detector_node(*leaving_node);
        schedule_tentative_shrink_event(region);
        return MwpmEvent::no_event();
    }
}

MwpmEvent GraphFlooder::do_neighbor_interaction(DetectorNode &src, size_t src_to_dst_index, DetectorNode &dst) {
    materialize_deferred_seed_node(*regions, src);
    materialize_deferred_seed_node(*regions, dst);
    // First check if one region is moving into an empty location
    if (src.region_that_arrived && !dst.region_that_arrived) {
        do_region_arriving_at_empty_detector_node(*src.top_region(), dst, src, src_to_dst_index);
        return MwpmEvent::no_event();
    } else if (dst.region_that_arrived && !src.region_that_arrived) {
        size_t reverse = src_to_dst_index ^ 1;
        do_region_arriving_at_empty_detector_node(*dst.top_region(), src, dst, reverse);
        return MwpmEvent::no_event();
    } else {
        // Two regions colliding
        auto range = pm::profile::counted_range(
            pm::profile::RangeId::NodeHitRegion, &pm::profile::ShotStats::flooder_collide_region);
        return RegionHitRegionEventData{
            src.top_region_id(),
            dst.top_region_id(),
            CompressedEdge{
                src.reached_from_source,
                dst.reached_from_source,
                src.observables_crossed_from_source ^ dst.observables_crossed_from_source ^
                    src.edge_observables(src_to_dst_index)},
        };
    }
}

MwpmEvent GraphFlooder::do_region_hit_boundary_interaction(DetectorNode &node, size_t boundary_handle) {
    materialize_deferred_seed_node(*regions, node);
    auto range = pm::profile::counted_range(
        pm::profile::RangeId::NodeHitBoundary, &pm::profile::ShotStats::flooder_collide_boundary);
    return RegionHitBoundaryEventData{
        node.top_region_id(),
        CompressedEdge{
            node.reached_from_source,
            nullptr,
            node.observables_crossed_from_source ^ node.edge_observables(boundary_handle)}};
}

MwpmEvent GraphFlooder::do_degenerate_implosion(const GraphFillRegion &region) {
    return RegionHitRegionEventData{
        region.alt_tree_node->parent.alt_tree_node->outer_region,
        region.alt_tree_node->outer_region,
        CompressedEdge{
            region.alt_tree_node->parent.edge.loc_to,
            region.alt_tree_node->inner_to_outer_edge.loc_to,
            region.alt_tree_node->inner_to_outer_edge.obs_mask ^ region.alt_tree_node->parent.edge.obs_mask}};
}

MwpmEvent GraphFlooder::do_blossom_shattering(GraphFillRegion &region) {
    return BlossomShatterEventData{
        region.region_id,
        region.alt_tree_node->parent.edge.loc_from->heir_region_on_shatter()->region_id,
        region.alt_tree_node->inner_to_outer_edge.loc_from->heir_region_on_shatter()->region_id};
}

GraphFillRegion *GraphFlooder::create_blossom(std::vector<RegionEdge> &contained_regions) {
    auto range = pm::profile::updated_range(pm::profile::RangeId::BlossomCreate, [&](pm::profile::ShotStats &stats) {
        stats.blossoms_created++;
        stats.blossom_created_cycle_length_hist.add(contained_regions.size());
    });
    auto blossom_region = allocate_region();
    pm::profile::record_region_created();
    blossom_region->radius_value() = VaryingCT::growing_varying_with_zero_distance_at_time(queue.cur_time);
    blossom_region->blossom_children = std::move(contained_regions);
    pm::profile::record_region_shell_area(blossom_region->shell_area.size());
    for (auto &region_edge : blossom_region->blossom_children) {
        auto freeze_range = pm::profile::counted_range(
            pm::profile::RangeId::RegionSetFrozen, &pm::profile::ShotStats::region_set_frozen);
        (*regions)[region_edge.region].radius_value() =
            (*regions)[region_edge.region].radius_value().then_frozen_at_time(queue.cur_time);
        (*regions)[region_edge.region].wrap_into_blossom(blossom_region);
        (*regions)[region_edge.region].shrink_event_tracker.set_no_desired_event();
    }

    blossom_region->do_op_for_each_node_in_total_area([this](DetectorNode *n) {
        reschedule_events_at_detector_node(*n);
    });

    return blossom_region;
}

bool GraphFlooder::dequeue_decision(FloodCheckEvent ev) {
    switch (ev.tentative_event_type) {
        case FloodCheckEventType::LOOK_AT_NODE: {
            auto &node = *ev.data_look_at_node;
            return node.node_event_tracker.dequeue_decision(ev, queue);
        }
        case FloodCheckEventType::LOOK_AT_SHRINKING_REGION: {
            if (!regions->is_live(ev.data_look_at_shrinking_region.id, ev.data_look_at_shrinking_region.generation)) {
#if PM_STRUCTURED_PROFILE_COUNTERS
                ++queue.profile.stale_cancelled;
#endif
#if PYMATCHING_ENABLE_PROFILE
                auto kind = profile_queue_kind_for_event(ev.tentative_event_type);
                auto range = pm::profile::queue_scoped_range(kind, "stale", pm::profile::Color::Brown);
                pm::profile::record_queue_valid(kind, false);
#endif
                return false;
            }
            auto &region = (*regions)[ev.data_look_at_shrinking_region.id];
            return region.shrink_event_tracker.dequeue_decision(ev, queue);
        }
        case FloodCheckEventType::NO_FLOOD_CHECK_EVENT:
            return true;
        default:
            throw std::invalid_argument("Unrecognized event type.");
    }
}

FloodCheckEvent GraphFlooder::dequeue_valid() {
    while (true) {
        FloodCheckEvent ev = queue.dequeue();
        if (dequeue_decision(ev)) {
            return ev;
        }
    }
}

void GraphFlooder::set_region_growing(GraphFillRegion &region) {
    regions->invalidate_seed_match(region.region_id);
    auto range =
        pm::profile::counted_range(pm::profile::RangeId::RegionSetGrowing, &pm::profile::ShotStats::region_set_growing);
    region.radius_value() = region.radius_value().then_growing_at_time(queue.cur_time);

    // No shrinking event can occur while growing.
    region.shrink_event_tracker.set_no_desired_event();

    // Node events can occur while growing, and events in the queue may occur sooner than
    // previously scheduled. Therefore, we must reschedule all the nodes.
    region.do_op_for_each_node_in_total_area([this](DetectorNode *n) {
        reschedule_events_at_detector_node(*n);
    });
}

void GraphFlooder::set_region_frozen(GraphFillRegion &region) {
    regions->invalidate_seed_match(region.region_id);
    auto range =
        pm::profile::counted_range(pm::profile::RangeId::RegionSetFrozen, &pm::profile::ShotStats::region_set_frozen);
    bool was_shrinking = region.radius_value().is_shrinking();
    region.radius_value() = region.radius_value().then_frozen_at_time(queue.cur_time);

    // No shrinking event can occur while frozen.
    region.shrink_event_tracker.set_no_desired_event();

    // Node events can occur while frozen, from other regions growing into this one.
    // However, those events can only be sooner than the currently scheduled events
    // if the region was previously shrinking (as opposed to growing).
    if (was_shrinking) {
        region.do_op_for_each_node_in_total_area([&](DetectorNode *n) {
            reschedule_events_at_detector_node(*n);
        });
    }
}

void GraphFlooder::set_region_shrinking(GraphFillRegion &region) {
    regions->invalidate_seed_match(region.region_id);
    auto range = pm::profile::counted_range(
        pm::profile::RangeId::RegionSetShrinking, &pm::profile::ShotStats::region_set_shrinking);
    region.radius_value() = region.radius_value().then_shrinking_at_time(queue.cur_time);

    // Shrinking events can now occur.
    schedule_tentative_shrink_event(region);

    // No node events can occur while shrinking.
    region.do_op_for_each_node_in_total_area([&](DetectorNode *n) {
        n->node_event_tracker.set_no_desired_event();
    });
}

MwpmEvent GraphFlooder::do_look_at_node_event(DetectorNode &node) {
    materialize_deferred_seed_node(*regions, node);
    auto range = pm::profile::counted_range(pm::profile::RangeId::NodeLook, &pm::profile::ShotStats::look_at_node);
    auto next = find_next_event_at_node_returning_neighbor_index_and_time(node);
    if (next.second == queue.cur_time) {
        // Need to revisit this node immediately after the mwpm event is handled. There may be an event to handle
        // along another edge, or even along the same edge at a later time (e.g. if this event isn't the *first* event
        // for the neighbor along the current edge when the neighbor is being rescheduled).
        node.node_event_tracker.set_desired_event(
            {
                &node,
                cyclic_time_int{queue.cur_time},
            },
            queue);

        auto *neighbor = node.neighbor(next.first);
        if (neighbor == nullptr) {
            return do_region_hit_boundary_interaction(node, next.first);
        }
        return do_neighbor_interaction(node, next.first, *neighbor);
    } else if (next.first != SIZE_MAX) {
        // Need to revisit this node at a later time.
        node.node_event_tracker.set_desired_event(
            {
                &node,
                cyclic_time_int{next.second},
            },
            queue);
    }

    return MwpmEvent::no_event();
}

MwpmEvent GraphFlooder::process_tentative_event_returning_mwpm_event(FloodCheckEvent tentative_event) {
    switch (tentative_event.tentative_event_type) {
        case LOOK_AT_NODE: {
            return do_look_at_node_event(*tentative_event.data_look_at_node);
        }
        case LOOK_AT_SHRINKING_REGION: {
            auto range =
                pm::profile::counted_range(pm::profile::RangeId::RegionLook, &pm::profile::ShotStats::look_at_region);
            return do_region_shrinking((*regions)[tentative_event.data_look_at_shrinking_region.id]);
        }
        default:
            throw std::invalid_argument("Unknown tentative event type.");
    }
}

MwpmEvent GraphFlooder::run_until_next_mwpm_notification() {
    auto range = pm::profile::counted_range(pm::profile::RangeId::DualPhase, &pm::profile::ShotStats::dual_phase_runs);
    while (true) {
        FloodCheckEvent tentative_event = dequeue_valid();
        if (tentative_event.tentative_event_type == NO_FLOOD_CHECK_EVENT) {
            return MwpmEvent::no_event();
        }
        MwpmEvent notification = process_tentative_event_returning_mwpm_event(tentative_event);
        if (notification.event_type != NO_EVENT) {
            return notification;
        }
    }
}

void GraphFlooder::sync_negative_weight_observables_and_detection_events() {
    /// Move set of negative weight detection events into a sorted vector, for faster processing during decoding
    negative_weight_detection_events.clear();
    negative_weight_detection_events.reserve(graph.negative_weight_detection_events_set.size());
    for (auto &det : graph.negative_weight_detection_events_set)
        negative_weight_detection_events.push_back(det);
    /// Move set of negative weight observable indices into a sorted vector, for faster processing during decoding
    negative_weight_observables.clear();
    negative_weight_observables.reserve(graph.negative_weight_observables_set.size());
    for (auto &obs : graph.negative_weight_observables_set)
        negative_weight_observables.push_back(obs);
    negative_weight_obs_mask = 0;
    if (graph.num_observables <= sizeof(pm::obs_int) * 8) {
        // Compute the observable mask for 64 or fewer observables
        for (auto &i : negative_weight_observables)
            negative_weight_obs_mask ^= (pm::obs_int)1 << i;
    }
    negative_weight_sum = graph.negative_weight_sum;
}

GraphFlooder::GraphFlooder() : negative_weight_obs_mask(0), negative_weight_sum(0) {
}

}  // namespace pm::structured
