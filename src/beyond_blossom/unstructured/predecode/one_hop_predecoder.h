// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef BEYOND_BLOSSOM_UNSTRUCTURED_ONE_HOP_PREDECODER_H
#define BEYOND_BLOSSOM_UNSTRUCTURED_ONE_HOP_PREDECODER_H

#include "beyond_blossom/unstructured/flooder/graph.h"
#include "beyond_blossom/unstructured/interop/compressed_edge.h"

namespace beyond_blossom {

// A dual-feasible warm start for arbitrary adjacency lists. Provisional matches
// remain available to alternating trees: removing them from an incomplete shot
// would be an unsafe greedy decoder. Only a complete tight matching is final.
class OneHopPredecoder {
   public:
    static constexpr size_t UNMATCHED = SIZE_MAX;
    static constexpr size_t BOUNDARY = SIZE_MAX - 1;
    struct Seed {
        DetectorNode* node;
        size_t mate = UNMATCHED;
        obs_int match_observables = 0;
        size_t candidate_begin = 0;
        weight_int radius = 0;
        weight_int base_cap = 0;
        uint8_t degree = 0;
        bool saturated = false;
    };
    std::vector<Seed> seeds;
    bool complete = false;
    bool result_pending = false;
    obs_int obs_mask = 0;
    total_weight_int weight = 0;

    void prepare(MatchingGraph& graph, const std::vector<uint64_t>& events,
                 const std::vector<uint64_t>& negative_events);
    void run();
    void reset() noexcept;
    void append_match_edges(std::vector<CompressedEdge>& edges) const;

   private:
    struct Candidate {
        size_t seed;
        size_t slot;
    };
    std::vector<size_t> active;
    std::vector<Candidate> candidates;
    DetectorNode* nodes = nullptr;
    size_t seed_index(const DetectorNode* node) const noexcept;
    void grow(size_t index);
    void run_one(size_t index);
};

}  // namespace beyond_blossom
#endif
