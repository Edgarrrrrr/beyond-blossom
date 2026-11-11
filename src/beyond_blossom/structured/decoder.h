// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_DECODER_H
#define BB_STRUCTURED_DECODER_H

#include "beyond_blossom/driver/decoder_strategy.h"

namespace beyond_blossom::structured {

// Geometry is explicit. Detector coordinates are read from the error model.
struct Options {
    size_t distance = 0;
    size_t rounds = 0;
    bool rotated = true;
    GridSimd simd = GridSimd::automatic;
    DecoderStrategy strategy = DecoderStrategy::Predecoded;
    std::vector<int> cpus;
    bool same_algorithm_single_thread = false;
    bool include_search_graph = false;
    weight_int num_distinct_weights = NUM_DISTINCT_WEIGHTS;
};

// Owns immutable topology, reusable shot state and any persistent worker team.
// Each instance accepts one caller at a time. Parallel scalar calls belong in
// run_with_team; decode_batch creates one team for the complete batch.
class Decoder {
    StrategyDecoder decoder_;

   public:
    Decoder(const stim::DetectorErrorModel& model, const Options& options);
    const DecoderMetadata& metadata() const { return decoder_.metadata(); }
    MatchingResult decode(const std::vector<uint64_t>& events) { return decoder_.decode(events); }
    void decode(const std::vector<uint64_t>& events, uint8_t* observables, total_weight_int& weight) {
        decoder_.decode(events, observables, weight);
    }
    void decode_to_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        decoder_.decode_to_edges(events, edges);
    }
    void decode_to_match_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
        decoder_.decode_to_match_edges(events, edges);
    }
    void run_with_team(const std::function<void(Decoder&)>& callback) {
        decoder_.run_with_team([&](StrategyDecoder&) { callback(*this); });
    }
    std::vector<MatchingResult> decode_batch(std::span<const std::vector<uint64_t>> shots) {
        return decoder_.decode_batch(shots);
    }
    void drain() { decoder_.drain(); }
};

}  // namespace beyond_blossom::structured
#endif
