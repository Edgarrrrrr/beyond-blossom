// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_STATIC_GRID_SIMD_KERNELS_H
#define PYMATCHING_STRUCTURED_STATIC_GRID_SIMD_KERNELS_H

#include <algorithm>
#include <bit>

#include "pymatching/sparse_blossom/structured/grid/static_grid_kernels.h"

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define PM_GRID_AVX2 1
#define PM_GRID_AVX512VL 1
#define PM_GRID_AVX2_TARGET __attribute__((target("avx2")))
#define PM_GRID_AVX512VL_TARGET __attribute__((target("avx2,avx512f,avx512vl")))
#define PM_GRID_SIMD_INLINE inline __attribute__((always_inline))
#else
#define PM_GRID_AVX2 0
#define PM_GRID_AVX512VL 0
#endif

namespace pm::structured {

// The default supports synthetic, unpadded and poisoned inactive test cells.
// Constructed is only for padded production grids whose every stored ID is valid.
enum class GridCellAccess { masked, constructed };

#if PM_GRID_AVX2
// A disabled lane uses offset zero, so even a synthetic source at cell zero
// never forms a pointer before the array. Consumers retain their present masks;
// the copied source state cannot turn a missing edge into a collision. Use a
// bitwise offset mask instead of twelve conditional memory-read branches.
template <typename Box, typename Stencil, size_t Slot, size_t Lane, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_SIMD_INLINE int64_t grid_load_collision_lane(const GridCollisionState* states, unsigned present) {
    if constexpr (Slot < STRUCTURED_STORAGE_DIRECTIONS) {
        if constexpr (CircuitStorage<Stencil>::mask & (uint16_t{1} << Slot)) {
            constexpr ptrdiff_t offset = Box::template offset<CircuitStorage<Stencil>::template direction<Slot>>();
            if constexpr (Access == GridCellAccess::constructed) {
                return std::bit_cast<int64_t>(states[offset]);
            } else {
                const ptrdiff_t enabled = -ptrdiff_t((present >> Lane) & 1);
                return std::bit_cast<int64_t>(states[offset & enabled]);
            }
        }
    }
    return 0;
}

// AVX2 has neither signed 64-bit shifts nor signed 64-bit minimum. These
// operations use only AVX2 instructions, including for negative odd values.
template <unsigned Bits>
PM_GRID_AVX2_TARGET PM_GRID_SIMD_INLINE __m256i grid_avx2_srai_epi64(__m256i value) {
    static_assert(Bits > 0 && Bits < 64);
    const auto bias = _mm256_set1_epi64x(int64_t{1} << (63 - Bits));
    return _mm256_sub_epi64(_mm256_xor_si256(_mm256_srli_epi64(value, Bits), bias), bias);
}

PM_GRID_AVX2_TARGET PM_GRID_SIMD_INLINE __m256i grid_avx2_min_epi64(__m256i a, __m256i b) {
    return _mm256_blendv_epi8(a, b, _mm256_cmpgt_epi64(a, b));
}

template <GridSourceGrowth Growth>
struct GridAvx2Source;

template <>
struct GridAvx2Source<GridSourceGrowth::growing> {
    PM_GRID_AVX2_TARGET static PM_GRID_SIMD_INLINE __m256i valid(__m256i region) {
        return _mm256_cmpeq_epi64(
            _mm256_and_si256(region, _mm256_set1_epi64x(2)), _mm256_setzero_si256());
    }
    PM_GRID_AVX2_TARGET static PM_GRID_SIMD_INLINE __m256i time(__m256i raw, __m256i region) {
        auto growing = _mm256_cmpeq_epi64(
            _mm256_and_si256(region, _mm256_set1_epi64x(1)), _mm256_set1_epi64x(1));
        return _mm256_blendv_epi8(raw, grid_avx2_srai_epi64<1>(raw), growing);
    }
};

template <>
struct GridAvx2Source<GridSourceGrowth::frozen> {
    PM_GRID_AVX2_TARGET static PM_GRID_SIMD_INLINE __m256i valid(__m256i region) {
        return _mm256_cmpeq_epi64(
            _mm256_and_si256(region, _mm256_set1_epi64x(1)), _mm256_set1_epi64x(1));
    }
    PM_GRID_AVX2_TARGET static PM_GRID_SIMD_INLINE __m256i time(__m256i raw, __m256i) {
        return raw;
    }
};

struct GridLoadedCollision {
    int64_t state;
    int64_t region;
};

// Keep both dependent loads in scalar registers, then pack their results. The
// state loader maps a missing lane onto the source, whose radius is already
// known valid at entry. Uninstantiated padding lanes use the reserved radius[0].
// The original SIMD present/owner masks still decide whether a lane can win.
template <typename Box, typename Stencil, size_t Slot, size_t Lane, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_SIMD_INLINE GridLoadedCollision grid_load_collision_and_radius(
    const GridCollisionState* states, unsigned present, const VaryingCT* radii, uint32_t source_id) {
    const auto state = grid_load_collision_lane<Box, Stencil, Slot, Lane, Access>(states, present);
    const uint32_t id = uint32_t(std::bit_cast<uint64_t>(state) >> 32);
    return {state, radii[id].data};
}

template <typename Box, typename Stencil, GridSourceGrowth Growth, size_t First, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX2_TARGET PM_GRID_SIMD_INLINE __m256i grid_matching_avx2_batch(
    const MatchingGridComponent& c,
    const DetectorNode& source,
    const VaryingCT* radii,
    __m256i source_intercept,
    __m256i source_id,
    uint32_t source_owner) {
    static_assert(First + 4 <= STRUCTURED_STORAGE_DIRECTIONS);
    static_assert(sizeof(VaryingCT) == sizeof(int64_t));
    static_assert(std::endian::native == std::endian::little);
    const unsigned present = (c.present[source.grid_cell] & CircuitStorage<Stencil>::mask) >> First & 15;
    if (!present)
        return _mm256_set1_epi64x(GRID_NO_TIME);
    const auto zero = _mm256_setzero_si256();
    auto mask = _mm256_cmpgt_epi64(
        _mm256_and_si256(_mm256_set1_epi64x(present), _mm256_setr_epi64x(1, 2, 4, 8)), zero);
    const auto* states = c.collision_states.data() + source.grid_cell;
    const auto s0 = grid_load_collision_and_radius<Box, Stencil, First + 0, 0, Access>(states, present, radii, source_owner);
    const auto s1 = grid_load_collision_and_radius<Box, Stencil, First + 1, 1, Access>(states, present, radii, source_owner);
    const auto s2 = grid_load_collision_and_radius<Box, Stencil, First + 2, 2, Access>(states, present, radii, source_owner);
    const auto s3 = grid_load_collision_and_radius<Box, Stencil, First + 3, 3, Access>(states, present, radii, source_owner);
    auto state = _mm256_setr_epi64x(s0.state, s1.state, s2.state, s3.state);
    auto region = _mm256_setr_epi64x(s0.region, s1.region, s2.region, s3.region);
    auto ids = _mm256_srli_epi64(state, 32);
    auto wrapped = grid_avx2_srai_epi64<32>(_mm256_slli_epi64(state, 32));
    auto different_owner = _mm256_andnot_si256(_mm256_cmpeq_epi64(ids, source_id), mask);
    auto valid = _mm256_and_si256(different_owner, GridAvx2Source<Growth>::valid(region));
    auto weights = _mm256_cvtepu32_epi64(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(c.weights[source.grid_cell].data() + First)));
    auto other_intercept = _mm256_add_epi64(grid_avx2_srai_epi64<2>(region), wrapped);
    auto raw = _mm256_sub_epi64(_mm256_sub_epi64(weights, source_intercept), other_intercept);
    auto times = GridAvx2Source<Growth>::time(raw, region);
    return _mm256_blendv_epi8(_mm256_set1_epi64x(GRID_NO_TIME), times, valid);
}

template <typename Box, typename Stencil, GridSourceGrowth Growth, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX2_TARGET PM_GRID_SIMD_INLINE GridNextEvent grid_matching_avx2_scan(
    const MatchingGridComponent& c,
    const DetectorNode& source,
    const VaryingCT* radii,
    cumulative_time_int source_intercept,
    uint32_t owner) {
    auto b1 = _mm256_set1_epi64x(source_intercept);
    auto source_id = _mm256_set1_epi64x(owner);
    auto a = grid_matching_avx2_batch<Box, Stencil, Growth, 0, Access>(c, source, radii, b1, source_id, owner);
    auto b = grid_matching_avx2_batch<Box, Stencil, Growth, 4, Access>(c, source, radii, b1, source_id, owner);
    auto d = grid_matching_avx2_batch<Box, Stencil, Growth, 8, Access>(c, source, radii, b1, source_id, owner);
    auto e = _mm256_set1_epi64x(GRID_NO_TIME);
    auto minimum = grid_avx2_min_epi64(grid_avx2_min_epi64(a, b), d);
    if constexpr (STRUCTURED_STORAGE_DIRECTIONS == 16) {
        e = grid_matching_avx2_batch<Box, Stencil, Growth, 12, Access>(c, source, radii, b1, source_id, owner);
        minimum = grid_avx2_min_epi64(minimum, e);
    }
    minimum = grid_avx2_min_epi64(minimum, _mm256_permute4x64_epi64(minimum, 0x4e));
    minimum = grid_avx2_min_epi64(minimum, _mm256_permute4x64_epi64(minimum, 0xb1));
    const auto time = _mm_cvtsi128_si64(_mm256_castsi256_si128(minimum));
    if (time == GRID_NO_TIME)
        return {SIZE_MAX, GRID_NO_TIME};
    unsigned winners = unsigned(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(a, minimum)))) |
                       (unsigned(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(b, minimum)))) << 4) |
                       (unsigned(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(d, minimum)))) << 8);
    if constexpr (STRUCTURED_STORAGE_DIRECTIONS == 16)
        winners |= unsigned(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(e, minimum)))) << 12;
    return {std::countr_zero(winners), time};
}

template <typename Box, typename Stencil, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX2_TARGET GridNextEvent grid_matching_avx2(
    const MatchingGridComponent& c, const DetectorNode& source, const VaryingCT* radii) {
    const auto& state = c.collision_states[source.grid_cell];
    const auto radius = radii[state.top_region_id];
    if (radius.is_shrinking())
        return {SIZE_MAX, GRID_NO_TIME};
    const auto source_intercept = radius.y_intercept() + state.wrapped_radius_cached;
    if (radius.is_growing())
        return grid_matching_avx2_scan<Box, Stencil, GridSourceGrowth::growing, Access>(
            c, source, radii, source_intercept, state.top_region_id);
    return grid_matching_avx2_scan<Box, Stencil, GridSourceGrowth::frozen, Access>(
        c, source, radii, source_intercept, state.top_region_id);
}
#endif

#if PM_GRID_AVX512VL
template <GridSourceGrowth Growth>
struct GridAvx512vlSource;

template <>
struct GridAvx512vlSource<GridSourceGrowth::growing> {
    PM_GRID_AVX512VL_TARGET static PM_GRID_SIMD_INLINE __mmask8 valid(__m256i region) {
        return _mm256_cmpeq_epi64_mask(
            _mm256_and_si256(region, _mm256_set1_epi64x(2)), _mm256_setzero_si256());
    }
    PM_GRID_AVX512VL_TARGET static PM_GRID_SIMD_INLINE __m256i time(__m256i raw, __m256i region) {
        return _mm256_srav_epi64(raw, _mm256_and_si256(region, _mm256_set1_epi64x(1)));
    }
};

template <>
struct GridAvx512vlSource<GridSourceGrowth::frozen> {
    PM_GRID_AVX512VL_TARGET static PM_GRID_SIMD_INLINE __mmask8 valid(__m256i region) {
        return _mm256_cmpeq_epi64_mask(
            _mm256_and_si256(region, _mm256_set1_epi64x(1)), _mm256_set1_epi64x(1));
    }
    PM_GRID_AVX512VL_TARGET static PM_GRID_SIMD_INLINE __m256i time(__m256i raw, __m256i) {
        return raw;
    }
};

template <typename Box, typename Stencil, GridSourceGrowth Growth, size_t First, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX512VL_TARGET PM_GRID_SIMD_INLINE __m256i grid_matching_avx512vl_batch(
    const MatchingGridComponent& c,
    const DetectorNode& source,
    const VaryingCT* radii,
    __m256i source_intercept,
    __m256i source_id,
    uint32_t source_owner) {
    static_assert(First < STRUCTURED_STORAGE_DIRECTIONS);
    static_assert(sizeof(VaryingCT) == sizeof(int64_t));
    static_assert(std::endian::native == std::endian::little);
    const __mmask8 present = (c.present[source.grid_cell] & CircuitStorage<Stencil>::mask) >> First & 15;
    if (!present)
        return _mm256_set1_epi64x(GRID_NO_TIME);
    auto zero = _mm256_setzero_si256();
    const auto* states = c.collision_states.data() + source.grid_cell;
    const auto s0 = grid_load_collision_and_radius<Box, Stencil, First + 0, 0, Access>(states, present, radii, source_owner);
    const auto s1 = grid_load_collision_and_radius<Box, Stencil, First + 1, 1, Access>(states, present, radii, source_owner);
    const auto s2 = grid_load_collision_and_radius<Box, Stencil, First + 2, 2, Access>(states, present, radii, source_owner);
    const auto s3 = grid_load_collision_and_radius<Box, Stencil, First + 3, 3, Access>(states, present, radii, source_owner);
    auto state = _mm256_setr_epi64x(s0.state, s1.state, s2.state, s3.state);
    auto region = _mm256_setr_epi64x(s0.region, s1.region, s2.region, s3.region);
    auto ids = _mm256_srli_epi64(state, 32);
    auto wrapped = _mm256_srai_epi64(_mm256_slli_epi64(state, 32), 32);
    __mmask8 different_owner = present & _mm256_cmpneq_epi64_mask(ids, source_id);
    __mmask8 valid = different_owner & GridAvx512vlSource<Growth>::valid(region);
    auto weights = _mm256_cvtepu32_epi64(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(c.weights[source.grid_cell].data() + First)));
    auto other_intercept = _mm256_add_epi64(_mm256_srai_epi64(region, 2), wrapped);
    auto raw = _mm256_sub_epi64(_mm256_sub_epi64(weights, source_intercept), other_intercept);
    auto times = GridAvx512vlSource<Growth>::time(raw, region);
    return _mm256_mask_mov_epi64(_mm256_set1_epi64x(GRID_NO_TIME), valid, times);
}

template <typename Box, typename Stencil, GridSourceGrowth Growth, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX512VL_TARGET PM_GRID_SIMD_INLINE GridNextEvent grid_matching_avx512vl_scan(
    const MatchingGridComponent& c,
    const DetectorNode& source,
    const VaryingCT* radii,
    cumulative_time_int source_intercept,
    uint32_t owner) {
    auto b1 = _mm256_set1_epi64x(source_intercept);
    auto source_id = _mm256_set1_epi64x(owner);
    auto a = grid_matching_avx512vl_batch<Box, Stencil, Growth, 0, Access>(c, source, radii, b1, source_id, owner);
    auto b = grid_matching_avx512vl_batch<Box, Stencil, Growth, 4, Access>(c, source, radii, b1, source_id, owner);
    auto d = grid_matching_avx512vl_batch<Box, Stencil, Growth, 8, Access>(c, source, radii, b1, source_id, owner);
    auto e = _mm256_set1_epi64x(GRID_NO_TIME);
    auto minimum = _mm256_min_epi64(_mm256_min_epi64(a, b), d);
    if constexpr (STRUCTURED_STORAGE_DIRECTIONS == 16) {
        e = grid_matching_avx512vl_batch<Box, Stencil, Growth, 12, Access>(c, source, radii, b1, source_id, owner);
        minimum = _mm256_min_epi64(minimum, e);
    }
    minimum = _mm256_min_epi64(minimum, _mm256_permute4x64_epi64(minimum, 0x4e));
    minimum = _mm256_min_epi64(minimum, _mm256_permute4x64_epi64(minimum, 0xb1));
    const auto time = _mm_cvtsi128_si64(_mm256_castsi256_si128(minimum));
    if (time == GRID_NO_TIME)
        return {SIZE_MAX, GRID_NO_TIME};
    unsigned winners = _mm256_cmpeq_epi64_mask(a, minimum) |
                       (unsigned(_mm256_cmpeq_epi64_mask(b, minimum)) << 4) |
                       (unsigned(_mm256_cmpeq_epi64_mask(d, minimum)) << 8);
    if constexpr (STRUCTURED_STORAGE_DIRECTIONS == 16)
        winners |= unsigned(_mm256_cmpeq_epi64_mask(e, minimum)) << 12;
    return {std::countr_zero(winners), time};
}

template <typename Box, typename Stencil, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX512VL_TARGET GridNextEvent grid_matching_avx512vl(
    const MatchingGridComponent& c, const DetectorNode& source, const VaryingCT* radii) {
    const auto& state = c.collision_states[source.grid_cell];
    const auto radius = radii[state.top_region_id];
    if (radius.is_shrinking())
        return {SIZE_MAX, GRID_NO_TIME};
    const auto source_intercept = radius.y_intercept() + state.wrapped_radius_cached;
    if (radius.is_growing())
        return grid_matching_avx512vl_scan<Box, Stencil, GridSourceGrowth::growing, Access>(
            c, source, radii, source_intercept, state.top_region_id);
    return grid_matching_avx512vl_scan<Box, Stencil, GridSourceGrowth::frozen, Access>(
        c, source, radii, source_intercept, state.top_region_id);
}
#endif

#if PM_GRID_AVX512
template <GridSourceGrowth Growth>
struct GridAvx512Source;

template <>
struct GridAvx512Source<GridSourceGrowth::growing> {
    PM_GRID_AVX512_TARGET static PM_GRID_SIMD_INLINE __mmask8 valid(__m512i region) {
        return _mm512_cmpeq_epi64_mask(
            _mm512_and_si512(region, _mm512_set1_epi64(2)), _mm512_setzero_si512());
    }
    PM_GRID_AVX512_TARGET static PM_GRID_SIMD_INLINE __m512i time(__m512i raw, __m512i region) {
        return _mm512_srav_epi64(raw, _mm512_and_si512(region, _mm512_set1_epi64(1)));
    }
};

template <>
struct GridAvx512Source<GridSourceGrowth::frozen> {
    PM_GRID_AVX512_TARGET static PM_GRID_SIMD_INLINE __mmask8 valid(__m512i region) {
        return _mm512_cmpeq_epi64_mask(
            _mm512_and_si512(region, _mm512_set1_epi64(1)), _mm512_set1_epi64(1));
    }
    PM_GRID_AVX512_TARGET static PM_GRID_SIMD_INLINE __m512i time(__m512i raw, __m512i) {
        return raw;
    }
};

// Both masks describe actual storage, not merely the selected stencil. With
// twelve slots the second batch reads exactly four uint32_t weights; lanes
// 12..15 are never addressed, even for a row ending at an inaccessible page.
template <size_t First>
PM_GRID_AVX512_TARGET PM_GRID_SIMD_INLINE __m512i grid_load_weights_avx512(const weight_int* weights) {
    static_assert(First < STRUCTURED_STORAGE_DIRECTIONS);
    constexpr size_t count = std::min(size_t{8}, STRUCTURED_STORAGE_DIRECTIONS - First);
    constexpr __mmask16 mask = (uint16_t{1} << count) - 1;
    return _mm512_cvtepu32_epi64(_mm512_castsi512_si256(_mm512_maskz_loadu_epi32(mask, weights + First)));
}

template <typename Box, typename Stencil, GridSourceGrowth Growth, size_t First, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX512_TARGET PM_GRID_SIMD_INLINE __m512i grid_matching_avx512_batch(
    const MatchingGridComponent& c,
    const DetectorNode& source,
    const VaryingCT* radii,
    __m512i source_intercept,
    __m512i source_id,
    uint32_t source_owner) {
    static_assert(First < STRUCTURED_STORAGE_DIRECTIONS);
    static_assert(sizeof(VaryingCT) == sizeof(int64_t));
    static_assert(std::endian::native == std::endian::little);
    const __mmask8 present = (c.present[source.grid_cell] & CircuitStorage<Stencil>::mask) >> First & 255;
    if (!present)
        return _mm512_set1_epi64(GRID_NO_TIME);
    auto zero = _mm512_setzero_si512();
    const auto* states = c.collision_states.data() + source.grid_cell;
    const auto s0 = grid_load_collision_and_radius<Box, Stencil, First + 0, 0, Access>(states, present, radii, source_owner);
    const auto s1 = grid_load_collision_and_radius<Box, Stencil, First + 1, 1, Access>(states, present, radii, source_owner);
    const auto s2 = grid_load_collision_and_radius<Box, Stencil, First + 2, 2, Access>(states, present, radii, source_owner);
    const auto s3 = grid_load_collision_and_radius<Box, Stencil, First + 3, 3, Access>(states, present, radii, source_owner);
    const auto s4 = grid_load_collision_and_radius<Box, Stencil, First + 4, 4, Access>(states, present, radii, source_owner);
    const auto s5 = grid_load_collision_and_radius<Box, Stencil, First + 5, 5, Access>(states, present, radii, source_owner);
    const auto s6 = grid_load_collision_and_radius<Box, Stencil, First + 6, 6, Access>(states, present, radii, source_owner);
    const auto s7 = grid_load_collision_and_radius<Box, Stencil, First + 7, 7, Access>(states, present, radii, source_owner);
    auto state = _mm512_setr_epi64(s0.state, s1.state, s2.state, s3.state, s4.state, s5.state, s6.state, s7.state);
    auto region = _mm512_setr_epi64(s0.region, s1.region, s2.region, s3.region, s4.region, s5.region, s6.region, s7.region);
    auto ids = _mm512_srli_epi64(state, 32);
    auto wrapped = _mm512_srai_epi64(_mm512_slli_epi64(state, 32), 32);
    __mmask8 different_owner = present & _mm512_cmpneq_epi64_mask(ids, source_id);
    __mmask8 valid = different_owner & GridAvx512Source<Growth>::valid(region);
    auto weights = grid_load_weights_avx512<First>(c.weights[source.grid_cell].data());
    auto other_intercept = _mm512_add_epi64(_mm512_srai_epi64(region, 2), wrapped);
    auto raw = _mm512_sub_epi64(_mm512_sub_epi64(weights, source_intercept), other_intercept);
    auto times = GridAvx512Source<Growth>::time(raw, region);
    return _mm512_mask_mov_epi64(_mm512_set1_epi64(GRID_NO_TIME), valid, times);
}

template <typename Box, typename Stencil, GridSourceGrowth Growth, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX512_TARGET PM_GRID_SIMD_INLINE GridNextEvent grid_matching_avx512_scan(
    const MatchingGridComponent& c,
    const DetectorNode& source,
    const VaryingCT* radii,
    cumulative_time_int source_intercept,
    uint32_t owner) {
    auto b1 = _mm512_set1_epi64(source_intercept);
    auto source_id = _mm512_set1_epi64(owner);
    auto a = grid_matching_avx512_batch<Box, Stencil, Growth, 0, Access>(c, source, radii, b1, source_id, owner);
    auto b = grid_matching_avx512_batch<Box, Stencil, Growth, 8, Access>(c, source, radii, b1, source_id, owner);
    const auto time = _mm512_reduce_min_epi64(_mm512_min_epi64(a, b));
    if (time == GRID_NO_TIME)
        return {SIZE_MAX, GRID_NO_TIME};
    auto minimum = _mm512_set1_epi64(time);
    unsigned winners = _mm512_cmpeq_epi64_mask(a, minimum) |
                       (unsigned(_mm512_cmpeq_epi64_mask(b, minimum)) << 8);
    return {std::countr_zero(winners), time};
}

template <typename Box, typename Stencil, GridCellAccess Access = GridCellAccess::masked>
PM_GRID_AVX512_TARGET GridNextEvent grid_matching_avx512(
    const MatchingGridComponent& c, const DetectorNode& source, const VaryingCT* radii) {
    const auto& state = c.collision_states[source.grid_cell];
    const auto radius = radii[state.top_region_id];
    if (radius.is_shrinking())
        return {SIZE_MAX, GRID_NO_TIME};
    const auto source_intercept = radius.y_intercept() + state.wrapped_radius_cached;
    if (radius.is_growing())
        return grid_matching_avx512_scan<Box, Stencil, GridSourceGrowth::growing, Access>(
            c, source, radii, source_intercept, state.top_region_id);
    return grid_matching_avx512_scan<Box, Stencil, GridSourceGrowth::frozen, Access>(
        c, source, radii, source_intercept, state.top_region_id);
}
#endif

}  // namespace pm::structured
#endif
