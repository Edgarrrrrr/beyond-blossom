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

#ifndef PYMATCHING_BACKEND_USER_GRAPH_H
#define PYMATCHING_BACKEND_USER_GRAPH_H

#include <optional>

#include "pymatching/sparse_blossom/driver/user_graph.h"
#include "pymatching/sparse_blossom/structured/driver/component_decoder.h"
#include "pymatching/sparse_blossom/structured/grid/static_grid.h"
#include "pymatching/sparse_blossom/structured/matcher/mwpm.h"

namespace pm {

// API adapter owning independent decoder caches. The original UserGraph remains
// unchanged so its construction, layout, and generated generic code are stable.
class BackendUserGraph {
   public:
    std::vector<UserNode> nodes;
    std::list<UserEdge> edges;
    std::set<size_t> boundary_nodes;
    bool loaded_from_dem_without_correlations = false;

    BackendUserGraph();
    explicit BackendUserGraph(size_t num_nodes);
    BackendUserGraph(size_t num_nodes, size_t num_observables);
    BackendUserGraph(size_t num_nodes, size_t num_observables, pm::weight_int num_distinct_weights);
    void merge_edge_or_boundary_edge(
        size_t node,
        size_t neighbor_index,
        const std::vector<size_t>& parallel_observables,
        double parallel_weight,
        double parallel_error_probability,
        MERGE_STRATEGY merge_strategy = DISALLOW);
    void add_or_merge_edge(
        size_t node1,
        size_t node2,
        const std::vector<size_t>& observables,
        double weight,
        double error_probability,
        MERGE_STRATEGY merge_strategy = DISALLOW);
    void add_or_merge_boundary_edge(
        size_t node,
        const std::vector<size_t>& observables,
        double weight,
        double error_probability,
        MERGE_STRATEGY merge_strategy = DISALLOW);
    bool has_edge(size_t node1, size_t node2);
    bool has_boundary_edge(size_t node);
    bool get_edge_or_boundary_edge_weight(size_t node1, size_t node2, double& weight_out);
    void set_boundary(const std::set<size_t>& boundary);
    std::set<size_t> get_boundary();
    size_t get_num_observables();
    void set_min_num_observables(size_t num_observables);
    size_t get_num_nodes();
    size_t get_num_detectors();
    size_t get_num_edges();
    bool is_boundary_node(size_t node_id);
    void add_noise(uint8_t* error_arr, uint8_t* syndrome_arr) const;
    bool all_edges_have_error_probabilities();
    double max_abs_weight();
    double get_edge_weight_normalising_constant(size_t max_num_distinct_weights);
    template <typename EdgeCallable, typename BoundaryEdgeCallable>
    double iter_discretized_edges(
        pm::weight_int num_distinct_weights,
        const EdgeCallable& edge_func,
        const BoundaryEdgeCallable& boundary_edge_func);
    template <typename EdgeCallable, typename BoundaryEdgeCallable>
    double to_matching_or_search_graph_helper(
        pm::weight_int num_distinct_weights,
        const EdgeCallable& edge_func,
        const BoundaryEdgeCallable& boundary_edge_func);
    pm::MatchingGraph to_matching_graph(pm::weight_int num_distinct_weights);
    pm::SearchGraph to_search_graph(pm::weight_int num_distinct_weights);
    pm::Mwpm to_mwpm(pm::weight_int num_distinct_weights, bool ensure_search_graph_included);
    void update_mwpm();
    Mwpm& get_mwpm();
    Mwpm& get_mwpm_with_search_graph();
    structured::MatchingGraph to_structured_matching_graph(pm::weight_int num_distinct_weights);
    structured::SearchGraph to_structured_search_graph(pm::weight_int num_distinct_weights);
    structured::Mwpm to_structured_mwpm(pm::weight_int num_distinct_weights, bool ensure_search_graph_included);
    structured::ComponentDecoder& get_structured_mwpm();
    structured::ComponentDecoder& get_structured_mwpm_with_search_graph();
    void handle_dem_instruction(double p, const std::vector<size_t>& detectors, const std::vector<size_t>& observables);
    void handle_dem_instruction_include_correlations(
        double p, const std::vector<size_t>& detectors, const std::vector<size_t>& observables);
    void get_nodes_on_shortest_path_from_source(size_t src, size_t dst, std::vector<size_t>& out_nodes);
    void populate_implied_edge_weights(
        std::map<std::pair<size_t, size_t>, std::map<std::pair<size_t, size_t>, double>>& joint_probabilites);
    void configure_static_grid(StaticGridSpec spec);
    void disable_static_grid();
    const std::optional<StaticGridSpec>& static_grid_spec() const {
        return _grid_spec;
    }

   private:
    pm::Mwpm _mwpm;
    size_t _num_observables;
    // Preserve construction-time precision whenever a decoder cache is rebuilt.
    pm::weight_int _num_distinct_weights = pm::NUM_DISTINCT_WEIGHTS;
    bool _mwpm_needs_updating;
    bool _all_edges_have_error_probabilities;
    std::optional<StaticGridSpec> _grid_spec;
    structured::ComponentDecoder _structured_mwpm;
    bool _structured_mwpm_needs_updating = true;
    void get_generic_nodes_on_shortest_path_from_source(size_t src, size_t dst, std::vector<size_t>& out_nodes);
    void get_structured_nodes_on_shortest_path_from_source(size_t src, size_t dst, std::vector<size_t>& out_nodes);
};

template <typename EdgeCallable, typename BoundaryEdgeCallable>
inline double BackendUserGraph::iter_discretized_edges(
    pm::weight_int num_distinct_weights,
    const EdgeCallable& edge_func,
    const BoundaryEdgeCallable& boundary_edge_func) {
    double normalising_constant = get_edge_weight_normalising_constant(num_distinct_weights);

    for (auto& e : edges) {
        pm::signed_weight_int w = (pm::signed_weight_int)round(e.weight * normalising_constant);
        // Extremely important!
        // If all edge weights are even integers, then all collision events occur at integer times.
        w *= 2;
        bool node1_boundary = is_boundary_node(e.node1);
        bool node2_boundary = is_boundary_node(e.node2);
        if (node2_boundary && !node1_boundary) {
            boundary_edge_func(e.node1, w, e.observable_indices, e.implied_weights_for_other_edges);
        } else if (node1_boundary && !node2_boundary) {
            boundary_edge_func(e.node2, w, e.observable_indices, e.implied_weights_for_other_edges);
        } else if (!node1_boundary) {
            edge_func(e.node1, e.node2, w, e.observable_indices, e.implied_weights_for_other_edges);
        }
    }
    return normalising_constant * 2;
}

template <typename EdgeCallable, typename BoundaryEdgeCallable>
inline double BackendUserGraph::to_matching_or_search_graph_helper(
    pm::weight_int num_distinct_weights,
    const EdgeCallable& edge_func,
    const BoundaryEdgeCallable& boundary_edge_func) {
    // Use vectors to store boundary edges initially before adding them to the graph, so
    // that parallel boundary edges with negative edge weights can be handled correctly
    std::vector<bool> has_boundary_edge(nodes.size(), false);
    std::vector<pm::signed_weight_int> boundary_edge_weights(nodes.size());
    std::vector<std::vector<size_t>> boundary_edge_observables(nodes.size());
    std::vector<std::vector<ImpliedWeightUnconverted>> boundary_edge_implied_weights_unconverted(nodes.size());

    double normalising_constant = iter_discretized_edges(
        num_distinct_weights,
        edge_func,
        [&](size_t u,
            pm::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            // For parallel boundary edges, keep the boundary edge with the smaller weight
            if (!has_boundary_edge[u] || boundary_edge_weights[u] > weight) {
                boundary_edge_weights[u] = weight;
                boundary_edge_observables[u] = observables;
                has_boundary_edge[u] = true;
                boundary_edge_implied_weights_unconverted[u] = implied_weights_for_other_edges;
            }
        });

    // Now add boundary edges to the graph
    for (size_t i = 0; i < has_boundary_edge.size(); i++) {
        if (has_boundary_edge[i])
            boundary_edge_func(
                i,
                boundary_edge_weights[i],
                boundary_edge_observables[i],
                boundary_edge_implied_weights_unconverted[i]);
    }
    return normalising_constant;
}

BackendUserGraph detector_error_model_to_backend_user_graph(
    const stim::DetectorErrorModel& detector_error_model,
    bool enable_correlations,
    pm::weight_int num_distinct_weights);

StaticGridSpec static_grid_spec_from_dem(
    const stim::DetectorErrorModel& dem,
    size_t distance,
    size_t rounds,
    bool rotated,
    GridSimd simd = GridSimd::automatic);

}  // namespace pm

#endif  // PYMATCHING_BACKEND_USER_GRAPH_H
