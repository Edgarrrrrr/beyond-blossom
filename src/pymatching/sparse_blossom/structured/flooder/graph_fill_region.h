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

#ifndef PYMATCHING_STRUCTURED_GRAPH_FILL_REGION_H
#define PYMATCHING_STRUCTURED_GRAPH_FILL_REGION_H

#include <vector>

#include "pymatching/sparse_blossom/flooder_matcher_interop/varying.h"
#include "pymatching/sparse_blossom/structured/flooder/match.h"
#include "pymatching/sparse_blossom/structured/flooder/region_pool.h"
#include "pymatching/sparse_blossom/structured/flooder/region_shell.h"
#include "pymatching/sparse_blossom/structured/flooder_matcher_interop/region_edge.h"
#include "pymatching/sparse_blossom/structured/tracker/queued_event_tracker.h"

namespace pm::structured {

class AltTreeNode;

struct GraphFillRegion {
    /// If this region has merged with others into a blossom, this is that blossom.
    RegionId blossom_parent;
    /// The topmost fill region that contains this region. This field must be kept up to date as
    /// the region structure changes.
    RegionId blossom_parent_top;
    /// If this is a top-level region (not a blossom child), this is the alternating tree that
    /// it is part of. Note that it may be a degenerate alternating tree with just a single
    /// graph fill region (this one).
    pm::structured::AltTreeNode* alt_tree_node;
    /// How much this region has grown since its creation (and whether it is currently growing).
    /// For graph fill regions starting from detection events, this is just the actual radius
    /// of the region. For graph fill regions starting from blossom-creation events, this is how
    /// much the blossom has grown since it was created (it's the *extra* radius, not the total
    /// radius starting from the detection events).
    VaryingCT& radius_value() {
        return pool->radii.data()[region_id];
    }
    const VaryingCT& radius_value() const {
        return pool->radii.data()[region_id];
    }
    /// Event tracker for shrink events. Handles ensuring at least, and ideally exactly, one event
    /// to look at the region is in the event queue.
    QueuedEventTracker shrink_event_tracker;
    /// If the region is matched, as opposed to growing/shrinking, this says what it is matched to.
    pm::structured::Match match;

    /// If this region is a blossom, these are its child regions along with the cyclic paths
    /// between the children.
    std::vector<pm::structured::RegionEdge> blossom_children;
    /// The set of nodes directly owned by this region. Note that this vector does not include
    /// the nodes indirectly owned by this region that are owned by the blossom children of this
    /// region (or their children or etc).
    RegionShell shell_area;
    const RegionId region_id;
    RegionPool* const pool;
    GraphFillRegion* parent_region() const {
        return pool->get(blossom_parent);
    }
    GraphFillRegion* matched_region() const {
        return pool->get(match.region);
    }

   public:
    void cleanup_shell_area();
    void cleanup_shell_area_deferring_seed_nodes();

    GraphFillRegion() = delete;
    ~GraphFillRegion();
    GraphFillRegion(GraphFillRegion&&) = delete;
    GraphFillRegion(const GraphFillRegion&) = delete;
    bool tree_equal(const pm::structured::GraphFillRegion& other) const;

    void add_match(pm::structured::GraphFillRegion* match, const pm::structured::CompressedEdge& edge);

    template <typename Callable>
    void do_op_for_each_node_in_total_area(const Callable& func);
    template <typename Callable>
    void do_op_for_each_descendant_and_self(const Callable& func);
    void clear_blossom_parent();
    void clear_blossom_parent_ignoring_wrapped_radius();
    void wrap_into_blossom(GraphFillRegion* new_blossom_parent_and_top);

    /// Determines if rhs is an ancestor of, or the same as, lhs.
    /// Caution: The equality used here is reference equality (not tree equality like operator==).
    bool operator<=(const GraphFillRegion& rhs) const;
    /// Determines if rhs is an ancestor of, but not the same as, lhs.
    bool operator<(const GraphFillRegion& rhs) const;
    /// Determines if rhs is a descendent of, or the same as, lhs.
    /// Caution: The equality used here is reference equality (not tree equality like operator==).
    bool operator>=(const GraphFillRegion& rhs) const;
    /// Determines if rhs is a descendent of, but not the same as, lhs.
    bool operator>(const GraphFillRegion& rhs) const;

    bool operator==(const GraphFillRegion& rhs) const;
    bool operator!=(const GraphFillRegion& rhs) const;

   private:
    friend class RegionPool;
    GraphFillRegion(RegionPool& pool, RegionId id) noexcept;
    GraphFillRegion(RegionPool& pool, RegionId id, DetectorNode* node, const Match& match) noexcept
        : blossom_parent(NO_REGION),
          blossom_parent_top(id),
          alt_tree_node(nullptr),
          shrink_event_tracker(),
          match(match),
          shell_area(node),
          region_id(id),
          pool(&pool) {
    }
};

inline GraphFillRegion& RegionPool::construct_reserved_seed(
    RegionId id, VaryingCT radius, DetectorNode* node, const Match& match) noexcept {
    assert(id && id < next_ && (states_[id] == reserved_raw || states_[id] == reserved_neutral || states_[id] == reserved_seed_match));
    seed_node_flags_[id] = 0;
    auto* region = &regions_[id];
    if (states_[id] == reserved_seed_match) {
        region->match = match;
        region->shell_area.replace_unchanged_singleton(node);
    } else if (states_[id] == reserved_neutral) {
        region->match = match;
        region->shell_area.set_singleton_from_neutral(node);
    } else {
        region = new (region) GraphFillRegion(*this, id, node, match);
    }
    radii.data()[id] = radius;
    if (match.edge.loc_from != nullptr && !radius.is_growing() && !radius.is_shrinking()) {
        seed_extraction_[id] = {match.edge.obs_mask, static_cast<weight_int>(radius.y_intercept()), match.region};
        seed_endpoints_[id].pointers = {node, match.edge.loc_to};
        states_[id] = live_seed_match;
    } else {
        states_[id] = live;
    }
    return *region;
}

inline void RegionPool::publish_reserved_matched_seed(
    RegionId id, VaryingCT radius, DetectorNode* node, const Match& match, bool defer_node) noexcept {
    assert(id && id < next_ && (states_[id] == reserved_raw || states_[id] == reserved_neutral || states_[id] == reserved_seed_match));
    assert(node && match.edge.loc_from == node && !radius.is_growing() && !radius.is_shrinking());
    assert((match.region != NO_REGION) == (match.edge.loc_to != nullptr));
    assert(match.region < next_ && match.region != id);
    seed_extraction_[id] = {match.edge.obs_mask, static_cast<weight_int>(radius.y_intercept()), match.region};
    seed_endpoints_[id].pointers = {node, match.edge.loc_to};
    seed_node_flags_[id] = defer_node ? node_deferred : 0;
    radii.data()[id] = radius;
    const auto state = states_[id];
    states_[id] = state == reserved_neutral ? lazy_neutral :
                  state == reserved_seed_match ? lazy_seed_match : lazy_raw;
}

inline void RegionPool::publish_reserved_compact_seed(RegionId id, VaryingCT radius, uint32_t detector,
    uint32_t mate_detector, RegionId mate, obs_int observable) noexcept {
    assert(id && id < next_ && (states_[id] == reserved_raw || states_[id] == reserved_neutral || states_[id] == reserved_seed_match));
    assert(compact_node_base_ && detector < compact_node_count_);
    assert(!radius.is_growing() && !radius.is_shrinking());
    assert(mate < next_ && mate != id && ((mate == NO_REGION) == (mate_detector == UINT32_MAX)));
    assert(mate_detector == UINT32_MAX || mate_detector < compact_node_count_);
    seed_extraction_[id] = {observable, static_cast<weight_int>(radius.y_intercept()), mate};
    seed_endpoints_[id].detectors = {detector, mate_detector};
    seed_node_flags_[id] = node_deferred | compact_endpoints;
    radii.data()[id] = radius;
    const auto state = states_[id];
    states_[id] = state == reserved_neutral ? lazy_neutral :
                  state == reserved_seed_match ? lazy_seed_match : lazy_raw;
}

inline GraphFillRegion& RegionPool::operator[](RegionId id) {
    assert(id && id < next_ && (states_[id] == live || states_[id] == live_seed_match || is_lazy_state(states_[id])));
    if (is_lazy_state(states_[id])) return materialize_seed(id);
    return regions_[id];
}
inline const GraphFillRegion& RegionPool::operator[](RegionId id) const {
    assert(id && id < next_ && (states_[id] == live || states_[id] == live_seed_match || is_lazy_state(states_[id])));
    if (is_lazy_state(states_[id])) return materialize_seed(id);
    return regions_[id];
}
inline GraphFillRegion* RegionPool::get(RegionId id) {
    return id ? &(*this)[id] : nullptr;
}
inline const GraphFillRegion* RegionPool::get(RegionId id) const {
    return id ? &(*this)[id] : nullptr;
}

template <typename Callable>
inline void pm::structured::GraphFillRegion::do_op_for_each_node_in_total_area(const Callable& func) {
    for (size_t i = 0; i < shell_area.size(); i++) {
        func(shell_area[shell_area.size() - i - 1]);
    }
    for (auto& child : blossom_children) {
        (*pool)[child.region].do_op_for_each_node_in_total_area(func);
    }
}

template <typename Callable>
inline void pm::structured::GraphFillRegion::do_op_for_each_descendant_and_self(const Callable& func) {
    func(this);
    for (RegionEdge& child : blossom_children) {
        (*pool)[child.region].do_op_for_each_descendant_and_self(func);
    }
}

}  // namespace pm::structured

#endif  // PYMATCHING_STRUCTURED_GRAPH_FILL_REGION_H
