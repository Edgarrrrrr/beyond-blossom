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

#include "pymatching/sparse_blossom/structured/matcher/mwpm.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <set>

#include "pymatching/sparse_blossom/profile/profile.h"
#include "pymatching/sparse_blossom/structured/flooder/graph_fill_region.h"
#include "pymatching/sparse_blossom/structured/matcher/alternating_tree.h"

namespace pm::structured {

namespace {

void delete_region(RegionPool &pool, GraphFillRegion *region) {
    pm::profile::record_region_destroyed();
    pool.release(region->region_id);
}

template <typename T>
bool arena_ptr_is_available(const Arena<T> &arena, const T *ptr) {
    return std::find(arena.available.begin(), arena.available.end(), ptr) != arena.available.end();
}

}  // namespace

Mwpm::Mwpm(GraphFlooder flooder) : flooder(std::move(flooder)), predecoder(this->flooder.graph) {
    bind_predecode_import();
    configure_predecode(PM_PREDECODE_ENABLED);
}

Mwpm::Mwpm(GraphFlooder flooder, SearchFlooder search_flooder)
    : flooder(std::move(flooder)), search_flooder(std::move(search_flooder)), predecoder(this->flooder.graph) {
    bind_predecode_import();
    configure_predecode(PM_PREDECODE_ENABLED);
}

Mwpm &Mwpm::operator=(Mwpm &&other) noexcept {
    if (this != &other) {
        this->~Mwpm();
        new (this) Mwpm(std::move(other));
    }
    return *this;
}

Mwpm::Mwpm(Mwpm &&other) noexcept
    : flooder(std::move(other.flooder)),
      node_arena(std::move(other.node_arena)),
      search_flooder(std::move(other.search_flooder)),
      predecoder(std::move(other.predecoder)),
      predecode_enabled_(other.predecode_enabled_),
      prepare_timeline_entry_(other.prepare_timeline_entry_),
      process_timeline_entry_(other.process_timeline_entry_),
      import_entry(std::exchange(other.import_entry, &Mwpm::import_predecode_state_impl<false>)),
      packed_import_entry(std::exchange(other.packed_import_entry, &Mwpm::import_predecode_state_impl<false>)),
      packed_fixed_prepare_entry(std::exchange(other.packed_fixed_prepare_entry, &Mwpm::prepare_packed_fixed_import_impl<false>)),
      captained_import_entries(other.captained_import_entries),
      captained_no_scratch_entries(other.captained_no_scratch_entries),
      packed_captained_import_entries(other.packed_captained_import_entries),
      packed_captained_no_scratch_entries(other.packed_captained_no_scratch_entries),
      direct_import_nodes(std::exchange(other.direct_import_nodes, nullptr)),
      bound_predecode_grid_(std::exchange(other.bound_predecode_grid_, nullptr)),
      serial_import_residual_nodes_(std::move(other.serial_import_residual_nodes_)),
      changed_seed_bits_(std::move(other.changed_seed_bits_)),
      num_active_trees(std::exchange(other.num_active_trees, 0)),
      pending_seed_cleanup(std::exchange(other.pending_seed_cleanup, false)) {
    other.captained_import_entries = {&Mwpm::import_fixed_captained_impl<2, false>,
        &Mwpm::import_fixed_captained_impl<4, false>, &Mwpm::import_fixed_captained_impl<8, false>};
    other.captained_no_scratch_entries = {&Mwpm::import_fixed_captained_no_scratch_impl<2, false>,
        &Mwpm::import_fixed_captained_no_scratch_impl<4, false>, &Mwpm::import_fixed_captained_no_scratch_impl<8, false>};
    other.packed_captained_import_entries = other.captained_import_entries;
    other.packed_captained_no_scratch_entries = other.captained_no_scratch_entries;
}

void Mwpm::shatter_descendants_into_matches_and_freeze(AltTreeNode &alt_tree_node) {
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

void Mwpm::handle_tree_hitting_boundary(const RegionHitBoundaryEventData &event) {
    auto range = pm::profile::counted_range(
        pm::profile::RangeId::MatchBoundary, &pm::profile::ShotStats::matcher_tree_hit_boundary);
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

void Mwpm::handle_tree_hitting_boundary_match(
    GraphFillRegion *unmatched_region,
    GraphFillRegion *matched_region,
    const CompressedEdge &unmatched_to_matched_edge) {
    auto range = pm::profile::counted_range(
        pm::profile::RangeId::MatchBoundaryPair, &pm::profile::ShotStats::matcher_tree_hit_boundary_match);
    auto &alt_tree_node = unmatched_region->alt_tree_node;
    unmatched_region->add_match(matched_region, unmatched_to_matched_edge);
    flooder.set_region_frozen(*unmatched_region);
    alt_tree_node->become_root();
    shatter_descendants_into_matches_and_freeze(*alt_tree_node);
    // The old boundary match supplies the augmenting path's other endpoint.
    assert(num_active_trees > 0);
    --num_active_trees;
}

void Mwpm::handle_tree_hitting_other_tree(const RegionHitRegionEventData &event) {
    auto range = pm::profile::counted_range(
        pm::profile::RangeId::MatchOtherTree, &pm::profile::ShotStats::matcher_tree_hit_other_tree);
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

AltTreeNode *Mwpm::make_child(
    AltTreeNode &parent,
    GraphFillRegion *child_inner_region,
    GraphFillRegion *child_outer_region,
    const CompressedEdge &child_inner_to_outer_edge,
    const CompressedEdge &child_compressed_edge) {
    pm::profile::record_counter(&pm::profile::ShotStats::matcher_make_child);
    flooder.regions->invalidate_seed_match(child_inner_region->region_id);
    flooder.regions->invalidate_seed_match(child_outer_region->region_id);
    auto child = node_arena.alloc_unconstructed();
    new (child) AltTreeNode(child_inner_region, child_outer_region, child_inner_to_outer_edge);
    auto child_alt_tree_edge = AltTreeEdge(child, child_compressed_edge);
    parent.add_child(child_alt_tree_edge);
    record_primal_tree_snapshot();
    return child;
}

void Mwpm::handle_tree_hitting_match(
    GraphFillRegion *unmatched_region,
    GraphFillRegion *matched_region,
    const CompressedEdge &unmatched_to_matched_edge) {
    auto range =
        pm::profile::counted_range(pm::profile::RangeId::MatchTree, &pm::profile::ShotStats::matcher_tree_hit_match);
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

void Mwpm::handle_tree_hitting_self(const RegionHitRegionEventData &event, AltTreeNode *common_ancestor) {
    auto range =
        pm::profile::counted_range(pm::profile::RangeId::MatchSelf, &pm::profile::ShotStats::matcher_tree_hit_self);
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

void Mwpm::handle_blossom_shattering(const BlossomShatterEventData &event) {
    auto range = pm::profile::updated_range(pm::profile::RangeId::BlossomShatter, [&](pm::profile::ShotStats &stats) {
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

    delete_region(*flooder.regions, region_for(event.blossom_region));
}

void Mwpm::handle_region_hit_region(const MwpmEvent event) {
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
void Mwpm::process_event(const MwpmEvent &event) {
    auto range =
        pm::profile::counted_range(pm::profile::RangeId::PrimalPhase, &pm::profile::ShotStats::primal_phase_runs);
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

GraphFillRegion *Mwpm::pair_and_shatter_subblossoms_and_extract_matches(GraphFillRegion *region, MatchingResult &res) {
    return pair_and_shatter_subblossoms_and_extract_matches_impl<false>(region, res);
}

template <bool defer_seed_nodes>
GraphFillRegion *Mwpm::pair_and_shatter_subblossoms_and_extract_matches_impl(GraphFillRegion *region, MatchingResult &res) {
    auto range = pm::profile::updated_range(pm::profile::RangeId::BlossomFinalPair, [&](pm::profile::ShotStats &stats) {
        stats.matcher_final_blossom_shatter++;
        stats.final_blossom_cycle_length_hist.add(region->blossom_children.size());
    });
    for (auto &r : region->blossom_children) {
        region_for(r.region)->clear_blossom_parent_ignoring_wrapped_radius();
    }
    auto subblossom = region->match.edge.loc_from->top_region();
    subblossom->match = region->match;
    if (subblossom->match.region)
        subblossom->matched_region()->match.region = subblossom->region_id;
    res.weight += region->radius_value().y_intercept();
    auto iter = std::find_if(
        region->blossom_children.begin(), region->blossom_children.end(), [&subblossom](const RegionEdge &e) {
            return e.region == subblossom->region_id;
        });
    size_t index = std::distance(region->blossom_children.begin(), iter);
    size_t num_children = region->blossom_children.size();
    for (size_t i = 0; i < num_children - 1; i += 2) {
        auto &re1 = region->blossom_children[(index + i + 1) % num_children];
        auto &re2 = region->blossom_children[(index + i + 2) % num_children];
        region_for(re1.region)->add_match(region_for(re2.region), re1.edge);
        res += shatter_blossom_and_extract_matches_impl<defer_seed_nodes>(region_for(re1.region));
    }
    delete_region(*flooder.regions, region);
    return subblossom;
}

MatchingResult Mwpm::shatter_blossom_and_extract_matches(GraphFillRegion *region) {
    return shatter_blossom_and_extract_matches_impl<false>(region);
}

MatchingResult Mwpm::shatter_blossom_and_extract_matches_deferring_seed_nodes(GraphFillRegion *region) {
    assert(predecoder.has_live_cell_state());
    return shatter_blossom_and_extract_matches_impl<true>(region);
}

template <bool defer_seed_nodes>
MatchingResult Mwpm::shatter_blossom_and_extract_matches_impl(GraphFillRegion *region) {
    if constexpr (defer_seed_nodes)
        region->cleanup_shell_area_deferring_seed_nodes();
    else
        region->cleanup_shell_area();

    // First handle base cases (no subblossoms)
    if (region->match.region) {
        if constexpr (defer_seed_nodes)
            region->matched_region()->cleanup_shell_area_deferring_seed_nodes();
        else
            region->matched_region()->cleanup_shell_area();
        if (region->blossom_children.empty() && region->matched_region()->blossom_children.empty()) {
            // Neither region nor matched region have blossom children
            // No shattering required, so just return MatchingResult from this match.
            MatchingResult res = {
                region->match.edge.obs_mask,
                region->radius_value().y_intercept() + region->matched_region()->radius_value().y_intercept()};
            delete_region(*flooder.regions, region->matched_region());
            delete_region(*flooder.regions, region);
            return res;
        }
    } else if (region->blossom_children.empty()) {
        // Region with no blossom children matched to boundary
        // No shattering required, so just return MatchingResult from this match.
        MatchingResult res = {region->match.edge.obs_mask, region->radius_value().y_intercept()};
        delete_region(*flooder.regions, region);
        return res;
    }

    // Pair up and shatter subblossoms into matches
    MatchingResult res{0, 0};
    if (!region->blossom_children.empty())
        region = pair_and_shatter_subblossoms_and_extract_matches_impl<defer_seed_nodes>(region, res);
    if (region->match.region && !region->matched_region()->blossom_children.empty())
        pair_and_shatter_subblossoms_and_extract_matches_impl<defer_seed_nodes>(region->matched_region(), res);
    res += shatter_blossom_and_extract_matches_impl<defer_seed_nodes>(region);
    return res;
}

GraphFillRegion *Mwpm::pair_and_shatter_subblossoms_and_extract_match_edges(
    GraphFillRegion *region, std::vector<CompressedEdge> &match_edges) {
    auto range =
        pm::profile::updated_range(pm::profile::RangeId::BlossomFinalPairEdges, [&](pm::profile::ShotStats &stats) {
            stats.matcher_final_blossom_shatter++;
            stats.final_blossom_cycle_length_hist.add(region->blossom_children.size());
        });
    for (auto &r : region->blossom_children) {
        region_for(r.region)->clear_blossom_parent_ignoring_wrapped_radius();
    }
    auto subblossom = region->match.edge.loc_from->top_region();
    subblossom->match = region->match;
    if (subblossom->match.region)
        subblossom->matched_region()->match.region = subblossom->region_id;
    auto iter = std::find_if(
        region->blossom_children.begin(), region->blossom_children.end(), [&subblossom](const RegionEdge &e) {
            return e.region == subblossom->region_id;
        });
    size_t index = std::distance(region->blossom_children.begin(), iter);
    size_t num_children = region->blossom_children.size();
    for (size_t i = 0; i < num_children - 1; i += 2) {
        auto &re1 = region->blossom_children[(index + i + 1) % num_children];
        auto &re2 = region->blossom_children[(index + i + 2) % num_children];
        region_for(re1.region)->add_match(region_for(re2.region), re1.edge);
        shatter_blossom_and_extract_match_edges(region_for(re1.region), match_edges);
    }
    delete_region(*flooder.regions, region);
    return subblossom;
}

void Mwpm::shatter_blossom_and_extract_match_edges(GraphFillRegion *region, std::vector<CompressedEdge> &match_edges) {
    region->cleanup_shell_area();

    // First handle base cases (no subblossoms)
    if (region->match.region) {
        region->matched_region()->cleanup_shell_area();
        if (region->blossom_children.empty() && region->matched_region()->blossom_children.empty()) {
            // Neither region nor matched region have blossom children
            // No shattering required, so just return MatchingResult from this match.
            match_edges.push_back(region->match.edge);
            delete_region(*flooder.regions, region->matched_region());
            delete_region(*flooder.regions, region);
            return;
        }
    } else if (region->blossom_children.empty()) {
        // Region with no blossom children matched to boundary
        // No shattering required, so just return MatchingResult from this match.
        match_edges.push_back(region->match.edge);
        delete_region(*flooder.regions, region);
        return;
    }

    // Pair up and shatter subblossoms into matches
    if (!region->blossom_children.empty())
        region = pair_and_shatter_subblossoms_and_extract_match_edges(region, match_edges);
    if (region->match.region && !region->matched_region()->blossom_children.empty())
        pair_and_shatter_subblossoms_and_extract_match_edges(region->matched_region(), match_edges);
    shatter_blossom_and_extract_match_edges(region, match_edges);
    return;
}

void Mwpm::create_detection_event(DetectorNode *node) {
    auto region = flooder.allocate_region();
    auto alt_tree_node = node_arena.alloc_unconstructed();
    new (alt_tree_node) AltTreeNode(region);
    region->alt_tree_node = alt_tree_node;
    ++num_active_trees;
    flooder.do_region_created_at_empty_detector_node(*region, *node);
    record_primal_tree_snapshot();
}

RegionId Mwpm::prepare_predecode_import(std::span<const PredecodeSeed> seeds) {
    return prepare_predecode_import(seeds.size());
}

RegionId Mwpm::prepare_predecode_import(size_t count) {
    PredecodePhaseScope<PredecodePhase::ImportReserve> reserve_phase(predecoder.stats);
    assert(flooder.queue.empty());
    assert(node_arena.allocated.size() == node_arena.available.size());
    assert(num_active_trees == 0);
    assert(flooder.regions->empty());
    flooder.queue.cur_time = 0;
    return flooder.regions->reserve_contiguous_ids(count);
}

template <bool Prepare, bool Captained, bool DirectNodes, bool Serial, bool Borrow, typename MateRank>
void Mwpm::import_predecode_slots(std::span<const PredecodeSeed> seeds, RegionId first,
    size_t begin, size_t end, size_t rank_begin, const MateRank& mate_rank, PredecodeImportScratch* scratch) {
    assert(begin <= end && end <= seeds.size());
    // Parallel callers give each worker a disjoint seed range and join every
    // worker before creating roots, scheduling events, or exception recovery.
    // The ordinary parallel modes write no arena, free-list, queue, or shared
    // profiling state. Captained is exclusively called by the component owner;
    // it additionally creates roots while each region is still locally hot.
    static_assert(!Serial || Captained);
    static_assert(!DirectNodes || Captained);
    static_assert(!Borrow || DirectNodes);
    const auto node_for = [&](uint32_t detector) -> DetectorNode& {
        if constexpr (DirectNodes) return direct_import_nodes[detector];
        else return flooder.graph.node(detector);
    };
    for (size_t i = begin; i < end; ++i) {
        const auto &seed = seeds[i];
        assert(seed.detector_id < flooder.graph.num_nodes);
        auto &node = node_for(seed.detector_id);
        assert(node.grid_cell == seed.cell);
        // Prepared seeds already carry the immutable grid/cell address. Keep
        // it across region construction instead of loading the colder node's
        // metadata again for Match and collision-state publication. Low-level
        // callers may supply legacy seeds without a grid pointer.
        auto* const grid = seed.grid ? seed.grid : node.grid_component;
        const size_t cell = seed.cell;
        assert(grid == node.grid_component);
        assert(node.region_that_arrived == NO_REGION);
        assert(!node.node_event_tracker.has_queued_time);
        assert((seed.radius & 1) == 0);
        Match match{NO_REGION, {nullptr, nullptr, 0}};
        if (seed.mate == PredecodeSeed::BOUNDARY) {
            assert(node.has_edge(seed.match_slot));
            assert(node.neighbor(seed.match_slot) == nullptr);
            assert(seed.radius == node.edge_weight(seed.match_slot));
            if constexpr (!DirectNodes)
                match = Match{NO_REGION, {&node, nullptr, grid->observables[cell][seed.match_slot]}};
        } else if (seed.mate != PredecodeSeed::UNMATCHED) {
            assert(seed.mate < seeds.size() && seed.mate != i);
            const auto &mate_seed = seeds[seed.mate];
            auto &mate_node = node_for(mate_seed.detector_id);
            assert(mate_seed.mate == i);
            assert(node.has_edge(seed.match_slot));
            assert(node.neighbor(seed.match_slot) == &mate_node);
            assert(cumulative_time_int(seed.radius) + mate_seed.radius == node.edge_weight(seed.match_slot));
            // Both endpoints write their own match. The mate's object need not
            // be constructed yet; its stable node address and reserved ID suffice.
            if constexpr (!DirectNodes)
                match = Match{first + mate_rank(seed.mate), {&node, &mate_node, grid->observables[cell][seed.match_slot]}};
        }
        const auto radius = VaryingCT::from_base_and_growth(
            static_cast<cumulative_time_int>(seed.radius), seed.mate == PredecodeSeed::UNMATCHED ? +1 : 0);
        // Predecode never expands a seed's singleton shell. Matched seeds can
        // therefore publish only their compact radius/match description: the
        // full region is needed only if MWPM subsequently touches this match.
        // Unmatched seeds immediately need a region for their alternating root.
        const RegionId id = first + static_cast<RegionId>(rank_begin + i - begin);
        GraphFillRegion* region = nullptr;
        if (seed.mate == PredecodeSeed::UNMATCHED) {
            region = &flooder.regions->construct_reserved_seed(id, radius, &node, match);
            if constexpr (Borrow) flooder.regions->record_initial_root(id);
        } else if constexpr (Borrow) {
            flooder.regions->publish_reserved_borrowed_seed(id, radius);
        } else if constexpr (DirectNodes) {
            const auto mate_id = seed.mate == PredecodeSeed::BOUNDARY ? NO_REGION : first + mate_rank(seed.mate);
            const auto mate_detector = seed.mate == PredecodeSeed::BOUNDARY ? UINT32_MAX : seeds[seed.mate].detector_id;
            flooder.regions->publish_reserved_compact_seed(id, radius, seed.detector_id, mate_detector,
                mate_id, grid->observables[cell][seed.match_slot]);
        } else {
            // Eager node publication for legacy range/fixed callers is unchanged.
            flooder.regions->publish_reserved_matched_seed(id, radius, &node, match, Captained);
        }
        // A frozen predecode singleton has never reached a neighbor. Captained
        // import can keep its node clean until the first collision/query;
        // matching kernels need only the collision ID and independent radius.
        if (!Captained || seed.mate == PredecodeSeed::UNMATCHED) {
            node.reached_from_source = &node;
            node.radius_of_arrival = 0;
            node.region_that_arrived = id;
        }
        grid->collision_states[cell] = {0, id};
        if constexpr (Prepare || Captained) {
            if (seed.mate == PredecodeSeed::UNMATCHED) {
                if constexpr (Prepare) {
                    // Capacity is reserved before any worker starts. Overlapped
                    // event helpers own different scratch from the captain.
                    assert(scratch->roots.size() < scratch->roots.capacity());
                    scratch->roots.push_back({&node, id});
                }
                if constexpr (Captained) {
                    // The root constructor only touches this region; no mate
                    // needs to be constructed yet. Preserve serial LIFO arena
                    // allocation and root order, with ordinary reset recovery
                    // owning every root immediately after construction.
                    auto* root = node_arena.alloc_unconstructed();
                    new (root) AltTreeNode(region);
                    ++num_active_trees;
                    if constexpr (Serial) serial_import_residual_nodes_.push_back(&node);
                }
            }
#if PM_STRUCTURED_PROFILE_COUNTERS
            else if constexpr (Prepare) {
                if (seed.mate == PredecodeSeed::BOUNDARY) {
                    ++scratch->boundary_count;
                } else if (i < seed.mate) {
                    ++scratch->pair_count;
                }
            }
#endif
        }
        if constexpr (Serial) {
#if PM_STRUCTURED_PROFILE_COUNTERS
            ++predecoder.stats.import_regions;
            if (seed.mate == PredecodeSeed::UNMATCHED) ++predecoder.stats.import_roots;
            else if (seed.mate == PredecodeSeed::BOUNDARY) ++predecoder.stats.import_boundaries;
            else if (i < seed.mate) ++predecoder.stats.import_pairs;
#endif
            pm::profile::record_counter(&pm::profile::ShotStats::detection_regions_created);
            pm::profile::record_region_created();
            pm::profile::record_region_shell_area(1);
        }
    }
}

void Mwpm::import_predecode_range(std::span<const PredecodeSeed> seeds, RegionId first, size_t begin, size_t end) {
    import_predecode_slots(seeds, first, begin, end, begin, [](uint32_t slot) { return slot; });
}

template <size_t Partitions>
void Mwpm::import_fixed_predecode_range(const SeedView& seeds, RegionId first, size_t begin, size_t end) {
    seeds.for_rank_range(begin, end, [&](size_t slot_begin, size_t slot_end, size_t rank) {
        import_predecode_slots(seeds.storage, first, slot_begin, slot_end, rank,
            [&](uint32_t slot) { return seeds.rank_of<Partitions>(slot); });
    });
}
template void Mwpm::import_fixed_predecode_range<2>(const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range<4>(const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range<8>(const SeedView&, RegionId, size_t, size_t);

void Mwpm::import_predecode_range_preparing(std::span<const PredecodeSeed> seeds, RegionId first,
    size_t begin, size_t end, PredecodeImportScratch& scratch) {
    scratch.clear();
    import_predecode_slots<true>(seeds, first, begin, end, begin,
        [](uint32_t slot) { return slot; }, &scratch);
}

template <size_t Partitions>
void Mwpm::import_fixed_predecode_range_preparing(const SeedView& seeds, RegionId first,
    size_t begin, size_t end, PredecodeImportScratch& scratch) {
    scratch.clear();
    seeds.for_rank_range(begin, end, [&](size_t slot_begin, size_t slot_end, size_t rank) {
        import_predecode_slots<true>(seeds.storage, first, slot_begin, slot_end, rank,
            [&](uint32_t slot) { return seeds.rank_of<Partitions>(slot); }, &scratch);
    });
}
template void Mwpm::import_fixed_predecode_range_preparing<2>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_preparing<4>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_preparing<8>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);

template <size_t Partitions, bool Packed>
void Mwpm::import_fixed_predecode_range_captained(const SeedView& seeds, RegionId first,
    size_t begin, size_t end, PredecodeImportScratch& scratch) {
    static_assert(Partitions == 2 || Partitions == 4 || Partitions == 8);
    constexpr size_t slot = Partitions == 2 ? 0 : Partitions == 4 ? 1 : 2;
    if constexpr (Packed) (this->*packed_captained_import_entries[slot])(seeds, first, begin, end, scratch);
    else (this->*captained_import_entries[slot])(seeds, first, begin, end, scratch);
}

template <size_t Partitions, bool DirectNodes, bool Borrow>
void Mwpm::import_fixed_captained_impl(const SeedView& seeds, RegionId first,
    size_t begin, size_t end, PredecodeImportScratch& scratch) {
    scratch.clear();
    seeds.for_rank_range(begin, end, [&](size_t slot_begin, size_t slot_end, size_t rank) {
        import_predecode_slots<true, true, DirectNodes, false, Borrow>(seeds.storage, first, slot_begin, slot_end, rank,
            [&](uint32_t slot) { return seeds.rank_of<Partitions>(slot); }, &scratch);
    });
}
template void Mwpm::import_fixed_predecode_range_captained<2>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_captained<4>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_captained<8>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_captained<2, true>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_captained<4, true>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
template void Mwpm::import_fixed_predecode_range_captained<8, true>(
    const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);

template <size_t Partitions, bool Packed>
void Mwpm::import_fixed_predecode_range_captained_no_scratch(
    const SeedView& seeds, RegionId first, size_t begin, size_t end) {
    static_assert(Partitions == 2 || Partitions == 4 || Partitions == 8);
    constexpr size_t slot = Partitions == 2 ? 0 : Partitions == 4 ? 1 : 2;
    if constexpr (Packed) (this->*packed_captained_no_scratch_entries[slot])(seeds, first, begin, end);
    else (this->*captained_no_scratch_entries[slot])(seeds, first, begin, end);
}

template <size_t Partitions, bool DirectNodes, bool Borrow>
void Mwpm::import_fixed_captained_no_scratch_impl(
    const SeedView& seeds, RegionId first, size_t begin, size_t end) {
    seeds.for_rank_range(begin, end, [&](size_t slot_begin, size_t slot_end, size_t rank) {
        import_predecode_slots<false, true, DirectNodes, false, Borrow>(seeds.storage, first, slot_begin, slot_end, rank,
            [&](uint32_t slot) { return seeds.rank_of<Partitions>(slot); });
    });
}
template void Mwpm::import_fixed_predecode_range_captained_no_scratch<2>(
    const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range_captained_no_scratch<4>(
    const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range_captained_no_scratch<8>(
    const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range_captained_no_scratch<2, true>(
    const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range_captained_no_scratch<4, true>(
    const SeedView&, RegionId, size_t, size_t);
template void Mwpm::import_fixed_predecode_range_captained_no_scratch<8, true>(
    const SeedView&, RegionId, size_t, size_t);

void Mwpm::prepare_fixed_predecode_events_from_seeds(const SeedView& seeds, RegionId first,
    size_t begin, size_t end, PredecodeImportScratch& scratch) {
    scratch.clear();
    // Import initially gives every seed its own singleton region. An unmatched
    // source grows, a matched neighbor is frozen, and an inactive/boundary cell
    // has zero radius. All wrapped radii are zero and no region is shrinking.
    // This is precisely the growing-source matching kernel at queue time zero,
    // using the predecode representation instead of freshly written MWPM data.
    seeds.for_rank_range(begin, end, [&](size_t slot_begin, size_t slot_end, size_t rank) {
        for (size_t i = slot_begin; i < slot_end; ++i) {
            const auto& seed = seeds.storage[i];
            if (seed.mate != PredecodeSeed::UNMATCHED) {
#if PM_STRUCTURED_PROFILE_COUNTERS
                if (seed.mate == PredecodeSeed::BOUNDARY) ++scratch.boundary_count;
                else if (i < seed.mate) ++scratch.pair_count;
#endif
                continue;
            }
            const auto& grid = *seed.grid;
            uint16_t remaining = grid.present[seed.cell];
#if PYMATCHING_ENABLE_PROFILE || PM_STRUCTURED_PROFILE_TIMERS
            scratch.neighbor_scans += std::popcount(remaining);
#endif
            cumulative_time_int best = GRID_NO_TIME;
            bool has_event = false;
            while (remaining) {
                const unsigned direction = std::countr_zero(remaining);
                remaining &= remaining - 1;
                const auto other_cell = ptrdiff_t(seed.cell) + grid.predecode.offsets[direction];
                assert(other_cell >= 0 && size_t(other_cell) < grid.predecode.active.size());
                const auto active = grid.predecode.active[other_cell];
                // Nonzero slots are stable physical seed slots, including holes
                // between fixed bucket prefixes; never treat them as dense rank.
                const bool growing = active &&
                    seeds.storage[active - 1].mate == PredecodeSeed::UNMATCHED;
                cumulative_time_int time = cumulative_time_int(grid.weights[seed.cell][direction])
                    - cumulative_time_int(seed.radius) - grid.predecode.radii[other_cell];
                time >>= unsigned(growing);
                if (time < best) {
                    best = time;
                    has_event = true;
                }
            }
            assert(scratch.roots.size() < scratch.roots.capacity());
            // The immutable pointer table is safe during node construction. Do
            // not dereference this pointer until the captain has joined helpers.
            auto* node = flooder.graph.grid->detector_nodes[seed.detector_id];
            scratch.roots.push_back({node, first + RegionId(rank + i - slot_begin), best, has_event});
        }
    });
}

std::span<AltTreeNode*> Mwpm::reserve_predecode_import_roots(size_t count) {
    assert(num_active_trees == 0);
    return node_arena.borrow_available(count);
}

void Mwpm::construct_predecode_import_roots(
    const PredecodeImportScratch& scratch, std::span<AltTreeNode*> storage) noexcept {
    assert(storage.size() == scratch.roots.size());
    // AltTreeNode(region) initializes scalars, an empty std::vector and the
    // region's own alt_tree_node pointer; it performs no allocation. Finish this
    // entire nonthrowing loop before computing events, so joined recovery can
    // commit all borrowed arena slots even if subsequent event preparation fails.
    for (size_t i = 0; i < storage.size(); ++i)
        new (storage[i]) AltTreeNode(region_for(scratch.roots[i].region));
}

void Mwpm::prepare_predecode_import_events(PredecodeImportScratch& scratch) {
    scratch.neighbor_scans += prepare_predecode_import_events(std::span<PredecodeImportRoot>(scratch.roots));
}

uint64_t Mwpm::prepare_predecode_import_events(std::span<PredecodeImportRoot> roots) {
    // All region/node importers have joined. Root construction only modifies
    // alt_tree_node pointers, which matching_next never reads, so another lane
    // can still be constructing its independent roots while this scans edges.
    uint64_t neighbor_scans = 0;
    for (auto& root : roots) {
        const auto next = flooder.prepare_initial_event_at_detector_node(*root.node);
        root.has_event = next.first != SIZE_MAX;
        root.event_time = next.second;
#if PYMATCHING_ENABLE_PROFILE || PM_STRUCTURED_PROFILE_TIMERS
        neighbor_scans += std::popcount(
            root.node->grid_component->present[root.node->grid_cell]);
#endif
    }
    return neighbor_scans;
}

void Mwpm::commit_predecode_import_roots(size_t count) noexcept {
    node_arena.commit_borrowed_available(count);
    num_active_trees += count;
}

void Mwpm::publish_predecode_import_events(std::span<const PredecodeImportScratch> lanes, size_t total_seeds) {
    // The coordinator alone mutates queue/profile state. Lanes and their entries
    // are in original active-rank order, preserving serial import's tie order.
    pm::profile::record_counter(&pm::profile::ShotStats::detection_regions_created, total_seeds);
    pm::profile::update_stats([&](pm::profile::ShotStats &stats) {
        for (size_t i = 0; i < total_seeds; ++i) {
            stats.record_region_created();
            stats.record_region_shell_area(1);
        }
    });
#if PM_STRUCTURED_PROFILE_COUNTERS
    predecoder.stats.import_regions += total_seeds;
#endif
    for (const auto& lane : lanes) {
        pm::profile::record_counter(&pm::profile::ShotStats::neighbor_scans, lane.neighbor_scans);
        pm::profile::record_counter(&pm::profile::ShotStats::node_reschedules, lane.roots.size());
#if PM_STRUCTURED_PROFILE_COUNTERS
        predecoder.stats.import_roots += lane.roots.size();
        predecoder.stats.import_scheduled += lane.roots.size();
        predecoder.stats.import_boundaries += lane.boundary_count;
        predecoder.stats.import_pairs += lane.pair_count;
#endif
        for (const auto& root : lane.roots) {
            if (root.has_event)
                root.node->node_event_tracker.set_desired_event(
                    {root.node, cyclic_time_int{root.event_time}}, flooder.queue);
            else
                root.node->node_event_tracker.set_no_desired_event();
        }
    }
    record_primal_tree_snapshot();
}

void Mwpm::finish_predecode_import(std::span<const PredecodeSeed> seeds, RegionId first) {
    {
        // This legacy timer now covers the fused root/event finish. Every
        // region and radius is already imported; computing an initial event
        // does not inspect alt-tree pointers or consume the queue. Root arena
        // allocation and queue insertion both retain the original seed order.
        PredecodePhaseScope<PredecodePhase::HandoffMatches> roots_phase(predecoder.stats);
        pm::profile::record_counter(&pm::profile::ShotStats::detection_regions_created, seeds.size());
        // Update profile state on the coordinator, after the worker join.
        pm::profile::update_stats([&](pm::profile::ShotStats &stats) {
            for (size_t i = 0; i < seeds.size(); ++i) {
                stats.record_region_created();
                stats.record_region_shell_area(1);
            }
        });
#if PM_STRUCTURED_PROFILE_COUNTERS
        predecoder.stats.import_regions += seeds.size();
#endif
        for (size_t i = 0; i < seeds.size(); ++i) {
            const auto &seed = seeds[i];
            if (seed.mate == PredecodeSeed::UNMATCHED) {
                auto *region = region_for(first + static_cast<RegionId>(i));
#if PM_STRUCTURED_PROFILE_COUNTERS
                ++predecoder.stats.import_roots;
#endif
                auto *root = node_arena.alloc_unconstructed();
                new (root) AltTreeNode(region);
                ++num_active_trees;
#if PM_STRUCTURED_PROFILE_COUNTERS
                ++predecoder.stats.import_scheduled;
#endif
                flooder.reschedule_events_at_detector_node(flooder.graph.node(seed.detector_id));
            }
#if PM_STRUCTURED_PROFILE_COUNTERS
            else if (seed.mate == PredecodeSeed::BOUNDARY) {
                ++predecoder.stats.import_boundaries;
            } else if (i < seed.mate) {
                ++predecoder.stats.import_pairs;
            }
#endif
        }
    }
    record_primal_tree_snapshot();
}

void Mwpm::finish_fixed_predecode_import(const SeedView& seeds, RegionId first) {
    {
        // As in the dense finish: all importers have joined before this fused
        // root/event pass. Preserve active-rank order across fixed-slot holes.
        // HandoffMatches includes scheduling; no per-seed clocks are added.
        PredecodePhaseScope<PredecodePhase::HandoffMatches> roots_phase(predecoder.stats);
        pm::profile::record_counter(&pm::profile::ShotStats::detection_regions_created, seeds.active_count());
        // Update profile state on the coordinator, after the worker join.
        pm::profile::update_stats([&](pm::profile::ShotStats &stats) {
            for (size_t i = 0; i < seeds.active_count(); ++i) {
                stats.record_region_created();
                stats.record_region_shell_area(1);
            }
        });
#if PM_STRUCTURED_PROFILE_COUNTERS
        predecoder.stats.import_regions += seeds.active_count();
#endif
        for (const auto& range : seeds.active_ranges())
        for (size_t i = range.begin; i < range.end; ++i) {
            const auto &seed = seeds.storage[i];
            if (seed.mate == PredecodeSeed::UNMATCHED) {
                auto *region = region_for(first + static_cast<RegionId>(range.rank_begin + i - range.begin));
#if PM_STRUCTURED_PROFILE_COUNTERS
                ++predecoder.stats.import_roots;
#endif
                auto *root = node_arena.alloc_unconstructed();
                new (root) AltTreeNode(region);
                ++num_active_trees;
#if PM_STRUCTURED_PROFILE_COUNTERS
                ++predecoder.stats.import_scheduled;
#endif
                flooder.reschedule_events_at_detector_node(flooder.graph.node(seed.detector_id));
            }
#if PM_STRUCTURED_PROFILE_COUNTERS
            else if (seed.mate == PredecodeSeed::BOUNDARY) {
                ++predecoder.stats.import_boundaries;
            } else if (i < seed.mate) {
                ++predecoder.stats.import_pairs;
            }
#endif
        }
    }
    record_primal_tree_snapshot();
}

void Mwpm::bind_predecode_import() noexcept {
    const auto& graph = flooder.graph;
    if (!graph.grid || graph.grid->components.size() != 1 || !graph.num_nodes || graph.num_nodes > UINT32_MAX)
        return;
    auto& nodes = graph.grid->components.front()->nodes;
    if (nodes.size() != graph.num_nodes)
        return;
    for (size_t id = 0; id < graph.num_nodes; ++id)
        if (&graph.node(id) != &nodes[id])
            return;
    direct_import_nodes = nodes.data();
    bound_predecode_grid_ = graph.grid->components.front().get();
    flooder.regions->bind_compact_seed_nodes(direct_import_nodes, graph.num_nodes);
    import_entry = &Mwpm::import_predecode_state_impl<true>;
    captained_import_entries = {&Mwpm::import_fixed_captained_impl<2, true>,
        &Mwpm::import_fixed_captained_impl<4, true>, &Mwpm::import_fixed_captained_impl<8, true>};
    captained_no_scratch_entries = {&Mwpm::import_fixed_captained_no_scratch_impl<2, true>,
        &Mwpm::import_fixed_captained_no_scratch_impl<4, true>, &Mwpm::import_fixed_captained_no_scratch_impl<8, true>};
    packed_import_entry = import_entry;
    packed_captained_import_entries = captained_import_entries;
    packed_captained_no_scratch_entries = captained_no_scratch_entries;
    if (graph.num_observables <= sizeof(obs_int) * 8) {
        packed_import_entry = &Mwpm::import_predecode_state_impl<true, true>;
        packed_fixed_prepare_entry = &Mwpm::prepare_packed_fixed_import_impl<true>;
        packed_captained_import_entries = {&Mwpm::import_fixed_captained_impl<2, true, true>,
            &Mwpm::import_fixed_captained_impl<4, true, true>, &Mwpm::import_fixed_captained_impl<8, true, true>};
        packed_captained_no_scratch_entries = {&Mwpm::import_fixed_captained_no_scratch_impl<2, true, true>,
            &Mwpm::import_fixed_captained_no_scratch_impl<4, true, true>, &Mwpm::import_fixed_captained_no_scratch_impl<8, true, true>};
    }
}

template <bool DirectNodes, bool Borrow>
void Mwpm::import_predecode_state_impl(std::span<const PredecodeSeed> seeds) {
    RegionId first;
    {
        // This phase includes both object and per-endpoint match initialization.
        PredecodePhaseScope<PredecodePhase::HandoffRegions> regions_phase(predecoder.stats);
        first = prepare_predecode_import(seeds);
        if constexpr (Borrow) {
            assert(first == 1);
            flooder.regions->borrow_seeds(predecoder.seed_view);
        }
        PredecodePhaseScope<PredecodePhase::ImportConstruct> construct_phase(predecoder.stats);
        serial_import_residual_nodes_.clear();
        serial_import_residual_nodes_.reserve(seeds.size());
        import_predecode_slots<false, true, DirectNodes, true, Borrow>(seeds, first, 0, seeds.size(), 0, [](uint32_t slot) { return slot; });
    }
    {
        PredecodePhaseScope<PredecodePhase::HandoffSchedule> schedule_phase(predecoder.stats);
        for (auto *node : serial_import_residual_nodes_) {
#if PM_STRUCTURED_PROFILE_COUNTERS
            ++predecoder.stats.import_scheduled;
#endif
            flooder.reschedule_events_at_detector_node(*node);
        }
    }
    record_primal_tree_snapshot();
}

void Mwpm::import_predecode_state(std::span<const PredecodeSeed> seeds) {
    (this->*import_entry)(seeds);
}

void Mwpm::import_packed_predecode_state() {
    const auto& view = predecoder.seed_view;
    assert(view.active_ranges().size() == 1 && view.storage.size() == view.active_count());
    (this->*packed_import_entry)(view.storage);
}

template <bool DeferSeedNodes>
bool Mwpm::try_extract_borrowed_seed_result(MatchingResult& result) {
    auto& pool = *flooder.regions;
    if (!pool.has_borrowed_seeds()) return false;
    const auto& view = pool.borrowed_seeds();
    result = predecoder.result;
    changed_seed_bits_.assign((view.active_count() + 63) / 64, 0);
    const auto mark = [&](size_t rank) {
        changed_seed_bits_[rank / 64] |= uint64_t{1} << (rank % 64);
    };
    for (const auto id : pool.changed_initial_seeds()) {
        const size_t rank = id - 1;
        const auto& seed = pool.borrowed_seed(id);
        if (seed.mate == PredecodeSeed::UNMATCHED) {
            mark(rank);
            continue;
        }
        // Initial pairs are disjoint. The bitmap records their original
        // identities even after retirement and ordinary reuse of a RegionId.
        if (changed_seed_bits_[rank / 64] & (uint64_t{1} << (rank % 64))) continue;
        mark(rank);
        if (seed.mate != PredecodeSeed::BOUNDARY) mark(view.rank_of(seed.mate));
        const auto* grid = seed.grid ? seed.grid : direct_import_nodes[seed.detector_id].grid_component;
        result.weight -= grid->weights[seed.cell][seed.match_slot];
        result.obs_mask ^= grid->observables[seed.cell][seed.match_slot];
    }
    // All original endpoints of an affected pair are selected. Shattering can
    // materialize its other endpoint without changing this traversal or order.
    for (size_t word = 0; word < changed_seed_bits_.size(); ++word) {
        auto bits = changed_seed_bits_[word];
        while (bits) {
            const auto rank = word * 64 + std::countr_zero(bits);
            bits &= bits - 1;
            auto& node = direct_import_nodes[view.storage[view.slot_of(rank)].detector_id];
            if (node.region_that_arrived) {
                if constexpr (DeferSeedNodes)
                    result += shatter_blossom_and_extract_matches_deferring_seed_nodes(node.top_region());
                else
                    result += shatter_blossom_and_extract_matches(node.top_region());
            }
        }
    }
    for (const auto& range : view.active_ranges())
        for (size_t slot = range.begin; slot < range.end; ++slot) {
            const auto& seed = view.storage[slot];
            auto* grid = seed.grid ? seed.grid : direct_import_nodes[seed.detector_id].grid_component;
            grid->collision_states[seed.cell] = {0, NO_REGION};
            const auto id = static_cast<RegionId>(1 + range.rank_begin + slot - range.begin);
            if (pool.finish_borrowed_seed(id)) pm::profile::record_region_destroyed();
        }
    pool.finish_borrowed_seeds();
    pool.mark_completed_shot();
    if constexpr (DeferSeedNodes) mark_seed_cleanup_pending();
    return true;
}

template bool Mwpm::try_extract_borrowed_seed_result<false>(MatchingResult&);
template bool Mwpm::try_extract_borrowed_seed_result<true>(MatchingResult&);

template <bool Borrow>
RegionId Mwpm::prepare_packed_fixed_import_impl() {
    const auto first = prepare_predecode_import(predecoder.seed_view.active_count());
    if constexpr (Borrow) {
        assert(first == 1);
        flooder.regions->borrow_seeds(predecoder.seed_view);
    }
    return first;
}

RegionId Mwpm::prepare_packed_fixed_predecode_import() {
    return (this->*packed_fixed_prepare_entry)();
}

void Mwpm::verify_invariants() const {
    size_t roots = 0;
    for (const auto *node : node_arena.allocated) {
        if (!arena_ptr_is_available(node_arena, node) && node->parent.alt_tree_node == nullptr)
            ++roots;
    }
    if (roots != num_active_trees)
        throw std::logic_error("Alternating-tree root count does not match the live forest.");
}

void Mwpm::record_primal_tree_snapshot() const {
    pm::profile::update_stats([&](pm::profile::ShotStats &stats) {
        uint64_t active_nodes = 0;
        std::set<const AltTreeNode *> active_node_set;
        std::vector<const AltTreeNode *> roots;
        for (const auto *node : node_arena.allocated) {
            if (arena_ptr_is_available(node_arena, node)) {
                continue;
            }
            active_nodes++;
            active_node_set.insert(node);
            if (node->parent.alt_tree_node == nullptr) {
                roots.push_back(node);
            }
        }
        stats.record_alt_tree_nodes_active(active_nodes);

        bool stable_tree_topology = true;
        std::set<const AltTreeNode *> visited;
        std::vector<uint64_t> tree_sizes;
        for (const auto *root : roots) {
            uint64_t tree_size = 0;
            std::vector<const AltTreeNode *> pending{root};
            while (!pending.empty() && stable_tree_topology) {
                const auto *node = pending.back();
                pending.pop_back();
                if (!active_node_set.contains(node) || !visited.insert(node).second) {
                    stable_tree_topology = false;
                    break;
                }
                tree_size++;
                for (const auto &child_edge : node->children) {
                    const auto *child = child_edge.alt_tree_node;
                    if (child == nullptr || !active_node_set.contains(child) || child->parent.alt_tree_node != node) {
                        stable_tree_topology = false;
                        break;
                    }
                    pending.push_back(child);
                }
            }
            if (!stable_tree_topology) {
                break;
            }
            tree_sizes.push_back(tree_size);
        }
        if (!stable_tree_topology || visited.size() != active_nodes) {
            return;
        }
        for (auto tree_size : tree_sizes) {
            stats.record_alt_tree_nodes_per_tree(tree_size);
        }
    });
}

void Mwpm::extract_paths_from_match_edges(
    const std::vector<CompressedEdge> &match_edges, uint8_t *obs_begin_ptr, total_weight_int &weight) {
    for (auto &edge : match_edges) {
        size_t loc_to_idx = edge.loc_to ? flooder.graph.node_id(edge.loc_to) : SIZE_MAX;
        search_flooder.iter_edges_on_shortest_path_from_middle(
            flooder.graph.node_id(edge.loc_from), loc_to_idx, [&](const SearchGraphEdge &e) {
                auto &obs = e.detector_node->edge_observables(e.neighbor_index);
                for (auto i : obs)
                    *(obs_begin_ptr + i) ^= 1;
                weight += e.detector_node->edge_weight(e.neighbor_index);
            });
    }
}

Mwpm::Mwpm() {
    configure_predecode(PM_PREDECODE_ENABLED);
}

void Mwpm::mark_seed_cleanup_pending() noexcept {
    assert(!pending_seed_cleanup && predecoder.has_live_cell_state());
    assert(!predecoder.complete && !predecoder.seed_view.empty());
    assert(flooder.queue.empty() && flooder.regions->empty());
    pending_seed_cleanup = true;
}

void Mwpm::reset_seed_nodes_range(size_t begin, size_t end) noexcept {
    assert(pending_seed_cleanup && begin <= end && end <= predecoder.seed_view.active_count());
    predecoder.seed_view.for_rank_range(begin, end, [&](size_t lo, size_t hi, size_t rank) {
        for (size_t i = lo; i < hi; ++i) {
            const auto id = static_cast<RegionId>(1 + rank + i - lo);
            if (!flooder.regions->is_seed_node_deferred(id))
                flooder.graph.node(predecoder.seed_view.storage[i].detector_id).reset_after_final_extraction();
        }
    });
}

void Mwpm::clear_pending_seed_nodes() noexcept {
    if (!pending_seed_cleanup)
        return;
    reset_seed_nodes_range(0, predecoder.seed_view.active_count());
    finish_seed_node_cleanup();
}

void Mwpm::reset() {
    serial_import_residual_nodes_.clear();
    num_active_trees = 0;
    pending_seed_cleanup = false;
    predecoder.reset();
    pm::profile::record_counter(&pm::profile::ShotStats::reset_runs);
    for (size_t i = 0; i < flooder.graph.num_nodes; i++)
        flooder.graph.node(i).reset();
    for (size_t i = 0; i < search_flooder.graph.num_nodes; i++)
        search_flooder.graph.node(i).reset();
    flooder.queue.clear();
    node_arena.~StructuredArena();
    new (&node_arena) StructuredArena<AltTreeNode>();
    if (flooder.regions)
        flooder.regions->reset();
    pm::profile::record_alt_tree_nodes_reset();
    pm::profile::record_region_reset();
}

}  // namespace pm::structured
