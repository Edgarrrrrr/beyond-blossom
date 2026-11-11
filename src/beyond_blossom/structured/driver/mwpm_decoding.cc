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

#include "beyond_blossom/structured/driver/mwpm_decoding.h"

#include "beyond_blossom/profile/profile.h"

namespace beyond_blossom::structured {

template <bool Predecode>
static void prepare_timeline(Mwpm& mwpm, bool run_predecoder, bool state_imported, bool packed) {
    if (state_imported && run_predecoder)
        throw std::invalid_argument("Imported state requires a completed predecode phase.");
    if (!state_imported) {
        if (!mwpm.flooder.queue.empty())
            throw std::invalid_argument("!mwpm.flooder.queue.empty()");
        mwpm.flooder.queue.cur_time = 0;
#if BB_STRUCTURED_PROFILE_COUNTERS
        // Imported state already counted its seed pushes before this entry.
        mwpm.flooder.queue.profile = {};
#endif
    }
    if constexpr (Predecode) {
        {
            PredecodePhaseScope<PredecodePhase::Prepare> phase(mwpm.predecoder.stats);
            if (run_predecoder)
                mwpm.predecoder.run(mwpm.single_predecode_grid());
        }
        if (mwpm.predecoder.complete)
            return;
        if (!state_imported) {
            PredecodePhaseScope<PredecodePhase::Handoff> phase(mwpm.predecoder.stats);
            if (packed) mwpm.import_packed_predecode_state();
            else mwpm.import_predecode_state(mwpm.predecoder.seed_view.storage);
        }
    } else {
        if (state_imported)
            throw std::invalid_argument("Imported predecode state requires predecode enabled.");
        PredecodePhaseScope<PredecodePhase::Initialization> initialization_phase(mwpm.predecoder.stats);
        beyond_blossom::profile::ScopedRange create_range(beyond_blossom::profile::RangeId::CreateDetectionEvents);
        for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
            for (const auto& seed : mwpm.predecoder.seed_view.storage.subspan(range.begin, range.count()))
                mwpm.create_detection_event(&mwpm.flooder.graph.node(seed.detector_id));
    }
}

void prepare_timeline_for_mwpm(Mwpm& mwpm, bool run_predecoder, bool state_imported) {
    mwpm.prepare_bound_timeline(run_predecoder, state_imported, false);
}

void prepare_packed_timeline_for_mwpm(Mwpm& mwpm, bool run_predecoder, bool state_imported) {
    mwpm.prepare_bound_timeline(run_predecoder, state_imported, true);
}

template <bool Predecode>
static void process_prepared_timeline_until_completion_impl(Mwpm& mwpm, bool run_predecoder,
                                                       bool state_imported, bool packed = false) {
    auto timeline_range =
        beyond_blossom::profile::counted_range(beyond_blossom::profile::RangeId::Timeline, &beyond_blossom::profile::ShotStats::timeline_runs);
    // A split lifecycle already completed these stages. Keep its diagnostic
    // prefix windows intact when continuing the imported or complete state.
    if constexpr (Predecode) {
        if (run_predecoder || (!state_imported && !mwpm.predecoder.complete))
            prepare_timeline<true>(mwpm, run_predecoder, state_imported, packed);
        if (mwpm.predecoder.complete)
            return;
    } else {
        prepare_timeline<false>(mwpm, run_predecoder, state_imported, packed);
    }
#if BB_STRUCTURED_PROFILE_COUNTERS
    mwpm.predecoder.stats.mwpm_entered = 1;
#endif
    PredecodePhaseScope<PredecodePhase::Mwpm> phase(mwpm.predecoder.stats);

    while (mwpm.active_tree_count() != 0) {
        auto event = mwpm.flooder.run_until_next_mwpm_notification();
        if (event.event_type == NO_EVENT)
            break;
        mwpm.process_event(event);
    }

    // If some alternating tree nodes remain, a perfect matching cannot be found
    if (mwpm.active_tree_count() != 0 || mwpm.node_arena.allocated.size() != mwpm.node_arena.available.size()) {
        mwpm.reset();
        throw std::invalid_argument(
            "No perfect matching could be found. This likely means that the syndrome has odd "
            "parity in the support of a connected component without a boundary.");
    }

    // Every tree has augmented and its regions have been frozen. Remaining
    // tentative events cannot change the matching. Retire node trackers before
    // discarding the buckets: nodes vacated during shrinking are outside the
    // surviving shells, so final extraction does not visit all queued nodes.
    for (const auto& bucket : mwpm.flooder.queue.bit_buckets)
        for (const auto& event : bucket)
            if (event.tentative_event_type == LOOK_AT_NODE)
                event.data_look_at_node->node_event_tracker.clear();
    // Keep cur_time at the last actual event for the completed dual solution.
    mwpm.flooder.queue.clear();
}

void Mwpm::configure_predecode(bool enabled) {
    if (!predecoder.seed_view.empty() || !flooder.queue.empty() || active_tree_count())
        throw std::logic_error("Bind predecode before preparing a shot.");
    predecode_enabled_ = enabled;
    prepare_timeline_entry_ = enabled ? &prepare_timeline<true> : &prepare_timeline<false>;
    process_timeline_entry_ = enabled ? &process_prepared_timeline_until_completion_impl<true>
                                     : &process_prepared_timeline_until_completion_impl<false>;
}

void Mwpm::prepare_bound_timeline(bool run_predecoder, bool state_imported, bool packed) {
    prepare_timeline_entry_(*this, run_predecoder, state_imported, packed);
}

void Mwpm::process_bound_timeline(bool run_predecoder, bool state_imported, bool packed) {
    process_timeline_entry_(*this, run_predecoder, state_imported, packed);
}

void process_prepared_timeline_until_completion(Mwpm& mwpm, bool run_predecoder, bool state_imported) {
    mwpm.process_bound_timeline(run_predecoder, state_imported, false);
}

// Initial seed IDs are contiguous in active-rank order, even with fixed-slot
// holes. Only the compact pool state/payload is read on this branch; no region
// object or matched-region object is resolved, including during retirement.
template <bool defer_seed_nodes, bool output_edges>
static bool extract_unchanged_seed(Mwpm& mwpm, const PredecodeSeed& seed,
                                   RegionId id, beyond_blossom::MatchingResult& result) {
    auto& pool = *mwpm.flooder.regions;
    if (!pool.is_unchanged_seed(id)) return false;
    const auto& cached = pool.seed_extraction(id);
    const bool representative = cached.mate == NO_REGION || id < cached.mate;
    // Resolve the published endpoint from its compact ID or cached pointer.
    // This lookup does not materialize the region or populate node fields.
    auto& node = *pool.seed_node(id);
    if constexpr (output_edges) {
        if (representative) {
            DetectorNode* mate = cached.mate == NO_REGION ? nullptr : pool.seed_node(cached.mate);
            mwpm.flooder.match_edges.push_back({&node, mate, cached.obs_mask});
        }
    } else {
        result.weight += cached.radius;
        if (representative) result.obs_mask ^= cached.obs_mask;
    }
    if (pool.is_seed_node_deferred(id)) {
        // The singleton never entered MWPM, so all node fields are still clean.
        // Only its compact collision ownership was published by import.
        auto* grid = seed.grid ? seed.grid : node.grid_component;
        grid->collision_states[seed.cell] = {0, NO_REGION};
    } else if constexpr (defer_seed_nodes) {
        auto* grid = seed.grid ? seed.grid : node.grid_component;
        node.region_that_arrived = NO_REGION;
        grid->collision_states[seed.cell] = {0, NO_REGION};
    } else {
        node.reset();
    }
    beyond_blossom::profile::record_region_destroyed();
    pool.release_unchanged_seed(id);
    return true;
}

template <bool defer_seed_nodes>
static beyond_blossom::MatchingResult shatter_blossoms_and_extract_obs_mask_and_weight(Mwpm& mwpm) {
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::FinalExtract, &beyond_blossom::profile::ShotStats::final_shatter_extract_runs);

    if (mwpm.predecoder.complete) {
        if (!mwpm.predecoder.result_pending)
            return {};
        mwpm.predecoder.result_pending = false;
        return mwpm.predecoder.result;
    }

    beyond_blossom::MatchingResult res;
    if (mwpm.try_extract_borrowed_seed_result<defer_seed_nodes>(res)) return res;
    for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
    for (size_t slot = range.begin; slot < range.end; ++slot) {
        const auto& seed = mwpm.predecoder.seed_view.storage[slot];
        const auto id = static_cast<RegionId>(1 + range.rank_begin + slot - range.begin);
        if (extract_unchanged_seed<defer_seed_nodes, false>(mwpm, seed, id, res)) continue;
        const auto i = seed.detector_id;
        if (mwpm.flooder.graph.node(i).region_that_arrived) {
            if constexpr (defer_seed_nodes)
                res += mwpm.shatter_blossom_and_extract_matches_deferring_seed_nodes(mwpm.flooder.graph.node(i).top_region());
            else
                res += mwpm.shatter_blossom_and_extract_matches(mwpm.flooder.graph.node(i).top_region());
        }
    }
    if (!mwpm.predecoder.seed_view.empty()) {
        mwpm.flooder.regions->mark_completed_shot();
        if constexpr (defer_seed_nodes)
            mwpm.mark_seed_cleanup_pending();
    }
    return res;
}

beyond_blossom::MatchingResult shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(Mwpm& mwpm) {
    return shatter_blossoms_and_extract_obs_mask_and_weight<false>(mwpm);
}

void shatter_blossoms_for_all_detection_events_and_extract_match_edges(Mwpm& mwpm) {
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::FinalExtractEdges, &beyond_blossom::profile::ShotStats::final_shatter_extract_runs);

    if (mwpm.predecoder.complete) {
        if (mwpm.predecoder.result_pending) {
            const auto seeds = mwpm.predecoder.seed_view.storage;
            for (const auto& valid : mwpm.predecoder.seed_view.active_ranges())
            for (size_t i = valid.begin; i < valid.end; i++) {
                const auto& seed = seeds[i];
                if (seed.mate < i)
                    continue;
                auto* node = &mwpm.flooder.graph.node(seed.detector_id);
                auto* mate = seed.mate == PredecodeSeed::BOUNDARY
                                 ? nullptr
                                 : &mwpm.flooder.graph.node(seeds[seed.mate].detector_id);
                mwpm.flooder.match_edges.push_back({node, mate, node->edge_observables(seed.match_slot)});
            }
            mwpm.predecoder.result_pending = false;
        }
        return;
    }

    beyond_blossom::MatchingResult unused;
    for (const auto& range : mwpm.predecoder.seed_view.active_ranges())
    for (size_t slot = range.begin; slot < range.end; ++slot) {
        const auto& seed = mwpm.predecoder.seed_view.storage[slot];
        const auto id = static_cast<RegionId>(1 + range.rank_begin + slot - range.begin);
        if (extract_unchanged_seed<false, true>(mwpm, seed, id, unused)) continue;
        const auto i = seed.detector_id;
        if (mwpm.flooder.graph.node(i).region_that_arrived)
            mwpm.shatter_blossom_and_extract_match_edges(
                mwpm.flooder.graph.node(i).top_region(), mwpm.flooder.match_edges);
    }
    if (!mwpm.predecoder.seed_view.empty())
        mwpm.flooder.regions->mark_completed_shot();
}

beyond_blossom::MatchingResult decode_prepared_detection_events_for_up_to_64_observables(Mwpm& mwpm, bool run_predecoder, bool state_imported) {
    beyond_blossom::profile::ScopedRange decode_range(beyond_blossom::profile::RangeId::DecodeShot);
    mwpm.process_bound_timeline(run_predecoder, state_imported, run_predecoder);
    PredecodePhaseScope<PredecodePhase::Extract> extract_phase(mwpm.predecoder.stats);
    auto res = shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(mwpm);
    res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
    res.weight += mwpm.flooder.negative_weight_sum;
    return res;
}

beyond_blossom::MatchingResult solve_prepared_detection_events_deferring_seed_nodes(Mwpm& mwpm) {
    beyond_blossom::profile::ScopedRange decode_range(beyond_blossom::profile::RangeId::DecodeShot);
    process_prepared_timeline_until_completion(mwpm, false, !mwpm.predecoder.complete);
    PredecodePhaseScope<PredecodePhase::Extract> extract_phase(mwpm.predecoder.stats);
    auto res = shatter_blossoms_and_extract_obs_mask_and_weight<true>(mwpm);
    res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
    res.weight += mwpm.flooder.negative_weight_sum;
    return res;
}

void decode_prepared_detection_events(
    Mwpm& mwpm, uint8_t* obs_begin_ptr, beyond_blossom::total_weight_int& weight, bool run_predecoder, bool state_imported) {
    beyond_blossom::profile::ScopedRange decode_range(beyond_blossom::profile::RangeId::DecodeShot);
    mwpm.process_bound_timeline(run_predecoder, state_imported,
        run_predecoder && mwpm.flooder.graph.num_observables <= sizeof(beyond_blossom::obs_int) * 8);
    PredecodePhaseScope<PredecodePhase::Extract> extract_phase(mwpm.predecoder.stats);
    const auto num_observables = mwpm.flooder.graph.num_observables;
    if (num_observables > sizeof(beyond_blossom::obs_int) * 8) {
        mwpm.flooder.match_edges.clear();
        shatter_blossoms_for_all_detection_events_and_extract_match_edges(mwpm);
        mwpm.extract_paths_from_match_edges(mwpm.flooder.match_edges, obs_begin_ptr, weight);
        for (auto obs : mwpm.flooder.negative_weight_observables)
            obs_begin_ptr[obs] ^= 1;
        weight += mwpm.flooder.negative_weight_sum;
    } else {
        auto res = shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(mwpm);
        res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
        fill_bit_vector_from_obs_mask(res.obs_mask, obs_begin_ptr, num_observables);
        weight = res.weight + mwpm.flooder.negative_weight_sum;
    }
}

void decode_prepared_detection_events_to_normalized_match_edges(Mwpm& mwpm, bool run_predecoder, bool state_imported) {
    process_prepared_timeline_until_completion(mwpm, run_predecoder, state_imported);
    PredecodePhaseScope<PredecodePhase::Extract> extract_phase(mwpm.predecoder.stats);
    mwpm.flooder.match_edges.clear();
    shatter_blossoms_for_all_detection_events_and_extract_match_edges(mwpm);
}

void decode_prepared_detection_events_to_match_edges(Mwpm& mwpm, bool run_predecoder, bool state_imported) {
    if (mwpm.flooder.negative_weight_sum != 0)
        throw std::invalid_argument(
            "Decoding to matched detection events not supported for graphs containing edges with negative weights.");
    decode_prepared_detection_events_to_normalized_match_edges(mwpm, run_predecoder, state_imported);
}

void flip_edge(const SearchGraphEdge& edge) {
    edge.detector_node->edge_marker(edge.neighbor_index) ^= FLIPPED;
    auto neighbor = edge.detector_node->neighbor(edge.neighbor_index);
    if (neighbor) {
        auto idx_from_neighbor = edge.neighbor_index ^ 1;
        neighbor->edge_marker(idx_from_neighbor) ^= FLIPPED;
    }
}

void decode_prepared_detection_events_to_edges(Mwpm& mwpm, std::vector<int64_t>& edges, bool run_predecoder, bool state_imported) {
    if (mwpm.flooder.graph.num_nodes != mwpm.search_flooder.graph.num_nodes) {
        throw std::invalid_argument(
            "Mwpm object does not contain search flooder, which is required to decode to edges.");
    }
    process_prepared_timeline_until_completion(mwpm, run_predecoder, state_imported);
    PredecodePhaseScope<PredecodePhase::Extract> extract_phase(mwpm.predecoder.stats);
    mwpm.flooder.match_edges.clear();
    shatter_blossoms_for_all_detection_events_and_extract_match_edges(mwpm);
    // Flip edges with negative weights and add to edges vector.
    for (const auto& neg_node_pair : mwpm.search_flooder.graph.negative_weight_edges) {
        auto node1_ptr = &mwpm.search_flooder.graph.node(neg_node_pair.first);
        auto node2_ptr =
            neg_node_pair.second != SIZE_MAX ? &mwpm.search_flooder.graph.node(neg_node_pair.second) : nullptr;
        SearchGraphEdge neg_edge = {node1_ptr, node1_ptr->index_of_neighbor(node2_ptr)};
        flip_edge(neg_edge);
        int64_t node1 = mwpm.search_flooder.graph.node_id(neg_edge.detector_node);
        int64_t node2 = node2_ptr ? int64_t(mwpm.search_flooder.graph.node_id(node2_ptr)) : -1;
        edges.push_back(node1);
        edges.push_back(node2);
    }
    // Flip edges along a shortest path between matched detection events and add to edges vector
    for (const auto& match_edge : mwpm.flooder.match_edges) {
        size_t node_from = mwpm.flooder.graph.node_id(match_edge.loc_from);
        size_t node_to = match_edge.loc_to ? mwpm.flooder.graph.node_id(match_edge.loc_to) : SIZE_MAX;
        mwpm.search_flooder.iter_edges_on_shortest_path_from_middle(node_from, node_to, [&](const SearchGraphEdge& e) {
            flip_edge(e);
            int64_t node1 = mwpm.search_flooder.graph.node_id(e.detector_node);
            auto node2_ptr = e.detector_node->neighbor(e.neighbor_index);
            int64_t node2 = node2_ptr ? int64_t(mwpm.search_flooder.graph.node_id(node2_ptr)) : -1;
            edges.push_back(node1);
            edges.push_back(node2);
        });
    }
    // Remove any edges in the edges vector that are no longer flipped. Also unflip edges.
    for (size_t i = 0; i < edges.size() / 2;) {
        int64_t u = edges[2 * i];
        int64_t v = edges[2 * i + 1];
        auto& u_node = mwpm.search_flooder.graph.node(u);
        size_t idx;
        if (v == -1) {
            idx = u_node.index_of_neighbor(nullptr);
        } else {
            auto v_ptr = &mwpm.search_flooder.graph.node(v);
            idx = u_node.index_of_neighbor(v_ptr);
        }
        if (!(u_node.edge_marker(idx) & FLIPPED)) {
            edges[2 * i] = edges[edges.size() - 2];
            edges[2 * i + 1] = edges[edges.size() - 1];
            edges.resize(edges.size() - 2);
        } else {
            flip_edge({&u_node, idx});
            i++;
        }
    }
}

beyond_blossom::MatchingResult decode_detection_events_for_up_to_64_observables(
    Mwpm& mwpm, const std::vector<uint64_t>& events, bool edge_correlations) {
    if (edge_correlations) {
        std::vector<int64_t> edges;
        decode_detection_events_to_edges(mwpm, events, edges);
        mwpm.flooder.graph.reweight_for_edges(edges);
        mwpm.search_flooder.graph.reweight_for_edges(edges);
    }
    mwpm.predecoder.prepare(mwpm.flooder.graph, events, mwpm.flooder.negative_weight_detection_events);
    auto result = decode_prepared_detection_events_for_up_to_64_observables(mwpm);
    if (edge_correlations) {
        mwpm.flooder.graph.undo_reweights();
        mwpm.search_flooder.graph.undo_reweights();
    }
    return result;
}

void decode_detection_events(
    Mwpm& mwpm,
    const std::vector<uint64_t>& events,
    uint8_t* observables,
    total_weight_int& weight,
    bool edge_correlations) {
    if (edge_correlations) {
        std::vector<int64_t> edges;
        decode_detection_events_to_edges(mwpm, events, edges);
        mwpm.flooder.graph.reweight_for_edges(edges);
        mwpm.search_flooder.graph.reweight_for_edges(edges);
    }
    mwpm.predecoder.prepare(mwpm.flooder.graph, events, mwpm.flooder.negative_weight_detection_events);
    decode_prepared_detection_events(mwpm, observables, weight);
    if (edge_correlations) {
        mwpm.flooder.graph.undo_reweights();
        mwpm.search_flooder.graph.undo_reweights();
    }
}

void decode_detection_events_to_match_edges(Mwpm& mwpm, const std::vector<uint64_t>& events) {
    mwpm.predecoder.prepare(mwpm.flooder.graph, events, mwpm.flooder.negative_weight_detection_events);
    decode_prepared_detection_events_to_match_edges(mwpm);
}

void decode_detection_events_to_edges(Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
    mwpm.predecoder.prepare(mwpm.flooder.graph, events, mwpm.flooder.negative_weight_detection_events);
    decode_prepared_detection_events_to_edges(mwpm, edges);
}

void decode_detection_events_to_edges_with_edge_correlations(
    Mwpm& mwpm, const std::vector<uint64_t>& detection_events, std::vector<int64_t>& edges) {
    decode_detection_events_to_edges(mwpm, detection_events, edges);
    mwpm.flooder.graph.reweight_for_edges(edges);
    mwpm.search_flooder.graph.reweight_for_edges(edges);
    edges.clear();

    decode_detection_events_to_edges(mwpm, detection_events, edges);

    mwpm.flooder.graph.undo_reweights();
    mwpm.search_flooder.graph.undo_reweights();
}

}  // namespace beyond_blossom::structured
