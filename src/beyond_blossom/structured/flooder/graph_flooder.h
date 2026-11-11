// Copyright 2022 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
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

#ifndef BB_STRUCTURED_GRAPH_FLOODER_H
#define BB_STRUCTURED_GRAPH_FLOODER_H

#include <queue>

#include "beyond_blossom/core/arena.h"
#include "beyond_blossom/structured/flooder/graph.h"
#include "beyond_blossom/structured/flooder/graph_fill_region.h"
#include "beyond_blossom/structured/flooder_matcher_interop/mwpm_event.h"
#include "beyond_blossom/structured/flooder_matcher_interop/region_edge.h"
#include "beyond_blossom/structured/tracker/flood_check_event.h"
#include "beyond_blossom/structured/tracker/radix_heap_queue.h"

namespace beyond_blossom::structured {

struct GraphFlooder {
    /// The graph of detector nodes that is being flooded.
    MatchingGraph graph;
    /// Tracks the next thing that will occur as flooding proceeds.
    /// The events are "tentative" because processing an event may remove another,
    /// for example if a region stops growing due to colliding with another region
    /// then the growing region will no longer reach other nodes even if those
    /// events were scheduled in this queue before the region collision was processed.
    ///
    /// Events are ordered by time; by when they will occur in a timeline.
    radix_heap_queue<false> queue;

    // Pool owns both actual objects and their identically indexed radii.
    std::unique_ptr<RegionPool> regions;
    RegionRadiusStore* radius_store = nullptr;
    GraphFillRegion* allocate_region();

    std::vector<CompressedEdge> match_edges;

    /// These are the detection events that would occur if an error occurred on every edge that has a negative weight.
    /// Stored as a sorted vector of indices of detection events.
    std::vector<uint64_t> negative_weight_detection_events;
    /// These are the observables that would be flipped if an error occurred on every edge that has a negative weight.
    /// Stored as a sorted vector of indices of observables.
    std::vector<size_t> negative_weight_observables;
    /// Observable mask corresponding to the observables that would be flipped if an error occurred on every edge that
    /// has a negative weight. Only used for fewer than 64 (=sizeof(beyond_blossom::obs_int)*8) observables.
    beyond_blossom::obs_int negative_weight_obs_mask;
    /// The sum of the edge weights of all edges with negative edge weights.
    beyond_blossom::total_weight_int negative_weight_sum;

    GraphFlooder();
    explicit GraphFlooder(MatchingGraph graph);
    GraphFlooder(GraphFlooder&&) noexcept;
    MwpmEvent run_until_next_mwpm_notification();
    void set_region_growing(beyond_blossom::structured::GraphFillRegion& region);
    void set_region_frozen(beyond_blossom::structured::GraphFillRegion& region);
    void set_region_shrinking(beyond_blossom::structured::GraphFillRegion& region);
    GraphFillRegion* create_blossom(std::vector<RegionEdge>& contained_regions);
    void schedule_tentative_shrink_event(GraphFillRegion& region);
    void reschedule_events_at_detector_node(DetectorNode& detector_node);
    // Install ownership and the seed shell without scheduling events. Used by
    // bulk warm starts so every neighbor sees the final initial dual state.
    void initialize_region_at_empty_detector_node(GraphFillRegion& region, DetectorNode& detector_node);
    void do_region_created_at_empty_detector_node(GraphFillRegion& region, DetectorNode& detector_node);
    void do_region_arriving_at_empty_detector_node(
        GraphFillRegion& region, DetectorNode& empty_node, const DetectorNode& from_node, size_t from_to_empty_index);
    MwpmEvent do_region_shrinking(GraphFillRegion& shrinking_region);
    beyond_blossom::structured::MwpmEvent do_neighbor_interaction(DetectorNode& src, size_t src_to_dst_index, DetectorNode& dst);
    beyond_blossom::structured::MwpmEvent do_region_hit_boundary_interaction(DetectorNode& node, size_t boundary_handle = 0);
    static MwpmEvent do_degenerate_implosion(const GraphFillRegion& region);
    static MwpmEvent do_blossom_shattering(GraphFillRegion& region);
    bool dequeue_decision(beyond_blossom::structured::FloodCheckEvent ev);
    std::pair<size_t, beyond_blossom::cumulative_time_int> find_next_event_at_node_returning_neighbor_index_and_time(
        const DetectorNode& detector_node) const;
    // Read-only initial-event calculation after every imported region/node is
    // published. Workers write neither the queue nor shared profiling counters.
    std::pair<size_t, beyond_blossom::cumulative_time_int> prepare_initial_event_at_detector_node(
        const DetectorNode& detector_node) const;
    beyond_blossom::structured::MwpmEvent do_look_at_node_event(DetectorNode& node);

    beyond_blossom::structured::FloodCheckEvent dequeue_valid();
    beyond_blossom::structured::MwpmEvent process_tentative_event_returning_mwpm_event(FloodCheckEvent tentative_event);

    void sync_negative_weight_observables_and_detection_events();
};

}  // namespace beyond_blossom::structured

#endif  // BB_STRUCTURED_GRAPH_FLOODER_H
