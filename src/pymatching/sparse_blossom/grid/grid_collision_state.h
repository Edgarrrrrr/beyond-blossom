// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef PYMATCHING_GRID_COLLISION_STATE_H
#define PYMATCHING_GRID_COLLISION_STATE_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pm {

// One authoritative collision state per lattice cell. Empty and boundary cells
// use id zero; changing a region's growth rate never writes this array.
struct alignas(8) GridCollisionState {
    int32_t wrapped_radius_cached = 0;
    uint32_t top_region_id = 0;
};

static_assert(std::is_trivially_copyable_v<GridCollisionState>);
static_assert(std::is_standard_layout_v<GridCollisionState>);
static_assert(sizeof(GridCollisionState) == 8);
static_assert(offsetof(GridCollisionState, top_region_id) == 4);

}  // namespace pm
#endif
