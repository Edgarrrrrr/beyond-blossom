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

#include "beyond_blossom/driver/backend_user_graph.h"

#include "beyond_blossom/rand/rand_gen.h"

namespace {

bool is_valid_probability(double p) {
    return (p >= 0 && p <= 1);
}

}  // namespace

void beyond_blossom::BackendUserGraph::merge_edge_or_boundary_edge(
    size_t node,
    size_t neighbor_index,
    const std::vector<size_t>& parallel_observables,
    double parallel_weight,
    double parallel_error_probability,
    beyond_blossom::MERGE_STRATEGY merge_strategy) {
    auto& neighbor = nodes[node].neighbors[neighbor_index];
    if (merge_strategy == DISALLOW) {
        throw std::invalid_argument(
            "Edge (" + std::to_string(neighbor.edge_it->node1) + ", " + std::to_string(neighbor.edge_it->node2) +
            ") already exists in the graph. "
            "Parallel edges not permitted with the provided `disallow` `merge_strategy`. Please provide a "
            "different `merge_strategy`.");
    } else if (
        merge_strategy == KEEP_ORIGINAL ||
        (merge_strategy == SMALLEST_WEIGHT && parallel_weight >= neighbor.edge_it->weight)) {
        return;
    } else {
        double new_weight, new_error_probability;
        bool use_new_observables;
        if (merge_strategy == REPLACE || merge_strategy == SMALLEST_WEIGHT) {
            new_weight = parallel_weight;
            new_error_probability = parallel_error_probability;
            use_new_observables = true;
        } else if (merge_strategy == INDEPENDENT) {
            new_weight = beyond_blossom::merge_weights(parallel_weight, neighbor.edge_it->weight);
            new_error_probability = -1;
            if (is_valid_probability(neighbor.edge_it->error_probability) &&
                is_valid_probability(parallel_error_probability))
                new_error_probability = parallel_error_probability * (1 - neighbor.edge_it->error_probability) +
                                        neighbor.edge_it->error_probability * (1 - parallel_error_probability);
            // We do not need to update the observables. If they do not match up, then the code has distance 2.
            use_new_observables = false;
        } else {
            throw std::invalid_argument("Merge strategy not recognised.");
        }
        // Update the existing edge weight and probability in the adjacency list of `node`
        neighbor.edge_it->weight = new_weight;
        neighbor.edge_it->error_probability = new_error_probability;
        if (use_new_observables)
            neighbor.edge_it->observable_indices = parallel_observables;

        _mwpm_needs_updating = true;
        _structured_mwpm_needs_updating = true;
        if (new_error_probability < 0 || new_error_probability > 1)
            _all_edges_have_error_probabilities = false;
    }
}

void beyond_blossom::BackendUserGraph::add_or_merge_edge(
    size_t node1,
    size_t node2,
    const std::vector<size_t>& observables,
    double weight,
    double error_probability,
    MERGE_STRATEGY merge_strategy) {
    auto max_id = std::max(node1, node2);
    if (max_id + 1 > nodes.size())
        nodes.resize(max_id + 1);

    size_t idx = nodes[node1].index_of_neighbor(node2);

    if (idx == SIZE_MAX) {
        beyond_blossom::UserEdge edge = {node1, node2, observables, weight, error_probability};
        edges.push_back(edge);
        nodes[node1].neighbors.push_back({std::prev(edges.end()), 1});
        if (node1 != node2)
            nodes[node2].neighbors.push_back({std::prev(edges.end()), 0});

        for (auto& obs : observables) {
            if (obs + 1 > _num_observables)
                _num_observables = obs + 1;
        }
        _mwpm_needs_updating = true;
        _structured_mwpm_needs_updating = true;
        if (error_probability < 0 || error_probability > 1)
            _all_edges_have_error_probabilities = false;
    } else {
        merge_edge_or_boundary_edge(node1, idx, observables, weight, error_probability, merge_strategy);
    }
}

void beyond_blossom::BackendUserGraph::add_or_merge_boundary_edge(
    size_t node,
    const std::vector<size_t>& observables,
    double weight,
    double error_probability,
    MERGE_STRATEGY merge_strategy) {
    if (node + 1 > nodes.size())
        nodes.resize(node + 1);

    size_t idx = nodes[node].index_of_neighbor(SIZE_MAX);

    if (idx == SIZE_MAX) {
        beyond_blossom::UserEdge edge = {node, SIZE_MAX, observables, weight, error_probability};
        edges.push_back(edge);
        nodes[node].neighbors.push_back({std::prev(edges.end()), 1});

        for (auto& obs : observables) {
            if (obs + 1 > _num_observables)
                _num_observables = obs + 1;
        }
        _mwpm_needs_updating = true;
        _structured_mwpm_needs_updating = true;
        if (error_probability < 0 || error_probability > 1)
            _all_edges_have_error_probabilities = false;
    } else {
        merge_edge_or_boundary_edge(node, idx, observables, weight, error_probability, merge_strategy);
    }
}

beyond_blossom::BackendUserGraph::BackendUserGraph()
    : _num_observables(0), _mwpm_needs_updating(true), _all_edges_have_error_probabilities(true) {
}

beyond_blossom::BackendUserGraph::BackendUserGraph(size_t num_nodes)
    : _num_observables(0), _mwpm_needs_updating(true), _all_edges_have_error_probabilities(true) {
    nodes.resize(num_nodes);
}

beyond_blossom::BackendUserGraph::BackendUserGraph(size_t num_nodes, size_t num_observables)
    : _num_observables(num_observables), _mwpm_needs_updating(true), _all_edges_have_error_probabilities(true) {
    nodes.resize(num_nodes);
}

beyond_blossom::BackendUserGraph::BackendUserGraph(size_t num_nodes, size_t num_observables, beyond_blossom::weight_int num_distinct_weights)
    : _num_observables(num_observables),
      _num_distinct_weights(num_distinct_weights),
      _mwpm_needs_updating(true),
      _all_edges_have_error_probabilities(true) {
    nodes.resize(num_nodes);
}

void beyond_blossom::BackendUserGraph::set_boundary(const std::set<size_t>& boundary) {
    for (auto& n : boundary_nodes)
        nodes[n].is_boundary = false;
    boundary_nodes = boundary;
    for (auto& n : boundary_nodes) {
        if (n >= nodes.size())
            nodes.resize(n + 1);
        nodes[n].is_boundary = true;
    }
    _mwpm_needs_updating = true;
    _structured_mwpm_needs_updating = true;
}

std::set<size_t> beyond_blossom::BackendUserGraph::get_boundary() {
    return boundary_nodes;
}

size_t beyond_blossom::BackendUserGraph::get_num_observables() {
    return _num_observables;
}

size_t beyond_blossom::BackendUserGraph::get_num_nodes() {
    return nodes.size();
}

size_t beyond_blossom::BackendUserGraph::get_num_detectors() {
    return get_num_nodes() - boundary_nodes.size();
}

bool beyond_blossom::BackendUserGraph::is_boundary_node(size_t node_id) {
    return (node_id == SIZE_MAX) || nodes[node_id].is_boundary;
}

void beyond_blossom::BackendUserGraph::update_mwpm() {
    _mwpm = to_mwpm(_num_distinct_weights, false);
    _mwpm_needs_updating = false;
}

beyond_blossom::Mwpm& beyond_blossom::BackendUserGraph::get_mwpm() {
    if (_mwpm_needs_updating)
        update_mwpm();
    return _mwpm;
}

void beyond_blossom::BackendUserGraph::add_noise(uint8_t* error_arr, uint8_t* syndrome_arr) const {
    if (!_all_edges_have_error_probabilities)
        return;

    for (auto& e : edges) {
        auto p = e.error_probability;
        if (rand_float(0.0, 1.0) < p) {
            // Flip the observables
            for (auto& obs : e.observable_indices) {
                *(error_arr + obs) ^= 1;
            }
            // Flip the syndrome bits
            *(syndrome_arr + e.node1) ^= 1;
            if (e.node2 != SIZE_MAX)
                *(syndrome_arr + e.node2) ^= 1;
        }
    }

    for (auto& b : boundary_nodes)
        *(syndrome_arr + b) = 0;
}

size_t beyond_blossom::BackendUserGraph::get_num_edges() {
    return edges.size();
}

bool beyond_blossom::BackendUserGraph::all_edges_have_error_probabilities() {
    return _all_edges_have_error_probabilities;
}

double beyond_blossom::BackendUserGraph::max_abs_weight() {
    double max_abs_weight = 0;
    for (auto& e : edges) {
        if (std::abs(e.weight) > max_abs_weight) {
            max_abs_weight = std::abs(e.weight);
        }
    }
    return max_abs_weight;
}

beyond_blossom::MatchingGraph beyond_blossom::BackendUserGraph::to_matching_graph(beyond_blossom::weight_int num_distinct_weights) {
    beyond_blossom::MatchingGraph matching_graph(nodes.size(), _num_observables);

    double normalising_constant = to_matching_or_search_graph_helper(
        num_distinct_weights,
        [&](size_t u,
            size_t v,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            matching_graph.add_edge(u, v, weight, observables, implied_weights_for_other_edges);
        },
        [&](size_t u,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            matching_graph.add_boundary_edge(u, weight, observables, implied_weights_for_other_edges);
        });

    matching_graph.normalising_constant = normalising_constant;
    if (boundary_nodes.size() > 0) {
        matching_graph.is_user_graph_boundary_node.clear();
        matching_graph.is_user_graph_boundary_node.resize(nodes.size(), false);
        for (auto& i : boundary_nodes)
            matching_graph.is_user_graph_boundary_node[i] = true;
    }

    matching_graph.convert_implied_weights(normalising_constant);
    return matching_graph;
}

beyond_blossom::SearchGraph beyond_blossom::BackendUserGraph::to_search_graph(beyond_blossom::weight_int num_distinct_weights) {
    /// Identical to to_matching_graph but for constructing a beyond_blossom::SearchGraph
    beyond_blossom::SearchGraph search_graph(nodes.size());

    double normalizing_constant = to_matching_or_search_graph_helper(
        num_distinct_weights,
        [&](size_t u,
            size_t v,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            search_graph.add_edge(u, v, weight, observables, implied_weights_for_other_edges);
        },
        [&](size_t u,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            search_graph.add_boundary_edge(u, weight, observables, implied_weights_for_other_edges);
        });

    search_graph.convert_implied_weights(normalizing_constant);
    return search_graph;
}

beyond_blossom::Mwpm beyond_blossom::BackendUserGraph::to_mwpm(beyond_blossom::weight_int num_distinct_weights, bool ensure_search_graph_included) {
    if (_num_observables > sizeof(beyond_blossom::obs_int) * 8 || ensure_search_graph_included) {
        auto mwpm = beyond_blossom::Mwpm(
            beyond_blossom::GraphFlooder(to_matching_graph(num_distinct_weights)),
            beyond_blossom::SearchFlooder(to_search_graph(num_distinct_weights)));
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        mwpm.flooder.graph.loaded_from_dem_without_correlations = loaded_from_dem_without_correlations;
        return mwpm;
    } else {
        auto mwpm = beyond_blossom::Mwpm(beyond_blossom::GraphFlooder(to_matching_graph(num_distinct_weights)));
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        mwpm.flooder.graph.loaded_from_dem_without_correlations = loaded_from_dem_without_correlations;
        return mwpm;
    }
}

beyond_blossom::Mwpm& beyond_blossom::BackendUserGraph::get_mwpm_with_search_graph() {
    if (!_mwpm_needs_updating && _mwpm.flooder.graph.nodes.size() == _mwpm.search_flooder.graph.nodes.size()) {
        return _mwpm;
    } else {
        _mwpm = to_mwpm(_num_distinct_weights, true);
        _mwpm_needs_updating = false;
        return _mwpm;
    }
}

beyond_blossom::structured::MatchingGraph beyond_blossom::BackendUserGraph::to_structured_matching_graph(beyond_blossom::weight_int num_distinct_weights) {
    beyond_blossom::structured::MatchingGraph matching_graph(nodes.size(), _num_observables);

    double normalising_constant = to_matching_or_search_graph_helper(
        num_distinct_weights,
        [&](size_t u,
            size_t v,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            matching_graph.add_edge(u, v, weight, observables, implied_weights_for_other_edges);
        },
        [&](size_t u,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            matching_graph.add_boundary_edge(u, weight, observables, implied_weights_for_other_edges);
        });

    matching_graph.normalising_constant = normalising_constant;
    if (boundary_nodes.size() > 0) {
        matching_graph.is_user_graph_boundary_node.clear();
        matching_graph.is_user_graph_boundary_node.resize(nodes.size(), false);
        for (auto& i : boundary_nodes)
            matching_graph.is_user_graph_boundary_node[i] = true;
    }

    matching_graph.configure_grid(_grid_spec.value());
    matching_graph.convert_implied_weights(normalising_constant);
    return matching_graph;
}

beyond_blossom::structured::SearchGraph beyond_blossom::BackendUserGraph::to_structured_search_graph(beyond_blossom::weight_int num_distinct_weights) {
    /// Identical to to_structured_matching_graph but for constructing a beyond_blossom::structured::SearchGraph
    beyond_blossom::structured::SearchGraph search_graph(nodes.size());

    double normalizing_constant = to_matching_or_search_graph_helper(
        num_distinct_weights,
        [&](size_t u,
            size_t v,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            search_graph.add_edge(u, v, weight, observables, implied_weights_for_other_edges);
        },
        [&](size_t u,
            beyond_blossom::signed_weight_int weight,
            const std::vector<size_t>& observables,
            const std::vector<ImpliedWeightUnconverted>& implied_weights_for_other_edges) {
            search_graph.add_boundary_edge(u, weight, observables, implied_weights_for_other_edges);
        });

    search_graph.configure_grid(_grid_spec.value());
    search_graph.convert_implied_weights(normalizing_constant);
    return search_graph;
}

beyond_blossom::structured::Mwpm beyond_blossom::BackendUserGraph::to_structured_mwpm(
    beyond_blossom::weight_int num_distinct_weights, bool ensure_search_graph_included) {
    if (_num_observables > sizeof(beyond_blossom::obs_int) * 8 || ensure_search_graph_included) {
        auto mwpm = beyond_blossom::structured::Mwpm(
            beyond_blossom::structured::GraphFlooder(to_structured_matching_graph(num_distinct_weights)),
            beyond_blossom::structured::SearchFlooder(to_structured_search_graph(num_distinct_weights)));
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        mwpm.flooder.graph.loaded_from_dem_without_correlations = loaded_from_dem_without_correlations;
        return mwpm;
    } else {
        auto mwpm =
            beyond_blossom::structured::Mwpm(beyond_blossom::structured::GraphFlooder(to_structured_matching_graph(num_distinct_weights)));
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        mwpm.flooder.graph.loaded_from_dem_without_correlations = loaded_from_dem_without_correlations;
        return mwpm;
    }
}

beyond_blossom::structured::ComponentDecoder& beyond_blossom::BackendUserGraph::get_structured_mwpm() {
    if (_structured_mwpm_needs_updating) {
        _structured_mwpm = structured::ComponentDecoder(*this, _grid_spec.value(), _num_distinct_weights, false);
        _structured_mwpm_needs_updating = false;
    }
    return _structured_mwpm;
}

beyond_blossom::structured::ComponentDecoder& beyond_blossom::BackendUserGraph::get_structured_mwpm_with_search_graph() {
    if (!_structured_mwpm_needs_updating && _structured_mwpm.has_search_graph()) {
        return _structured_mwpm;
    }
    _structured_mwpm = structured::ComponentDecoder(*this, _grid_spec.value(), _num_distinct_weights, true);
    _structured_mwpm_needs_updating = false;
    return _structured_mwpm;
}

void beyond_blossom::BackendUserGraph::handle_dem_instruction(
    double p, const std::vector<size_t>& detectors, const std::vector<size_t>& observables) {
    if (detectors.size() == 2) {
        add_or_merge_edge(detectors[0], detectors[1], observables, std::log((1 - p) / p), p, INDEPENDENT);
    } else if (detectors.size() == 1) {
        add_or_merge_boundary_edge(detectors[0], observables, std::log((1 - p) / p), p, INDEPENDENT);
    }
}

void beyond_blossom::BackendUserGraph::handle_dem_instruction_include_correlations(
    double p, const std::vector<size_t>& detectors, const std::vector<size_t>& observables) {
    if (detectors.size() == 2) {
        add_or_merge_edge(detectors[0], detectors[1], observables, beyond_blossom::to_weight_for_correlations(p), p, INDEPENDENT);
    } else if (detectors.size() == 1) {
        add_or_merge_boundary_edge(detectors[0], observables, beyond_blossom::to_weight_for_correlations(p), p, INDEPENDENT);
    }
}

void beyond_blossom::BackendUserGraph::get_nodes_on_shortest_path_from_source(
    size_t src, size_t dst, std::vector<size_t>& out_nodes) {
    if (_grid_spec) {
        get_structured_nodes_on_shortest_path_from_source(src, dst, out_nodes);
    } else {
        get_generic_nodes_on_shortest_path_from_source(src, dst, out_nodes);
    }
}

void beyond_blossom::BackendUserGraph::get_generic_nodes_on_shortest_path_from_source(
    size_t src, size_t dst, std::vector<size_t>& out_nodes) {
    auto& mwpm = get_mwpm_with_search_graph();
    bool src_is_boundary = is_boundary_node(src);
    bool dst_is_boundary = is_boundary_node(dst);
    if (src != SIZE_MAX && src >= nodes.size())
        throw std::invalid_argument("node " + std::to_string(src) + " is not in the graph");
    if (dst != SIZE_MAX && dst >= nodes.size())
        throw std::invalid_argument("node " + std::to_string(dst) + " is not in the graph");
    if (!src_is_boundary) {
        size_t dst_tmp = dst_is_boundary ? SIZE_MAX : dst;
        mwpm.search_flooder.iter_edges_on_shortest_path_from_source(src, dst_tmp, [&](const beyond_blossom::SearchGraphEdge edge) {
            out_nodes.push_back(edge.detector_node - &mwpm.search_flooder.graph.nodes[0]);
        });
        if (!dst_is_boundary)
            out_nodes.push_back(dst);
    } else if (!dst_is_boundary) {
        std::vector<size_t> temp_out_nodes;
        get_generic_nodes_on_shortest_path_from_source(dst, src, temp_out_nodes);
        for (size_t i = 0; i < temp_out_nodes.size(); i++) {
            out_nodes.push_back(temp_out_nodes[temp_out_nodes.size() - 1 - i]);
        }
    } else {
        throw std::invalid_argument("Both the source and destination vertices provided are boundary nodes");
    }
}

void beyond_blossom::BackendUserGraph::get_structured_nodes_on_shortest_path_from_source(
    size_t src, size_t dst, std::vector<size_t>& out_nodes) {
    get_structured_mwpm_with_search_graph().get_nodes_on_shortest_path_from_source(src, dst, out_nodes);
}

void beyond_blossom::BackendUserGraph::configure_static_grid(StaticGridSpec spec) {
    // Validate construction without invalidating a working decoder on failure.
    auto previous = std::move(_grid_spec);
    _grid_spec = std::move(spec);
    try {
        auto candidate = structured::ComponentDecoder(*this, _grid_spec.value(), _num_distinct_weights, false);
        _structured_mwpm = std::move(candidate);
        _structured_mwpm_needs_updating = false;
    } catch (...) {
        _grid_spec = std::move(previous);
        throw;
    }
}

beyond_blossom::StaticGridSpec beyond_blossom::static_grid_spec_from_dem(
    const stim::DetectorErrorModel& dem, size_t distance, size_t rounds, bool rotated, GridSimd simd) {
    StaticGridSpec spec{distance, rounds, rotated, {}, {}, simd};
    const auto num_detectors = dem.count_detectors();
    std::set<uint64_t> ids;
    for (size_t k = 0; k < num_detectors; k++)
        ids.insert(k);
    auto coordinates = dem.get_detector_coordinates(ids);
    for (size_t k = 0; k < num_detectors; k++) {
        const auto& values = coordinates[k];
        if (values.size() < 3)
            throw std::invalid_argument("Static grid requires three coordinates for every detector.");
        std::array<int64_t, 3> p;
        for (size_t axis = 0; axis < 3; axis++) {
            double value = values[axis];
            if (!std::isfinite(value) || value != std::floor(value) || std::abs(value) > double(int64_t{1} << 30))
                throw std::invalid_argument("Static grid requires finite, bounded integer coordinates.");
            p[axis] = int64_t(value);
        }
        spec.coordinates.push_back(p);
    }
    return spec;
}

void beyond_blossom::BackendUserGraph::disable_static_grid() {
    _grid_spec.reset();
    _structured_mwpm = beyond_blossom::structured::ComponentDecoder();
    _structured_mwpm_needs_updating = true;
}

bool beyond_blossom::BackendUserGraph::has_edge(size_t node1, size_t node2) {
    if (node1 >= nodes.size())
        return false;
    return nodes[node1].index_of_neighbor(node2) != SIZE_MAX;
}

bool beyond_blossom::BackendUserGraph::has_boundary_edge(size_t node) {
    if (node >= nodes.size())
        return false;
    return nodes[node].index_of_neighbor(SIZE_MAX) != SIZE_MAX;
}

bool beyond_blossom::BackendUserGraph::get_edge_or_boundary_edge_weight(size_t node1, size_t node2, double& weight_out) {
    if (node1 >= nodes.size()) {
        return false;
    }
    size_t neighbor_idx = nodes[node1].index_of_neighbor(node2);
    if (neighbor_idx == SIZE_MAX) {
        return false;
    }
    weight_out = nodes[node1].neighbors[neighbor_idx].edge_it->weight;
    return true;
}

void beyond_blossom::BackendUserGraph::set_min_num_observables(size_t num_observables) {
    if (num_observables > _num_observables) {
        _num_observables = num_observables;
        _mwpm_needs_updating = true;
        _structured_mwpm_needs_updating = true;
    }
}

double beyond_blossom::BackendUserGraph::get_edge_weight_normalising_constant(size_t max_num_distinct_weights) {
    double max_abs_weight = 0;
    bool all_integral_weight = true;
    for (auto& e : edges) {
        if (std::abs(e.weight) > max_abs_weight)
            max_abs_weight = std::abs(e.weight);

        if (round(e.weight) != e.weight) {
            all_integral_weight = false;
        }

        for (auto implied : e.implied_weights_for_other_edges) {
            if (std::abs(implied.implied_weight) > max_abs_weight) {
                max_abs_weight = std::abs(implied.implied_weight);
            }

            if (round(implied.implied_weight) != implied.implied_weight) {
                all_integral_weight = false;
            }

            double current_weight;
            bool has_edge = get_edge_or_boundary_edge_weight(implied.node1, implied.node2, current_weight);
            if (!has_edge) {
                throw std::invalid_argument(
                    "Edge rewrite rule refers to non-existent edge (" + std::to_string(implied.node1) + ", " +
                    std::to_string(implied.node2) + ")");
            }
            bool same_sign = (current_weight * implied.implied_weight) >= 0.;
            if (!same_sign) {
                throw std::invalid_argument(
                    "Edge weight rewrite rules that change the sign of an edge weight are not currently supported.");
            }
        }
    }

    if (max_abs_weight > beyond_blossom::MAX_USER_EDGE_WEIGHT)
        throw std::invalid_argument(
            "maximum absolute edge weight of " + std::to_string(beyond_blossom::MAX_USER_EDGE_WEIGHT) + " exceeded.");

    if (all_integral_weight) {
        return 1.0;
    } else {
        beyond_blossom::weight_int max_half_edge_weight = max_num_distinct_weights - 1;
        return (double)max_half_edge_weight / max_abs_weight;
    }
}

beyond_blossom::BackendUserGraph beyond_blossom::detector_error_model_to_backend_user_graph(
    const stim::DetectorErrorModel& detector_error_model,
    const bool enable_correlations,
    beyond_blossom::weight_int num_distinct_weights) {
    beyond_blossom::BackendUserGraph user_graph(
        detector_error_model.count_detectors(), detector_error_model.count_observables(), num_distinct_weights);
    std::map<std::pair<size_t, size_t>, std::map<std::pair<size_t, size_t>, double>> joint_probabilites;
    if (enable_correlations) {
        beyond_blossom::iter_dem_instructions_include_correlations(
            detector_error_model,
            [&](double p, const std::vector<size_t>& detectors, std::vector<size_t>& observables) {
                user_graph.handle_dem_instruction_include_correlations(p, detectors, observables);
            },
            joint_probabilites);

        user_graph.populate_implied_edge_weights(joint_probabilites);
    } else {
        beyond_blossom::iter_detector_error_model_edges(
            detector_error_model,
            [&](double p, const std::vector<size_t>& detectors, std::vector<size_t>& observables) {
                user_graph.handle_dem_instruction(p, detectors, observables);
            });
        user_graph.loaded_from_dem_without_correlations = true;
    }
    return user_graph;
}

void beyond_blossom::BackendUserGraph::populate_implied_edge_weights(
    std::map<std::pair<size_t, size_t>, std::map<std::pair<size_t, size_t>, double>>& joint_probabilites) {
    _mwpm_needs_updating = true;
    _structured_mwpm_needs_updating = true;
    for (auto& edge : edges) {
        std::pair<size_t, size_t> current_edge_nodes = std::minmax(edge.node1, edge.node2);
        auto it = joint_probabilites.find(current_edge_nodes);
        if (it != joint_probabilites.end()) {
            const auto& pf = *it;
            std::pair<size_t, size_t> causal_edge = pf.first;
            double marginal_probability = pf.second.at(causal_edge);
            if (marginal_probability == 0)
                continue;

            for (const auto& affected_edge_and_probability : pf.second) {
                std::pair<size_t, size_t> affected_edge = affected_edge_and_probability.first;
                if (affected_edge != causal_edge) {
                    // Since edge weights are computed as std::log((1-p)/p), a probability of more than 0.5 for an
                    // error, would lead to a negatively weighted error. We do not support this (yet), and use a
                    // minimum of 0.5 as an implied probability for an edge to be reweighted.
                    double implied_probability_for_other_edge =
                        std::min(0.5, affected_edge_and_probability.second / marginal_probability);
                    double w = beyond_blossom::to_weight_for_correlations(implied_probability_for_other_edge);
                    ImpliedWeightUnconverted implied{affected_edge.first, affected_edge.second, w};
                    edge.implied_weights_for_other_edges.push_back(implied);
                }
            }
        }
    }
}
