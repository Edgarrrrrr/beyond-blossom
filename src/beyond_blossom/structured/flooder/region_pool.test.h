// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_REGION_POOL_TEST_H
#define BB_STRUCTURED_REGION_POOL_TEST_H
#include "beyond_blossom/structured/flooder/region_pool.h"

namespace beyond_blossom::structured {
// White-box lifetime checks; no production counters or inspection branches.
struct RegionPoolTestAccess {
    static uint8_t state(const RegionPool& pool, RegionId id) {
        return pool.states_[id];
    }
    static RegionId lifetime_end(const RegionPool& pool) {
        return pool.lifetime_end_;
    }
    static RegionId next(const RegionPool& pool) {
        return pool.next_;
    }
    static void set_generation(RegionPool& pool, RegionId id, RegionGeneration generation) {
        pool.generations_[id] = generation;
    }
    // Inspect retained lifetimes without operator[] materializing a lazy seed.
    static const GraphFillRegion* physical_object(const RegionPool& pool, RegionId id) {
        return RegionPool::has_object(pool.states_[id]) ? &pool.regions_[id] : nullptr;
    }
    static const RegionPool::SeedExtraction& retired_seed_payload(const RegionPool& pool, RegionId id) {
        return pool.seed_extraction_[id];
    }
    static bool compact_endpoints(const RegionPool& pool, RegionId id) {
        return (pool.seed_node_flags_[id] & RegionPool::compact_endpoints) != 0;
    }
};
}  // namespace beyond_blossom::structured
#endif
