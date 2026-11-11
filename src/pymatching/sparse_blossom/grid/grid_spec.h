// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef PYMATCHING_GRID_SPEC_H
#define PYMATCHING_GRID_SPEC_H

#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "pymatching/sparse_blossom/driver/implied_weights.h"
#include "pymatching/sparse_blossom/flooder_matcher_interop/varying.h"
#include "pymatching/sparse_blossom/grid/grid_collision_state.h"

namespace pm {

enum class GridNodeKind : uint8_t { absent, detector, boundary };
enum class GridSimd : uint8_t { automatic, scalar, avx512, avx2, avx512vl };

struct StaticGridSpec {
    size_t distance;
    size_t rounds;
    bool rotated;
    std::vector<std::array<int64_t, 3>> coordinates;
    // Empty labels infer ordinary-edge components.
    std::vector<uint32_t> groups;
    GridSimd simd = GridSimd::automatic;
};

inline constexpr size_t GRID_DIRECTIONS = 16;

struct alignas(64) GridWeightRow {
    std::array<weight_int, GRID_DIRECTIONS> values{};
    weight_int& operator[](size_t d) {
        return values[d];
    }
    const weight_int& operator[](size_t d) const {
        return values[d];
    }
    const weight_int* data() const {
        return values.data();
    }
};

static_assert(sizeof(GridWeightRow) == 64);
static_assert(alignof(GridWeightRow) == 64);

inline constexpr std::array<std::array<int, 3>, GRID_DIRECTIONS> SURFACE_OFFSETS{
    {{0, 0, 1},
     {0, 0, -1},
     {1, 0, 0},
     {-1, 0, 0},
     {0, 1, 0},
     {0, -1, 0},
     {-1, 0, 1},
     {1, 0, -1},
     {0, -1, 1},
     {0, 1, -1},
     {-1, 1, 1},
     {1, -1, -1},
     {-1, -1, 1},
     {1, 1, -1},
     {0, 1, 1},
     {0, -1, -1}}};

template <size_t NX, size_t NY, size_t NT>
struct StaticGridBox {
    static_assert(NX > 0 && NY > 0 && NT > 0);
    static_assert(NX <= PTRDIFF_MAX / NY && NX * NY <= PTRDIFF_MAX / NT);
    static constexpr size_t nx = NX, ny = NY, nt = NT, volume = NX * NY * NT;

    template <size_t D>
    static constexpr ptrdiff_t offset() {
        static_assert(D < GRID_DIRECTIONS);
        return SURFACE_OFFSETS[D][0] + ptrdiff_t(NX) * SURFACE_OFFSETS[D][1] +
               ptrdiff_t(NX * NY) * SURFACE_OFFSETS[D][2];
    }
};

using GridNextEvent = std::pair<size_t, cumulative_time_int>;
inline constexpr auto GRID_NO_TIME = std::numeric_limits<cumulative_time_int>::max();
bool grid_avx512_available();

inline bool grid_avx2_available() {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

inline bool grid_avx512vl_available() {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("avx512f") &&
           __builtin_cpu_supports("avx512vl");
#else
    return false;
#endif
}

}  // namespace pm
#endif
