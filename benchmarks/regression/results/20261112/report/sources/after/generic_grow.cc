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
