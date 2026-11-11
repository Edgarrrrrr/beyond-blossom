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
