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

#include "beyond_blossom/driver/mwpm_decoding.h"

#include "beyond_blossom/driver/user_graph.h"
// Exclude diagnostic arguments and helper code before optimization when profiling is disabled.
#if BB_ENABLE_PROFILE
#include "beyond_blossom/profile/profile.h"
#endif

namespace {
// Correlation reweighting changes the problem between passes. Keep both passes
// on the established solver path and restore weights even if a pass throws.
struct CorrelationScope {
    beyond_blossom::Mwpm& mwpm;
    bool enabled;
    bool previous_predecode;
    CorrelationScope(beyond_blossom::Mwpm& mwpm, bool enabled)
        : mwpm(mwpm), enabled(enabled), previous_predecode(mwpm.predecode_enabled) {
        if (enabled)
            mwpm.predecode_enabled = false;
    }
    ~CorrelationScope() {
        if (enabled) {
            mwpm.flooder.graph.undo_reweights();
            mwpm.search_flooder.graph.undo_reweights();
            mwpm.predecode_enabled = previous_predecode;
        }
    }
};
}  // namespace

beyond_blossom::Mwpm beyond_blossom::detector_error_model_to_mwpm(
    const stim::DetectorErrorModel& detector_error_model,
    beyond_blossom::weight_int num_distinct_weights,
    bool ensure_search_flooder_included,
    bool enable_correlations) {
    auto user_graph =
        beyond_blossom::detector_error_model_to_user_graph(detector_error_model, enable_correlations, num_distinct_weights);
    return user_graph.to_mwpm(num_distinct_weights, ensure_search_flooder_included);
}

void process_timeline_until_completion(beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
#if BB_ENABLE_PROFILE
    auto timeline_range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::Timeline, &beyond_blossom::profile::ShotStats::timeline_runs);
#endif
    if (!mwpm.flooder.queue.empty())
        throw std::invalid_argument("!mwpm.flooder.queue.empty()");
    mwpm.flooder.queue.cur_time = 0;
    try {
        auto& graph = mwpm.flooder.graph;
        if (mwpm.predecode_enabled) {
            mwpm.predecoder.prepare(graph, detection_events, mwpm.flooder.negative_weight_detection_events);
            mwpm.predecoder.run();
            if (mwpm.predecoder.complete)
                return;
            mwpm.import_predecode_state();
        } else if (mwpm.flooder.negative_weight_detection_events.empty()) {
#if BB_ENABLE_PROFILE
            beyond_blossom::profile::ScopedRange create_range(beyond_blossom::profile::RangeId::CreateDetectionEvents);
#endif
            for (auto detection : detection_events) {
                if (detection >= graph.nodes.size())
                    throw std::invalid_argument("Detection event index exceeds the number of graph nodes.");
                if (detection < graph.is_user_graph_boundary_node.size() && graph.is_user_graph_boundary_node[detection])
                    continue;
                auto& node = graph.nodes[detection];
                // No flooding has run yet: an owned node is a duplicate seed.
                if (node.region_that_arrived)
                    throw std::invalid_argument("Duplicate detection event in decoder input.");
                mwpm.create_detection_event(&node);
            }
        } else {
#if BB_ENABLE_PROFILE
            beyond_blossom::profile::ScopedRange create_range(beyond_blossom::profile::RangeId::CreateDetectionEvents);
#endif
            for (auto detection : mwpm.flooder.negative_weight_detection_events)
                graph.nodes[detection].radius_of_arrival = 1;
            for (auto detection : detection_events) {
                if (detection >= graph.nodes.size())
                    throw std::invalid_argument("Detection event index exceeds the number of graph nodes.");
                if (detection < graph.is_user_graph_boundary_node.size() && graph.is_user_graph_boundary_node[detection])
                    continue;
                auto& node = graph.nodes[detection];
                if (node.region_that_arrived || node.radius_of_arrival < 0)
                    throw std::invalid_argument("Duplicate detection event in decoder input.");
                if (node.radius_of_arrival == 0) {
                    mwpm.create_detection_event(&node);
                } else {
                    // A cancelled negative seed stays distinguishable from an
                    // unvisited node until all input duplicates are checked.
                    node.radius_of_arrival = -1;
                }
            }
            for (auto detection : mwpm.flooder.negative_weight_detection_events) {
                auto& node = graph.nodes[detection];
                const bool pending = node.radius_of_arrival > 0;
                node.radius_of_arrival = 0;
                if (pending && (detection >= graph.is_user_graph_boundary_node.size() ||
                                !graph.is_user_graph_boundary_node[detection]))
                    mwpm.create_detection_event(&node);
            }
        }
        while (true) {
            auto event = mwpm.flooder.run_until_next_mwpm_notification();
            if (event.event_type == beyond_blossom::NO_EVENT)
                break;
            mwpm.process_event(event);
        }
        if (mwpm.node_arena.allocated.size() != mwpm.node_arena.available.size())
            throw std::invalid_argument(
                "No perfect matching could be found. This likely means that the syndrome has odd "
                "parity in the support of a connected component without a boundary.");
    } catch (...) {
        // Validation stays in the ordinary creation pass. Only failing shots
        // pay for full cleanup of partial roots, queues, and negative markers.
        mwpm.reset();
        throw;
    }
}

beyond_blossom::MatchingResult shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(
    beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
    if (mwpm.predecode_enabled && mwpm.predecoder.complete) {
        if (!mwpm.predecoder.result_pending)
            return {};
        mwpm.predecoder.result_pending = false;
        return {mwpm.predecoder.obs_mask, mwpm.predecoder.weight};
    }
#if BB_ENABLE_PROFILE
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::FinalExtract, &beyond_blossom::profile::ShotStats::final_shatter_extract_runs);
#endif
    beyond_blossom::MatchingResult res;
    for (auto& i : detection_events) {
        if (mwpm.flooder.graph.nodes[i].region_that_arrived)
            res += mwpm.shatter_blossom_and_extract_matches(mwpm.flooder.graph.nodes[i].region_that_arrived_top);
    }
    return res;
}

void shatter_blossoms_for_all_detection_events_and_extract_match_edges(
    beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
    if (mwpm.predecode_enabled && mwpm.predecoder.complete) {
        if (mwpm.predecoder.result_pending) {
            mwpm.predecoder.append_match_edges(mwpm.flooder.match_edges);
            mwpm.predecoder.result_pending = false;
        }
        return;
    }
#if BB_ENABLE_PROFILE
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::FinalExtractEdges, &beyond_blossom::profile::ShotStats::final_shatter_extract_runs);
#endif
    for (auto& i : detection_events) {
        if (mwpm.flooder.graph.nodes[i].region_that_arrived)
            mwpm.shatter_blossom_and_extract_match_edges(
                mwpm.flooder.graph.nodes[i].region_that_arrived_top, mwpm.flooder.match_edges);
    }
}

beyond_blossom::MatchingResult beyond_blossom::decode_detection_events_for_up_to_64_observables(
    beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events, bool edge_correlations) {
    CorrelationScope correlation_scope(mwpm, edge_correlations);
#if BB_ENABLE_PROFILE
    beyond_blossom::profile::ScopedRange decode_range(beyond_blossom::profile::RangeId::DecodeShot);
#endif
    if (edge_correlations) {
        // Edge correlations might also be called 2-pass matching. This is the slowest and
        // highest-accuracy reweighting rule for correlated decoding in which we first decode to edges,
        // and then reweight the associated edges conditioned on the assumption that an error occurred
        // at that edge.
        std::vector<int64_t> edges;
        decode_detection_events_to_edges(mwpm, detection_events, edges);
        mwpm.flooder.graph.reweight_for_edges(edges);
        mwpm.search_flooder.graph.reweight_for_edges(edges);
    }

    process_timeline_until_completion(mwpm, detection_events);
    auto res = shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(mwpm, detection_events);
    if (!mwpm.flooder.negative_weight_detection_events.empty())
        res += shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(
            mwpm, mwpm.flooder.negative_weight_detection_events);
    res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
    res.weight += mwpm.flooder.negative_weight_sum;


    return res;
}

void beyond_blossom::decode_detection_events(
    beyond_blossom::Mwpm& mwpm,
    const std::vector<uint64_t>& detection_events,
    uint8_t* obs_begin_ptr,
    beyond_blossom::total_weight_int& weight,
    bool edge_correlations) {
    CorrelationScope correlation_scope(mwpm, edge_correlations);
#if BB_ENABLE_PROFILE
    beyond_blossom::profile::ScopedRange decode_range(beyond_blossom::profile::RangeId::DecodeShot);
#endif
    if (edge_correlations) {
        // Edge correlations might also be called 2-pass matching. This is the slowest and
        // highest-accuracy reweighting rule for correlated decoding in which we first decode to edges,
        // and then reweight the associated edges conditioned on the assumption that an error occurred
        // at that edge.
        std::vector<int64_t> edges;
        decode_detection_events_to_edges(mwpm, detection_events, edges);
        mwpm.flooder.graph.reweight_for_edges(edges);
        mwpm.search_flooder.graph.reweight_for_edges(edges);
    }

    size_t num_observables = mwpm.flooder.graph.num_observables;
    process_timeline_until_completion(mwpm, detection_events);

    if (num_observables > sizeof(beyond_blossom::obs_int) * 8) {
        mwpm.flooder.match_edges.clear();
        shatter_blossoms_for_all_detection_events_and_extract_match_edges(mwpm, detection_events);
        if (!mwpm.flooder.negative_weight_detection_events.empty())
            shatter_blossoms_for_all_detection_events_and_extract_match_edges(
                mwpm, mwpm.flooder.negative_weight_detection_events);
        mwpm.extract_paths_from_match_edges(mwpm.flooder.match_edges, obs_begin_ptr, weight);

        // XOR negative weight observables
        for (auto& obs : mwpm.flooder.negative_weight_observables)
            *(obs_begin_ptr + obs) ^= 1;
        // Add negative weight sum to blossom solution weight
        weight += mwpm.flooder.negative_weight_sum;

    } else {
        beyond_blossom::MatchingResult bit_packed_res =
            shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(mwpm, detection_events);
        if (!mwpm.flooder.negative_weight_detection_events.empty())
            bit_packed_res += shatter_blossoms_for_all_detection_events_and_extract_obs_mask_and_weight(
                mwpm, mwpm.flooder.negative_weight_detection_events);
        // XOR in negative weight observable mask
        bit_packed_res.obs_mask ^= mwpm.flooder.negative_weight_obs_mask;
        // Translate observable mask into bit vector
        fill_bit_vector_from_obs_mask(bit_packed_res.obs_mask, obs_begin_ptr, num_observables);
        // Add negative weight sum to blossom solution weight
        weight = bit_packed_res.weight + mwpm.flooder.negative_weight_sum;
    }

}

void beyond_blossom::decode_detection_events_to_match_edges(beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events) {
    if (mwpm.flooder.negative_weight_sum != 0)
        throw std::invalid_argument(
            "Decoding to matched detection events not supported for graphs containing edges with negative weights.");
    process_timeline_until_completion(mwpm, detection_events);
    mwpm.flooder.match_edges.clear();
    shatter_blossoms_for_all_detection_events_and_extract_match_edges(mwpm, detection_events);
}

void flip_edge(const beyond_blossom::SearchGraphEdge& edge) {
    edge.detector_node->neighbor_markers[edge.neighbor_index] ^= beyond_blossom::FLIPPED;
    auto neighbor = edge.detector_node->neighbors[edge.neighbor_index];
    if (neighbor) {
        auto idx_from_neighbor = neighbor->index_of_neighbor(edge.detector_node);
        neighbor->neighbor_markers[idx_from_neighbor] ^= beyond_blossom::FLIPPED;
    }
}

void beyond_blossom::decode_detection_events_to_edges(
    beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events, std::vector<int64_t>& edges) {
    if (mwpm.flooder.graph.nodes.size() != mwpm.search_flooder.graph.nodes.size()) {
        throw std::invalid_argument(
            "Mwpm object does not contain search flooder, which is required to decode to edges.");
    }
    process_timeline_until_completion(mwpm, detection_events);
    mwpm.flooder.match_edges.clear();
    shatter_blossoms_for_all_detection_events_and_extract_match_edges(mwpm, detection_events);
    if (!mwpm.flooder.negative_weight_detection_events.empty())
        shatter_blossoms_for_all_detection_events_and_extract_match_edges(
            mwpm, mwpm.flooder.negative_weight_detection_events);
    // Flip edges with negative weights and add to edges vector.
    for (const auto& neg_node_pair : mwpm.search_flooder.graph.negative_weight_edges) {
        auto node1_ptr = &mwpm.search_flooder.graph.nodes[neg_node_pair.first];
        auto node2_ptr =
            neg_node_pair.second != SIZE_MAX ? &mwpm.search_flooder.graph.nodes[neg_node_pair.second] : nullptr;
        SearchGraphEdge neg_edge = {node1_ptr, node1_ptr->index_of_neighbor(node2_ptr)};
        flip_edge(neg_edge);
        int64_t node1 = neg_edge.detector_node - &mwpm.search_flooder.graph.nodes[0];
        int64_t node2 = node2_ptr ? node2_ptr - &mwpm.search_flooder.graph.nodes[0] : -1;
        edges.push_back(node1);
        edges.push_back(node2);
    }
    // Flip edges along a shortest path between matched detection events and add to edges vector
    for (const auto& match_edge : mwpm.flooder.match_edges) {
        size_t node_from = match_edge.loc_from - &mwpm.flooder.graph.nodes[0];
        size_t node_to = match_edge.loc_to ? match_edge.loc_to - &mwpm.flooder.graph.nodes[0] : SIZE_MAX;
        mwpm.search_flooder.iter_edges_on_shortest_path_from_middle(node_from, node_to, [&](const SearchGraphEdge& e) {
            flip_edge(e);
            int64_t node1 = e.detector_node - &mwpm.search_flooder.graph.nodes[0];
            auto node2_ptr = e.detector_node->neighbors[e.neighbor_index];
            int64_t node2 = node2_ptr ? node2_ptr - &mwpm.search_flooder.graph.nodes[0] : -1;
            edges.push_back(node1);
            edges.push_back(node2);
        });
    }
    // Remove any edges in the edges vector that are no longer flipped. Also unflip edges.
    for (size_t i = 0; i < edges.size() / 2;) {
        int64_t u = edges[2 * i];
        int64_t v = edges[2 * i + 1];
        auto& u_node = mwpm.search_flooder.graph.nodes[u];
        size_t idx;
        if (v == -1) {
            idx = 0;
        } else {
            auto v_ptr = &mwpm.search_flooder.graph.nodes[v];
            idx = u_node.index_of_neighbor(v_ptr);
        }
        if (!(u_node.neighbor_markers[idx] & beyond_blossom::FLIPPED)) {
            edges[2 * i] = edges[edges.size() - 2];
            edges[2 * i + 1] = edges[edges.size() - 1];
            edges.resize(edges.size() - 2);
        } else {
            flip_edge({&u_node, idx});
            i++;
        }
    }
}

void beyond_blossom::decode_detection_events_to_edges_with_edge_correlations(
    beyond_blossom::Mwpm& mwpm, const std::vector<uint64_t>& detection_events, std::vector<int64_t>& edges) {
    CorrelationScope correlation_scope(mwpm, true);
    decode_detection_events_to_edges(mwpm, detection_events, edges);
    mwpm.flooder.graph.reweight_for_edges(edges);
    mwpm.search_flooder.graph.reweight_for_edges(edges);
    edges.clear();

    decode_detection_events_to_edges(mwpm, detection_events, edges);

}
