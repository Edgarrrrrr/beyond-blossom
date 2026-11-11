// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/structured/decoder.h"

#include <stdexcept>
#include "beyond_blossom/driver/backend_user_graph.h"

namespace beyond_blossom::structured {
namespace {
beyond_blossom::DecoderOptions make_options(const stim::DetectorErrorModel& model, const Options& options) {
    if (options.distance == 0 || options.rounds == 0)
        throw std::invalid_argument("Structured decoding requires positive distance and rounds.");
    if (options.strategy == DecoderStrategy::Unstructured)
        throw std::invalid_argument("Structured Decoder requires a structured strategy.");
    beyond_blossom::DecoderOptions result;
    result.strategy = options.strategy;
    result.grid = static_grid_spec_from_dem(model, options.distance, options.rounds, options.rotated, options.simd);
    result.cpus = options.cpus;
    result.same_algorithm_single_thread = options.same_algorithm_single_thread;
    result.ensure_search_graph_included = options.include_search_graph;
    result.num_distinct_weights = options.num_distinct_weights;
    return result;
}
}  // namespace

Decoder::Decoder(const stim::DetectorErrorModel& model, const Options& options)
    : decoder_(model, make_options(model, options)) {}

}  // namespace beyond_blossom::structured
