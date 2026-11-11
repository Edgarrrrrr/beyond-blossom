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

#include "search_graph.h"

#include <cassert>
#include <cmath>

namespace beyond_blossom::structured {

SearchGraph::SearchGraph() : num_nodes(0) {
}

SearchGraph::SearchGraph(size_t num_nodes) : num_nodes(num_nodes) {
    nodes.resize(num_nodes);
}

SearchGraph::SearchGraph(SearchGraph&& graph) noexcept
    : nodes(std::move(graph.nodes)),
      grid(std::move(graph.grid)),
      num_nodes(graph.num_nodes),
      negative_weight_edges(std::move(graph.negative_weight_edges)),
      previous_weights(std::move(graph.previous_weights)),
      edges_to_implied_weights_unconverted(std::move(graph.edges_to_implied_weights_unconverted)) {
}

void SearchGraph::add_edge(
    size_t u,
    size_t v,
    signed_weight_int weight,
    const std::vector<size_t>& observables,
    const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
    size_t larger_node = std::max(u, v);
    if (larger_node + 1 > nodes.size()) {
        throw std::invalid_argument(
            "Node " + std::to_string(larger_node) +
            " exceeds number of nodes "
            "in graph (" +
            std::to_string(num_nodes) + ")");
    }

    // Self-loops are ignored, since they are never included in the shortest-path (the search graph has only
    // positive weights).
    if (u == v)
        return;

    uint8_t weight_sign = 0;
    if (weight < 0) {
        negative_weight_edges.push_back({u, v});
        weight_sign = WEIGHT_SIGN;
    }

    nodes[u].neighbors.push_back(&(nodes[v]));
    nodes[u].neighbor_weights.push_back(std::abs(weight));
    nodes[u].neighbor_observable_indices.push_back(observables);
    nodes[u].neighbor_markers.push_back(weight_sign);
    nodes[u].neighbor_implied_weights.push_back({});
    edges_to_implied_weights_unconverted[u].emplace_back(implied_weights_for_other_edges);

    nodes[v].neighbors.push_back(&(nodes[u]));
    nodes[v].neighbor_weights.push_back(std::abs(weight));
    nodes[v].neighbor_observable_indices.push_back(observables);
    nodes[v].neighbor_markers.push_back(weight_sign);
    nodes[v].neighbor_implied_weights.push_back({});
    edges_to_implied_weights_unconverted[v].emplace_back(implied_weights_for_other_edges);
}

void SearchGraph::add_boundary_edge(
    size_t u,
    signed_weight_int weight,
    const std::vector<size_t>& observables,
    const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
    if (u >= nodes.size()) {
        throw std::invalid_argument(
            "Node " + std::to_string(u) +
            " exceeds number of nodes "
            "in graph (" +
            std::to_string(num_nodes) + ")");
    }

    uint8_t weight_sign = 0;
    if (weight < 0) {
        negative_weight_edges.push_back({u, SIZE_MAX});
        weight_sign = WEIGHT_SIGN;
    }

    nodes[u].neighbors.insert(nodes[u].neighbors.begin(), 1, nullptr);
    nodes[u].neighbor_weights.insert(nodes[u].neighbor_weights.begin(), 1, std::abs(weight));
    nodes[u].neighbor_observable_indices.insert(nodes[u].neighbor_observable_indices.begin(), 1, observables);
    nodes[u].neighbor_markers.insert(nodes[u].neighbor_markers.begin(), 1, weight_sign);
    nodes[u].neighbor_implied_weights.insert(nodes[u].neighbor_implied_weights.begin(), 1, {});
    edges_to_implied_weights_unconverted[u].insert(
        edges_to_implied_weights_unconverted[u].begin(), 1, implied_weights_for_other_edges);
}

// Reweight assuming an error has occurred on a single edge u, v. When v == -1, assumes an edge from
// u to the boundary.
void SearchGraph::reweight_for_edge(const int64_t& u, const int64_t& v) {
    size_t z = node(u).index_of_neighbor(v == -1 ? nullptr : &node(v));
    reweight(node(u).edge_implied_weights(z));
}

void SearchGraph::reweight_for_edges(const std::vector<int64_t>& edges) {
    for (size_t i = 0; i < edges.size() >> 1; ++i) {
        int64_t u = edges[2 * i];
        int64_t v = edges[2 * i + 1];
        reweight_for_edge(u, v);
    }
}

void SearchGraph::undo_reweights() {
    // We iterate backward over the previous weights, since some edges
    // may have been reweighted more than once.
    for (auto it = previous_weights.rbegin(); it != previous_weights.rend(); ++it) {
        PreviousWeight& prev = *it;
        *prev.ptr = prev.val;
    }
    previous_weights.clear();
}

void SearchGraph::convert_implied_weights(double) {
    throw std::invalid_argument("The independent static-grid backend does not support correlated decoding.");
}

}  // namespace beyond_blossom::structured
