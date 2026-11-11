// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef PYMATCHING_BACKEND_H
#define PYMATCHING_BACKEND_H

#include "pymatching/sparse_blossom/driver/backend_user_graph.h"
#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"

namespace pm {

// Backend selection is performed by the API before entering a decoding batch.
// There is deliberately no primary implementation or runtime backend state.
enum class BackendKind { Generic, Structured };

template <BackendKind Kind>
struct Backend;

template <>
struct Backend<BackendKind::Generic> {
    using Mwpm = pm::Mwpm;
    using MatchingGraph = pm::MatchingGraph;
    using SearchGraph = pm::SearchGraph;
    using DetectorNode = pm::DetectorNode;
    using SearchDetectorNode = pm::SearchDetectorNode;
    using GraphFlooder = pm::GraphFlooder;
    using SearchFlooder = pm::SearchFlooder;

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
        pm::decode_detection_events(mwpm, events, observables, weight, correlations);
    }
    // The API validates correlations once before entering a batch.
    static void decode_detection_events_validated(
        Mwpm& mwpm,
        const std::vector<uint64_t>& events,
        uint8_t* observables,
        total_weight_int& weight,
        bool correlations) {
        pm::decode_detection_events(mwpm, events, observables, weight, correlations);
    }
    static void decode_detection_events_to_match_edges(Mwpm& mwpm, const std::vector<uint64_t>& events) {
        pm::decode_detection_events_to_match_edges(mwpm, events);
    }
    static void decode_detection_events_to_edges(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        pm::decode_detection_events_to_edges(mwpm, events, edges);
    }
    static void decode_detection_events_to_edges_with_edge_correlations(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        pm::decode_detection_events_to_edges_with_edge_correlations(mwpm, events, edges);
    }
};

template <>
struct Backend<BackendKind::Structured> {
    using Mwpm = pm::structured::ComponentDecoder;
    using MatchingGraph = pm::structured::MatchingGraph;
    using SearchGraph = pm::structured::SearchGraph;
    using DetectorNode = pm::structured::DetectorNode;
    using SearchDetectorNode = pm::structured::SearchDetectorNode;
    using GraphFlooder = pm::structured::GraphFlooder;
    using SearchFlooder = pm::structured::SearchFlooder;

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
        pm::structured::decode_detection_events_to_match_edges(mwpm, events);
    }
    static void decode_detection_events_to_edges(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        pm::structured::decode_detection_events_to_edges(mwpm, events, edges);
    }
    static void decode_detection_events_to_edges_with_edge_correlations(
        Mwpm& mwpm, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        pm::structured::decode_detection_events_to_edges_with_edge_correlations(mwpm, events, edges);
    }
};

}  // namespace pm

#endif  // PYMATCHING_BACKEND_H
