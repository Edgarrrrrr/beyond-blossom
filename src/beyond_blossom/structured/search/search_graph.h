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

#ifndef BB_STRUCTURED_SEARCH_GRAPH_H
#define BB_STRUCTURED_SEARCH_GRAPH_H

#include <unordered_map>

#include "beyond_blossom/structured/flooder/graph.h"
#include "beyond_blossom/structured/search/search_detector_node.h"

namespace beyond_blossom::structured {

/// The edge on which two search regions collided
struct SearchGraphEdge {
    SearchDetectorNode* detector_node;
    size_t neighbor_index;
};

class SearchGraph {
   public:
    std::vector<SearchDetectorNode> nodes;
    std::unique_ptr<StaticGridStorage<SearchDetectorNode, std::vector<size_t>>> grid;
    SearchDetectorNode& node(size_t id) {
        return *grid->detector_nodes[id];
    }
    const SearchDetectorNode& node(size_t id) const {
        return *grid->detector_nodes[id];
    }
    size_t node_id(const SearchDetectorNode* n) const {
        return n->detector_id;
    }
    void configure_grid(const StaticGridSpec& spec) {
        grid = build_static_grid<SearchDetectorNode, std::vector<size_t>>(nodes, spec);
    }
    size_t num_nodes;
    std::vector<std::pair<size_t, size_t>> negative_weight_edges;

    // Used to restore weights after reweighting.
    std::vector<PreviousWeight> previous_weights;
    std::map<size_t, std::vector<std::vector<ImpliedWeightUnconverted>>> edges_to_implied_weights_unconverted;

    SearchGraph();
    explicit SearchGraph(size_t num_nodes);
    SearchGraph(SearchGraph&& graph) noexcept;
    void add_edge(
        size_t u,
        size_t v,
        signed_weight_int weight,
        const std::vector<size_t>& observables,
        const std::vector<ImpliedWeightUnconverted>& implied_weights = {});
    void add_boundary_edge(
        size_t u,
        signed_weight_int weight,
        const std::vector<size_t>& observables,
        const std::vector<ImpliedWeightUnconverted>& implied_weights = {});
    void convert_implied_weights(const double normalizing_constant);
    void reweight(std::vector<ImpliedWeight>& implied_weights);
    void reweight_for_edge(const int64_t& u, const int64_t& v);
    void reweight_for_edges(const std::vector<int64_t>& edges);
    void undo_reweights();
};

inline void SearchGraph::reweight(std::vector<ImpliedWeight>& implied_weights) {
    apply_reweights(implied_weights, previous_weights);
}

}  // namespace beyond_blossom::structured

#endif  // BB_STRUCTURED_SEARCH_GRAPH_H
