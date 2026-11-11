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

#ifndef BB_STRUCTURED_MWPM_INL
#define BB_STRUCTURED_MWPM_INL

#include <cassert>
#include <stdexcept>
#include "beyond_blossom/profile/profile.h"
#include "beyond_blossom/structured/flooder/graph_fill_region.h"

namespace beyond_blossom::structured {

inline void Mwpm::shatter_descendants_into_matches_and_freeze(AltTreeNode &alt_tree_node) {
    for (auto &child_edge : alt_tree_node.children) {
        shatter_descendants_into_matches_and_freeze(*child_edge.alt_tree_node);
    }
    if (alt_tree_node.inner_region) {
        alt_tree_node.parent = AltTreeEdge();
        alt_tree_node.inner()->add_match(alt_tree_node.outer(), alt_tree_node.inner_to_outer_edge);
        flooder.set_region_frozen(*alt_tree_node.inner());
        flooder.set_region_frozen(*alt_tree_node.outer());
        alt_tree_node.inner()->alt_tree_node = nullptr;
        alt_tree_node.outer()->alt_tree_node = nullptr;
    }
    if (alt_tree_node.outer_region) {
        alt_tree_node.outer()->alt_tree_node = nullptr;
    }
    node_arena.del(&alt_tree_node);
}

inline void Mwpm::handle_tree_hitting_boundary(const RegionHitBoundaryEventData &event) {
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::MatchBoundary, &beyond_blossom::profile::ShotStats::matcher_tree_hit_boundary);
    auto node = region_for(event.region)->alt_tree_node;
    node->become_root();
    // Match descendents, deleting AltTreeNodes and freezing GraphFillRegions
    shatter_descendants_into_matches_and_freeze(*node);

    // Now match the event region to the boundary and freeze
    region_for(event.region)->match = Match{NO_REGION, event.edge};
    flooder.set_region_frozen(*region_for(event.region));
    assert(num_active_trees > 0);
    --num_active_trees;
}

inline void Mwpm::handle_tree_hitting_boundary_match(
    GraphFillRegion *unmatched_region,
    GraphFillRegion *matched_region,
    const CompressedEdge &unmatched_to_matched_edge) {
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::MatchBoundaryPair, &beyond_blossom::profile::ShotStats::matcher_tree_hit_boundary_match);
    auto &alt_tree_node = unmatched_region->alt_tree_node;
    unmatched_region->add_match(matched_region, unmatched_to_matched_edge);
    flooder.set_region_frozen(*unmatched_region);
    alt_tree_node->become_root();
    shatter_descendants_into_matches_and_freeze(*alt_tree_node);
    // The old boundary match supplies the augmenting path's other endpoint.
    assert(num_active_trees > 0);
    --num_active_trees;
}

inline void Mwpm::handle_tree_hitting_other_tree(const RegionHitRegionEventData &event) {
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::MatchOtherTree, &beyond_blossom::profile::ShotStats::matcher_tree_hit_other_tree);
    auto alt_node_1 = region_for(event.region1)->alt_tree_node;
    auto alt_node_2 = region_for(event.region2)->alt_tree_node;
    // Tree rotation
    region_for(event.region1)->alt_tree_node->become_root();
    region_for(event.region2)->alt_tree_node->become_root();
    // Match and freeze descendants
    shatter_descendants_into_matches_and_freeze(*alt_node_1);
    shatter_descendants_into_matches_and_freeze(*alt_node_2);
    // Match colliding nodes
    region_for(event.region1)->add_match(region_for(event.region2), event.edge);
    // Freeze colliding regions
    flooder.set_region_frozen(*region_for(event.region1));
    flooder.set_region_frozen(*region_for(event.region2));
    assert(num_active_trees >= 2);
    num_active_trees -= 2;
}

inline AltTreeNode *Mwpm::make_child(
    AltTreeNode &parent,
    GraphFillRegion *child_inner_region,
    GraphFillRegion *child_outer_region,
    const CompressedEdge &child_inner_to_outer_edge,
    const CompressedEdge &child_compressed_edge) {
    beyond_blossom::profile::record_counter(&beyond_blossom::profile::ShotStats::matcher_make_child);
    flooder.regions->invalidate_seed_match(child_inner_region->region_id);
    flooder.regions->invalidate_seed_match(child_outer_region->region_id);
    auto child = node_arena.alloc_unconstructed();
    new (child) AltTreeNode(child_inner_region, child_outer_region, child_inner_to_outer_edge);
    auto child_alt_tree_edge = AltTreeEdge(child, child_compressed_edge);
    parent.add_child(child_alt_tree_edge);
    record_primal_tree_snapshot();
    return child;
}

inline void Mwpm::handle_tree_hitting_match(
    GraphFillRegion *unmatched_region,
    GraphFillRegion *matched_region,
    const CompressedEdge &unmatched_to_matched_edge) {
    auto range =
        beyond_blossom::profile::counted_range(beyond_blossom::profile::RangeId::MatchTree, &beyond_blossom::profile::ShotStats::matcher_tree_hit_match);
    auto alt_tree_node = unmatched_region->alt_tree_node;
    make_child(
        *alt_tree_node,
        matched_region,
        matched_region->matched_region(),
        matched_region->match.edge,
        unmatched_to_matched_edge);
    auto other_match = matched_region->matched_region();
    other_match->match.clear();
    matched_region->match.clear();
    flooder.set_region_shrinking(*matched_region);
    flooder.set_region_growing(*other_match);
}

inline void Mwpm::handle_tree_hitting_self(const RegionHitRegionEventData &event, AltTreeNode *common_ancestor) {
    auto range =
        beyond_blossom::profile::counted_range(beyond_blossom::profile::RangeId::MatchSelf, &beyond_blossom::profile::ShotStats::matcher_tree_hit_self);
    auto alt_node_1 = region_for(event.region1)->alt_tree_node;
    auto alt_node_2 = region_for(event.region2)->alt_tree_node;
    auto prune_result_1 = alt_node_1->prune_upward_path_stopping_before(node_arena, common_ancestor, true);
    auto prune_result_2 = alt_node_2->prune_upward_path_stopping_before(node_arena, common_ancestor, false);

    // Construct blossom region cycle
    auto blossom_cycle = std::move(prune_result_2.pruned_path_region_edges);
    auto p1s = prune_result_1.pruned_path_region_edges.size();
    blossom_cycle.reserve(blossom_cycle.size() + p1s + 1);
    for (size_t i = 0; i < p1s; i++)
        blossom_cycle.push_back(prune_result_1.pruned_path_region_edges[p1s - i - 1]);
    blossom_cycle.push_back(RegionEdge{event.region1, event.edge});
    common_ancestor->outer()->alt_tree_node = nullptr;
    auto blossom_region = flooder.create_blossom(blossom_cycle);

    common_ancestor->outer_region = blossom_region->region_id;
    blossom_region->alt_tree_node = common_ancestor;
    common_ancestor->children.reserve(
        common_ancestor->children.size() + prune_result_1.orphan_edges.size() + prune_result_2.orphan_edges.size());
    for (auto &c : prune_result_1.orphan_edges) {
        common_ancestor->add_child(c);
    }
    for (auto &c : prune_result_2.orphan_edges) {
        common_ancestor->add_child(c);
    }
    record_primal_tree_snapshot();
}

inline void Mwpm::handle_blossom_shattering(const BlossomShatterEventData &event) {
    auto range = beyond_blossom::profile::updated_range(beyond_blossom::profile::RangeId::BlossomShatter, [&](beyond_blossom::profile::ShotStats &stats) {
        stats.matcher_blossom_shatter++;
        stats.blossom_shattered_cycle_length_hist.add(region_for(event.blossom_region)->blossom_children.size());
    });
    for (auto &child : region_for(event.blossom_region)->blossom_children) {
        region_for(child.region)->clear_blossom_parent();
    }

    // First find indices of in_parent_region and in_child_region
    // in_parent_region is the blossom cycle region connected to the parent of the blossom inner node.
    // in_child_region is the blossom cycle region connected to the child of the inner node
    auto blossom_cycle = std::move(region_for(event.blossom_region)->blossom_children);
    auto blossom_alt_node = region_for(event.blossom_region)->alt_tree_node;
    size_t bsize = blossom_cycle.size();
    size_t parent_idx = 0;
    size_t child_idx = 0;
    for (size_t i = 0; i < bsize; i++) {
        if (blossom_cycle[i].region == event.in_parent_region) {
            parent_idx = i;
        }
        if (blossom_cycle[i].region == event.in_child_region) {
            child_idx = i;
        }
    }

    // Length of path starting on in_parent and stopping before in_child
    size_t gap = ((child_idx + bsize - parent_idx) % bsize);
    AltTreeNode *current_alt_node;
    size_t evens_start, evens_end;

    current_alt_node = region_for(event.blossom_region)->alt_tree_node->parent.alt_tree_node;
    unstable_erase(current_alt_node->children, [blossom_alt_node](AltTreeEdge x) {
        return x.alt_tree_node == blossom_alt_node;
    });
    auto child_edge = blossom_alt_node->parent.edge.reversed();

    if (gap % 2 == 0) {
        // The path starting after in_child and stopping before in_parent is even length. Regions will
        // be matched along this path
        evens_start = child_idx + 1;
        evens_end = child_idx + bsize - gap;

        // Now insert odd-length path starting on in_parent and ending on in_child into alternating tree
        for (size_t i = parent_idx; i < parent_idx + gap; i += 2) {
            current_alt_node = make_child(
                *current_alt_node,
                region_for(blossom_cycle[i % bsize].region),
                region_for(blossom_cycle[(i + 1) % bsize].region),
                blossom_cycle[i % bsize].edge,
                child_edge);
            child_edge = blossom_cycle[(i + 1) % bsize].edge;
            flooder.set_region_shrinking(*current_alt_node->inner());
            flooder.set_region_growing(*current_alt_node->outer());
        }
    } else {
        // The path starting after in_parent and stopping before in_child is even length. Regions will
        // be matched along this path.
        evens_start = parent_idx + 1;
        evens_end = parent_idx + gap;

        // Now insert odd-length path into alternating tree
        for (size_t i = 0; i < bsize - gap; i += 2) {
            size_t k1 = (parent_idx + bsize - i) % bsize;
            size_t k2 = (parent_idx + bsize - i - 1) % bsize;
            size_t k3 = (parent_idx + bsize - i - 2) % bsize;
            current_alt_node = make_child(
                *current_alt_node,
                region_for(blossom_cycle[k1].region),
                region_for(blossom_cycle[k2].region),
                blossom_cycle[k2].edge.reversed(),
                child_edge);
            child_edge = blossom_cycle[k3].edge.reversed();
            flooder.set_region_shrinking(*current_alt_node->inner());
            flooder.set_region_growing(*current_alt_node->outer());
        }
    }

    for (size_t j = evens_start; j < evens_end; j += 2) {
        size_t k1 = j % bsize;
        size_t k2 = (j + 1) % bsize;
        region_for(blossom_cycle[k1].region)->add_match(region_for(blossom_cycle[k2].region), blossom_cycle[k1].edge);

        // The blossom regions were previously shrinking. Now they are stopped. This can create new
        // events on the nodes, and so the nodes must be reprocessed.
        region_for(blossom_cycle[k1].region)->do_op_for_each_node_in_total_area([this](DetectorNode *n) {
            flooder.reschedule_events_at_detector_node(*n);
        });
        region_for(blossom_cycle[k2].region)->do_op_for_each_node_in_total_area([this](DetectorNode *n) {
            flooder.reschedule_events_at_detector_node(*n);
        });
    }

    blossom_alt_node->inner_region = blossom_cycle[child_idx].region;
    flooder.set_region_shrinking(*blossom_alt_node->inner());
    region_for(blossom_cycle[child_idx].region)->alt_tree_node = blossom_alt_node;
    current_alt_node->add_child(AltTreeEdge(blossom_alt_node, child_edge));
    record_primal_tree_snapshot();

    beyond_blossom::profile::record_region_destroyed();
    flooder.regions->release(event.blossom_region);
}

inline void Mwpm::handle_region_hit_region(const MwpmEvent event) {
    const auto &d = event.region_hit_region_event_data;
    auto alt_node_1 = region_for(d.region1)->alt_tree_node;
    auto alt_node_2 = region_for(d.region2)->alt_tree_node;
    if (alt_node_1 && alt_node_2) {
        auto common_ancestor = alt_node_1->most_recent_common_ancestor(*alt_node_2);
        if (!common_ancestor) {
            handle_tree_hitting_other_tree(d);
        } else {
            handle_tree_hitting_self(d, common_ancestor);
        }
    } else if (alt_node_1) {
        // Region 2 is not in the tree, so must be matched to the boundary or another region
        if (region_for(d.region2)->match.region) {
            handle_tree_hitting_match(region_for(d.region1), region_for(d.region2), d.edge);
        } else {
            handle_tree_hitting_boundary_match(region_for(d.region1), region_for(d.region2), d.edge);
        }
    } else {
        // Region 1 is not in the tree, so must be matched to the boundary or another region
        if (region_for(d.region1)->match.region) {
            handle_tree_hitting_match(region_for(d.region2), region_for(d.region1), d.edge.reversed());
        } else {
            handle_tree_hitting_boundary_match(region_for(d.region2), region_for(d.region1), d.edge.reversed());
        }
    }
}
inline void Mwpm::process_event(const MwpmEvent &event) {
    auto range =
        beyond_blossom::profile::counted_range(beyond_blossom::profile::RangeId::PrimalPhase, &beyond_blossom::profile::ShotStats::primal_phase_runs);
    record_primal_tree_snapshot();
    switch (event.event_type) {
        case REGION_HIT_REGION:
            handle_region_hit_region(event);
            break;
        case REGION_HIT_BOUNDARY:
            handle_tree_hitting_boundary(event.region_hit_boundary_event_data);
            break;
        case BLOSSOM_SHATTER:
            handle_blossom_shattering(event.blossom_shatter_event_data);
            break;
        case NO_EVENT:
            // Do nothing.
            break;
        default:
            throw std::invalid_argument("Unrecognized event type");
    }
    record_primal_tree_snapshot();
}

inline void Mwpm::create_detection_event(DetectorNode *node) {
    auto region = flooder.allocate_region();
    auto alt_tree_node = node_arena.alloc_unconstructed();
    new (alt_tree_node) AltTreeNode(region);
    region->alt_tree_node = alt_tree_node;
    ++num_active_trees;
    flooder.do_region_created_at_empty_detector_node(*region, *node);
    record_primal_tree_snapshot();
}

}  // namespace beyond_blossom::structured
#endif
