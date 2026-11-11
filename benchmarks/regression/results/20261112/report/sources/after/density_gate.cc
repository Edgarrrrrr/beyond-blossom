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
