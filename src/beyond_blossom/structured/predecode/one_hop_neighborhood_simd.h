// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_ONE_HOP_NEIGHBORHOOD_SIMD_H
#define BB_STRUCTURED_ONE_HOP_NEIGHBORHOOD_SIMD_H
#include "beyond_blossom/structured/predecode/one_hop_simd_kernels.h"

#ifndef BB_PREDECODE_NEIGHBORHOOD_SIMD
#define BB_PREDECODE_NEIGHBORHOOD_SIMD 0
#endif

namespace beyond_blossom::structured {
#if BB_PREDECODE_AVX2_AVAILABLE && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
// POPCNT is independent from AVX2. Keep the portable-count callback valid when
// only AVX2 is present, even if the surrounding TU is compiled with POPCNT.
#define BB_PREDECODE_NEIGHBORHOOD_AVX2_TARGET __attribute__((target("avx2,no-popcnt")))
#define BB_PREDECODE_NEIGHBORHOOD_POPCNT_TARGET __attribute__((target("avx2,popcnt")))

template <typename Box, typename Stencil, size_t First, size_t... Lane>
BB_PREDECODE_NEIGHBORHOOD_AVX2_TARGET inline __m128i one_hop_active_batch4(
    const uint32_t* active, std::index_sequence<Lane...>) {
    return _mm_setr_epi32(
        std::bit_cast<int32_t>(active[Box::template offset<CircuitStorage<Stencil>::directions[First + Lane]>()])...);
}

template <typename Box, typename Stencil>
BB_PREDECODE_NEIGHBORHOOD_AVX2_TARGET inline uint16_t one_hop_syndrome_mask_avx2(
    const OneHopPredecoder::Grid& w, size_t cell) {
    const auto* active = w.predecode.active.data() + cell;
    const auto zero = _mm_setzero_si128();
    const auto empty0 =
        _mm_cmpeq_epi32(one_hop_active_batch4<Box, Stencil, 0>(active, std::make_index_sequence<4>{}), zero);
    const auto empty4 =
        _mm_cmpeq_epi32(one_hop_active_batch4<Box, Stencil, 4>(active, std::make_index_sequence<4>{}), zero);
    const auto empty8 =
        _mm_cmpeq_epi32(one_hop_active_batch4<Box, Stencil, 8>(active, std::make_index_sequence<4>{}), zero);
    const uint32_t empty = uint32_t(_mm_movemask_ps(_mm_castsi128_ps(empty0))) |
                           (uint32_t(_mm_movemask_ps(_mm_castsi128_ps(empty4))) << 4) |
                           (uint32_t(_mm_movemask_ps(_mm_castsi128_ps(empty8))) << 8);
    // Twelve fixed-offset uint32 reads (48 useful bytes); present removes absent
    // lanes after loading. Equality with zero accepts every nonzero uint32 ID.
    return uint16_t(~empty & 0xFFFU) & w.present[cell];
}

template <typename Box, typename Stencil>
BB_PREDECODE_NEIGHBORHOOD_AVX2_TARGET uint32_t
one_hop_neighborhood_avx2_portable(const OneHopPredecoder::Grid& w, size_t cell, uint16_t boundary_mask) {
    const uint16_t candidates = one_hop_syndrome_mask_avx2<Box, Stencil>(w, cell);
    const auto degree = one_hop_degree_portable(uint16_t(candidates | boundary_mask));
    return uint32_t(candidates) | (degree << 16);
}

template <typename Box, typename Stencil>
BB_PREDECODE_NEIGHBORHOOD_POPCNT_TARGET uint32_t
one_hop_neighborhood_avx2_popcnt(const OneHopPredecoder::Grid& w, size_t cell, uint16_t boundary_mask) {
    const uint16_t candidates = one_hop_syndrome_mask_avx2<Box, Stencil>(w, cell);
    const auto degree = uint32_t(std::popcount(uint16_t(candidates | boundary_mask)));
    return uint32_t(candidates) | (degree << 16);
}
#undef BB_PREDECODE_NEIGHBORHOOD_AVX2_TARGET
#undef BB_PREDECODE_NEIGHBORHOOD_POPCNT_TARGET
#endif
}  // namespace beyond_blossom::structured
#endif
