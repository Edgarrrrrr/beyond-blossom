// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef BB_BACKEND_H
#define BB_BACKEND_H

#include "beyond_blossom/driver/backend_user_graph.h"
#include "beyond_blossom/driver/mwpm_decoding.h"
#include "beyond_blossom/structured/driver/mwpm_decoding.h"

namespace beyond_blossom {

// Backend selection is performed by the API before entering a decoding batch.
// There is deliberately no primary implementation or runtime backend state.
enum class BackendKind { Generic, Structured };

template <BackendKind Kind>
struct Backend;

template <>
struct Backend<BackendKind::Generic> {
    using Mwpm = beyond_blossom::Mwpm;
    using MatchingGraph = beyond_blossom::MatchingGraph;
    using SearchGraph = beyond_blossom::SearchGraph;
    using DetectorNode = beyond_blossom::DetectorNode;
    using SearchDetectorNode = beyond_blossom::SearchDetectorNode;
    using GraphFlooder = beyond_blossom::GraphFlooder;
    using SearchFlooder = beyond_blossom::SearchFlooder;

    static void validate_correlations(bool) {
    }
    static double normalising_constant(const Mwpm& decoder) {
        return decoder.flooder.graph.normalising_constant;
    }
    static size_t match_edge_count(const Mwpm& decoder) {
        return decoder.flooder.match_edges.size();
    }
    static std::pair<int64_t, int64_t> match_edge(const Mwpm& decoder, size_t index) {
        const auto& edge = decoder.flooder.match_edges[index];
        const auto* nodes = decoder.flooder.graph.nodes.data();
        return {edge.loc_from - nodes, edge.loc_to ? edge.loc_to - nodes : -1};
    }
    static Mwpm& get_mwpm(BackendUserGraph& graph) {
        return graph.get_mwpm();
    }
    static Mwpm& get_mwpm_with_search_graph(BackendUserGraph& graph) {
        return graph.get_mwpm_with_search_graph();
    }
    static size_t node_id(const MatchingGraph& graph, const DetectorNode* node) {
        return node - graph.nodes.data();
    }
    static size_t node_id(const SearchGraph& graph, const SearchDetectorNode* node) {
        return node - graph.nodes.data();
    }
    static void decode_detection_events(
        Mwpm& mwpm,
        const std::vector<uint64_t>& events,
        uint8_t* observables,
        total_weight_int& weight,
        bool correlations) {
        beyond_blossom::decode_detection_events(mwpm, events, observables, weight, correlations);
    }
    // The API validates correlations once before entering a batch.
    static void decode_detection_events_validated(
        Mwpm& mwpm,
        const std::vector<uint64_t>& events,
        uint8_t* observables,
        total_weight_int& weight,
        bool correlations) {
        beyond_blossom::decode_detection_events(mwpm, events, observables, weight, correlations);
    }
    static void decode_detection_events_to_match_edges(Mwpm& mwpm, const std::vector<uint64_t>& events) {
        beyond_blossom::decode_detection_events_to_match_edges(mwpm, events);
    }
    static void decode_detection_events_to_edges(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        beyond_blossom::decode_detection_events_to_edges(mwpm, events, edges);
    }
    static void decode_detection_events_to_edges_with_edge_correlations(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        beyond_blossom::decode_detection_events_to_edges_with_edge_correlations(mwpm, events, edges);
    }
};

template <>
struct Backend<BackendKind::Structured> {
    using Mwpm = beyond_blossom::structured::ComponentDecoder;
    using MatchingGraph = beyond_blossom::structured::MatchingGraph;
    using SearchGraph = beyond_blossom::structured::SearchGraph;
    using DetectorNode = beyond_blossom::structured::DetectorNode;
    using SearchDetectorNode = beyond_blossom::structured::SearchDetectorNode;
    using GraphFlooder = beyond_blossom::structured::GraphFlooder;
    using SearchFlooder = beyond_blossom::structured::SearchFlooder;

    static void validate_correlations(bool enabled) {
        Mwpm::validate_correlations(enabled);
    }
    static double normalising_constant(const Mwpm& decoder) {
        return decoder.normalising_constant();
    }
    static size_t match_edge_count(const Mwpm& decoder) {
        return decoder.match_edges.size() / 2;
    }
    static std::pair<int64_t, int64_t> match_edge(const Mwpm& decoder, size_t index) {
        return {decoder.match_edges[2 * index], decoder.match_edges[2 * index + 1]};
    }
    static Mwpm& get_mwpm(BackendUserGraph& graph) {
        return graph.get_structured_mwpm();
    }
    static Mwpm& get_mwpm_with_search_graph(BackendUserGraph& graph) {
        return graph.get_structured_mwpm_with_search_graph();
    }
    static size_t node_id(const MatchingGraph& graph, const DetectorNode* node) {
        return graph.node_id(node);
    }
    static size_t node_id(const SearchGraph& graph, const SearchDetectorNode* node) {
        return graph.node_id(node);
    }
    static void decode_detection_events(
        Mwpm& mwpm,
        const std::vector<uint64_t>& events,
        uint8_t* observables,
        total_weight_int& weight,
        bool correlations) {
        validate_correlations(correlations);
        mwpm.decode(events, observables, weight);
    }
    // The API validates correlations once before entering a batch.
    static void decode_detection_events_validated(
        Mwpm& mwpm,
        const std::vector<uint64_t>& events,
        uint8_t* observables,
        total_weight_int& weight,
        bool correlations) {
        mwpm.decode(events, observables, weight);
    }
    static void decode_detection_events_to_match_edges(Mwpm& mwpm, const std::vector<uint64_t>& events) {
        beyond_blossom::structured::decode_detection_events_to_match_edges(mwpm, events);
    }
    static void decode_detection_events_to_edges(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        beyond_blossom::structured::decode_detection_events_to_edges(mwpm, events, edges);
    }
    static void decode_detection_events_to_edges_with_edge_correlations(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        beyond_blossom::structured::decode_detection_events_to_edges_with_edge_correlations(mwpm, events, edges);
    }
};

}  // namespace beyond_blossom

#endif  // BB_BACKEND_H
