// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.

#ifndef BB_STRUCTURED_PREDECODE_STATE_H
#define BB_STRUCTURED_PREDECODE_STATE_H

#include <cstdint>
#include <limits>

#include "beyond_blossom/core/ints.h"
#include "beyond_blossom/structured/predecode/predecode_diagnostics.h"

namespace beyond_blossom::structured {
class DetectorNode;
template <typename Node, typename Observable>
struct StaticGridComponent;

// A singleton dual region prepared before the MWPM arenas or queue are touched.
// Ordinary mate values index the same seed vector, not detector IDs or cells.
struct PredecodeSeed {
    static constexpr uint32_t UNMATCHED = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t BOUNDARY = UNMATCHED - 1;

    uint32_t detector_id;
    uint32_t cell;
    uint32_t mate = UNMATCHED;
    weight_int radius = 0;
    uint8_t match_slot = 0;
#if BB_PREDECODE_CAP_CACHE
    // Finite saturation persists until this seed is rebuilt for a new shot.
    bool cap_saturated = false;
#endif
#if BB_STRUCTURED_PROFILE_COUNTERS
    // Diagnostic-only: an earlier grow executed a finite cap scan this shot.
    bool profile_finite_grow = false;
#endif
    StaticGridComponent<DetectorNode, obs_int>* grid = nullptr;
};

}  // namespace beyond_blossom::structured
#endif
