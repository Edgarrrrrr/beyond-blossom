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

#ifndef BB_STRUCTURED_DETECTOR_NODE_H
#define BB_STRUCTURED_DETECTOR_NODE_H

#include <optional>
#include <vector>

#include "beyond_blossom/core/implied_weights.h"
#include "beyond_blossom/core/interop/varying.h"
#include "beyond_blossom/structured/flooder/graph_fill_region.h"
#include "beyond_blossom/structured/grid/static_grid.h"
#include "beyond_blossom/structured/tracker/queued_event_tracker.h"

namespace beyond_blossom::structured {

/// A detector node is a location where a detection event might occur.
///
/// It corresponds to a potential symptom that could be seen, and can
/// be used to infer which errors have occurred.
///
/// There is a 1:1 correspondence between detector nodes in a matching
/// graph and the DETECTOR annotations in a Stim circuit.
class alignas(64) DetectorNode {
   public:
    void set_top_region(GraphFillRegion* region) {
        grid_component->collision_states[grid_cell].top_region_id = region ? region->region_id : 0;
    }
    void set_wrapped_radius(int32_t value) {
        grid_component->collision_states[grid_cell].wrapped_radius_cached = value;
    }
    size_t edge_slots() const {
        return STRUCTURED_STORAGE_DIRECTIONS;
    }
    bool has_edge(size_t h) const {
        return h < edge_slots() && (grid_component->present[grid_cell] & (uint16_t{1} << h));
    }
    DetectorNode* neighbor(size_t h) const {
        return grid_component->neighbor(grid_cell, h);
    }
    weight_int& edge_weight(size_t h) {
        return grid_component->weights[grid_cell][h];
    }
    weight_int edge_weight(size_t h) const {
        return grid_component->weights[grid_cell][h];
    }
    obs_int edge_observables(size_t h) const {
        return grid_component->observables[grid_cell][h];
    }
    std::vector<ImpliedWeight>& edge_implied_weights(size_t h) {
        return grid_component->implied_weights[grid_cell * STRUCTURED_STORAGE_DIRECTIONS + h];
    }
    DetectorNode()
        : region_that_arrived(NO_REGION),
          reached_from_source(nullptr),
          observables_crossed_from_source(0),
          radius_of_arrival(0) {
    }

    /// == Ephemeral fields used to track algorithmic state during matching. ==
    /// The region that reached and owns this node.
    RegionId region_that_arrived;
    // The 12-byte tracker fills the alignment gap next to the 4-byte region ID.
    /// Manages the next "look at me!" event for the node.
    QueuedEventTracker node_event_tracker;
    // The collision-state ID is the sole top-region ownership record.
    RegionId top_region_id() const {
        return grid_component->collision_states[grid_cell].top_region_id;
    }
    GraphFillRegion* top_region() const {
        auto id = top_region_id();
        return id ? &(*grid_component->region_pool)[id] : nullptr;
    }
    GraphFillRegion* owner_region() const {
        materialize_seed_state();
        return region_that_arrived ? &(*grid_component->region_pool)[region_that_arrived] : nullptr;
    }
    // Captained import can represent a frozen singleton entirely by its grid
    // collision ID. Queries that need node/source fields resolve that state.
    void materialize_seed_state() const {
        if (region_that_arrived == NO_REGION && grid_component) {
            const auto id = top_region_id();
            if (id != NO_REGION) (void)grid_component->region_pool->get(id);
        }
    }
    /// Of the detection events within the owning region, which one is this node linked to.
    DetectorNode* reached_from_source;
    /// Which observables are crossed, travelling from this node to the source
    /// detection event that reached it. Must be 0 if reached_from_source == nullptr.
    obs_int observables_crossed_from_source;
    /// This was the radius of `region_that_arrived` when it arrived at this node. Otherwise 0.
    cumulative_time_int radius_of_arrival;

    /// Radius at this cell, read directly from structured state.
    inline VaryingCT local_radius() const {
        if (top_region_id() == NO_REGION)
            return VaryingCT{0};
        return grid_component->region_pool->radii.data()[top_region_id()] +
               grid_component->collision_states[grid_cell].wrapped_radius_cached;
    }

    /// Determines the region that owns this node which is a child of this node's top region.
    GraphFillRegion* heir_region_on_shatter() const;

    /// Check if this node is part the same top-level region as another.
    /// Note that they may have different lower level owners that still merge into the same top level owned.
    inline bool has_same_owner_as(const DetectorNode& other) const {
        return grid_component->region_pool == other.grid_component->region_pool &&
               top_region_id() == other.top_region_id();
    }

    /// Zero all ephemeral fields.
    /// Doesn't free anything or propagate a signal to other objects. Just zeros the fields.
    void reset();

    // Final extraction already retired arrived/top. Clear the remaining
    // ephemeral state on this shot's importer without writing those IDs twice.
    void reset_after_final_extraction() noexcept;

    size_t index_of_neighbor(DetectorNode* neighbor) const;

    /// The 'wrapped radius' is the amount of region growth that has occurred past the point where
    /// this node was hit, up until the last time a blossom was formed around this node. It is the
    /// sum of the radius of (always frozen) not-top-level regions containing this node. It is a
    /// useful intermediate value for quickly computing the local radius of the node, because it
    /// accounts for everything except the (potentially varying) top level region.
    int32_t compute_wrapped_radius() const;

    /// It works out the local radius of the node, but bounds it to not go beyond the given bounding
    /// region. This is useful for understanding the internal structure of the blossoms.
    ///
    /// This is a utility method used to help with drawing the internal state of the algorithm.
    cumulative_time_int compute_local_radius_at_time_bounded_by_region(
        cumulative_time_int time, const GraphFillRegion& bounding_region) const;

    /// The 'stitch radius' is the place where ownership of an edge stops.
    ///
    /// This is a utility method used to help with drawing the internal state of the algorithm.
    ///
    /// Args:
    ///     time: In case the result is varying, this specifies exactly which time to look at it.
    ///     bounding_region: This says which graph fill region is being "focused on". In particular,
    ///         this may be a frozen region within a blossom in which case the goal is to find the
    ///         exact place along edges where the region froze or where two source nodes were
    ///         bumping up against each other.
    ///     neighbor_index: The edge of interest.
    ///
    /// Returns:
    ///     Empty: This is a fully internal edge. There is no region transition along it.
    ///     An integer: This is the distance, starting from *this node and running along the edge,
    ///         to the transition point. This may be equal to 0, or equal to the full length of the
    ///         edge, or somewhere in between.
    std::optional<float> compute_stitch_radius_at_time_bounded_by_region_towards_neighbor(
        cumulative_time_int time, const GraphFillRegion& bounding_region, size_t neighbor_index) const;

    /// Permanent location in the structured grid. Adjacency is stored only in
    /// the grid arrays; construction-only vectors belong to a separate type.
    StaticGridComponent<DetectorNode, obs_int>* grid_component = nullptr;
    size_t grid_cell = 0;
    size_t detector_id = SIZE_MAX;
};

static_assert(sizeof(void*) != 8 || sizeof(DetectorNode) == 64);
static_assert(alignof(DetectorNode) == 64);

}  // namespace beyond_blossom::structured

#endif  // BB_STRUCTURED_DETECTOR_NODE_H
