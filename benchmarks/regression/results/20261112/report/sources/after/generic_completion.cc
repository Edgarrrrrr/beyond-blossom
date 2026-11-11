    complete = std::all_of(seeds.begin(), seeds.end(), [](const Seed& seed) { return seed.mate != UNMATCHED; });
    result_pending = complete;
