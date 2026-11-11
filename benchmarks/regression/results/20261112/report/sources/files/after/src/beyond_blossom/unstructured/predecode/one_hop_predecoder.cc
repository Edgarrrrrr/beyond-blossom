// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/unstructured/predecode/one_hop_predecoder.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace beyond_blossom {

void OneHopPredecoder::reset() noexcept {
    for (const auto& seed : seeds)
        active[size_t(seed.node - nodes)] = 0;
    seeds.clear();
    candidates.clear();
    complete = result_pending = skipped = false;
    obs_mask = 0;
    weight = 0;
}

void OneHopPredecoder::prepare(MatchingGraph& graph, const std::vector<uint64_t>& events,
                              const std::vector<uint64_t>& negative_events) {
    reset();
    active.resize(graph.nodes.size(), 0);
    nodes = graph.nodes.data();
    const auto is_boundary = [&](size_t index) {
        return index < graph.is_user_graph_boundary_node.size() && graph.is_user_graph_boundary_node[index];
    };
    try {
        // Validate before touching any flooder node or arena. The sparse index
        // is also the duplicate detector, and is cleared on every error path.
        for (auto index : events) {
            if (index >= graph.nodes.size())
                throw std::invalid_argument("Detection event index exceeds the number of graph nodes.");
            if (is_boundary(index))
                continue;
            if (active[index])
                throw std::invalid_argument("Duplicate detection event in predecoder input.");
            seeds.push_back({nodes + index});
            active[index] = seeds.size();
        }
        // Negative edges are already stored at their absolute weight. Toggle
        // their endpoints to reduce the shot to the same nonnegative T-join.
        for (auto index : negative_events) {
            if (is_boundary(index))
                continue;
            if (auto slot = active[index]) {
                seeds[slot - 1] = seeds.back();
                active[size_t(seeds[slot - 1].node - nodes)] = slot;
                seeds.pop_back();
                active[index] = 0;
            } else {
                seeds.push_back({nodes + index});
                active[index] = seeds.size();
            }
        }
    } catch (...) {
        reset();
        throw;
    }
}

void OneHopPredecoder::grow(size_t index) {
    auto& seed = seeds[index];
    if (seed.saturated)
        return;
    const auto& node = *seed.node;
    weight_int limit = seed.base_cap;
    const size_t end = index + 1 < seeds.size() ? seeds[index + 1].candidate_begin : candidates.size();
    for (size_t position = seed.candidate_begin; position < end; ++position) {
        const auto& candidate = candidates[position];
        const auto other_radius = seeds[candidate.seed].radius;
        assert(other_radius <= node.neighbor_weights[candidate.slot]);
        limit = std::min(limit, node.neighbor_weights[candidate.slot] - other_radius);
    }
    if (!node.neighbors.empty()) {
        // The full solver's two-growing-front collision times use an even
        // radius lattice. UserGraph quantizes all edge weights onto it.
        seed.radius = limit & ~weight_int{1};
        seed.saturated = true;
    }
}

void OneHopPredecoder::run_one(size_t index) {
    auto& seed = seeds[index];
    const auto& node = *seed.node;
    grow(index);
    // Try detector partners first, preserving the structured decoder's local
    // preference without relying on a fixed stencil or a degree limit.
    const size_t end = index + 1 < seeds.size() ? seeds[index + 1].candidate_begin : candidates.size();
    for (size_t position = seed.candidate_begin; position < end; ++position) {
        const auto& candidate = candidates[position];
        const auto slot = candidate.slot;
        if (seeds[candidate.seed].mate != UNMATCHED)
            continue;
        auto& mate = seeds[candidate.seed];
        if (uint64_t(seed.radius) + mate.radius < node.neighbor_weights[slot])
            grow(candidate.seed);
        if (uint64_t(seed.radius) + mate.radius != node.neighbor_weights[slot])
            continue;
        seed.mate = candidate.seed;
        mate.mate = index;
        seed.match_observables = mate.match_observables = node.neighbor_observables[slot];
        // Both endpoints retain the selected edge summary, avoiding a reverse
        // adjacency lookup when importing or extracting the provisional pair.
        obs_mask ^= node.neighbor_observables[slot];
        weight += node.neighbor_weights[slot];
        return;
    }
    if (!node.neighbors.empty() && !node.neighbors[0] && seed.radius == node.neighbor_weights[0]) {
        seed.mate = BOUNDARY;
        seed.match_observables = node.neighbor_observables[0];
        obs_mask ^= node.neighbor_observables[0];
        weight += node.neighbor_weights[0];
    }
}

bool OneHopPredecoder::should_skip_sparse_high_degree() const noexcept {
    // In a sparse high-degree neighborhood the cap/candidate scan duplicates
    // substantial flooder work, while few local pairs become available. Bound
    // the dispatch cost before that scan: inspect seed degrees, then at most
    // 8 * 32 evenly spaced adjacency entries. This is a performance heuristic;
    // the fallback is the full solver on the same normalized syndrome.
    if (seeds.empty())
        return false;
    size_t degree_sum = 0;
    for (const auto& seed : seeds) {
        const auto& neighbors = seed.node->neighbors;
        // A direct boundary can finish a seed even without an active neighbor.
        if (!neighbors.empty() && !neighbors[0])
            return false;
        degree_sum += neighbors.size();
    }
    if (degree_sum / seeds.size() < 32)
        return false;
    size_t sampled = 0, active_neighbors = 0;
    // Short shots should not pay the maximum fixed probe cost. Inspect at
    // most one neighborhood per four seeds, with a nonzero rounded-up count.
    const size_t seed_samples = std::min(size_t{8}, (seeds.size() - 1) / 4 + 1);
    const size_t seed_step = seeds.size() / seed_samples;
    const size_t seed_remainder = seeds.size() % seed_samples;
    size_t seed_position = 0, seed_error = 0;
    for (size_t sample = 0; sample < seed_samples; ++sample) {
        const auto& neighbors = seeds[seed_position].node->neighbors;
        if (neighbors.size() >= 32) {
            for (size_t position = 0; position < 32; ++position)
                active_neighbors += seed_index(neighbors[(position * neighbors.size()) >> 5]) != 0;
            sampled += 32;
        } else {
            for (const auto* neighbor : neighbors)
                active_neighbors += seed_index(neighbor) != 0;
            sampled += neighbors.size();
        }
        // Exactly floor((sample + 1) * seeds.size() / seed_samples), without
        // a variable integer division on each sampled neighborhood.
        seed_position += seed_step;
        seed_error += seed_remainder;
        if (seed_error >= seed_samples) {
            ++seed_position;
            seed_error -= seed_samples;
        }
    }
    // Local density preserves dense active components even when the graph has
    // many disconnected or irrelevant vertices. No graph-level cache is used.
    return sampled && active_neighbors * 8 < sampled;
}

void OneHopPredecoder::run() {
    skipped = should_skip_sparse_high_degree();
    if (skipped)
        return;
    // Put syndrome leaves first. Three bounded buckets avoid comparison sorts
    // and preserve linear work even on arbitrary high-degree graphs.
    // The immutable nonseed constraints enter through the minimum edge weight.
    // Only active seed neighbors can tighten this cap as their radii increase.
    // Record those neighbors once instead of rescanning arbitrary adjacency
    // lists separately for degree counting, radius growth, and matching.
    for (auto& seed : seeds) {
        seed.candidate_begin = candidates.size();
        seed.base_cap = std::numeric_limits<weight_int>::max();
        const auto& node = *seed.node;
        bool boundary = false;
        for (size_t slot = 0; slot < node.neighbors.size(); ++slot) {
            seed.base_cap = std::min(seed.base_cap, node.neighbor_weights[slot]);
            if (const auto* neighbor = node.neighbors[slot]) {
                if (const auto other = seed_index(neighbor))
                    candidates.push_back({other - 1, slot});
            } else {
                boundary = true;
            }
        }
        seed.degree = uint8_t(std::min(size_t{2}, candidates.size() - seed.candidate_begin + boundary));
    }
    for (uint8_t degree : {uint8_t{1}, uint8_t{2}, uint8_t{0}})
        for (size_t index = 0; index < seeds.size(); ++index)
            if (seeds[index].degree == degree && seeds[index].mate == UNMATCHED)
                run_one(index);
    complete = std::all_of(seeds.begin(), seeds.end(), [](const Seed& seed) { return seed.mate != UNMATCHED; });
    result_pending = complete;
}

void OneHopPredecoder::append_match_edges(std::vector<CompressedEdge>& edges) const {
    for (size_t index = 0; index < seeds.size(); ++index) {
        const auto& seed = seeds[index];
        if (seed.mate == UNMATCHED)
            continue;
        if (seed.mate == BOUNDARY || index < seed.mate) {
            edges.push_back({seed.node, seed.mate == BOUNDARY ? nullptr : seeds[seed.mate].node,
                             seed.match_observables});
        }
    }
}

}  // namespace beyond_blossom
