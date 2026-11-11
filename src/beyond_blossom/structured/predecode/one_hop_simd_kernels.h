// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_ONE_HOP_SIMD_KERNELS_H
#define BB_STRUCTURED_ONE_HOP_SIMD_KERNELS_H
#include "beyond_blossom/structured/predecode/one_hop_kernels.h"

#ifndef BB_PREDECODE_CAP_3X128
#define BB_PREDECODE_CAP_3X128 0
#endif

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define BB_PREDECODE_AVX2_AVAILABLE 1
#define BB_PREDECODE_AVX2_TARGET __attribute__((target("avx2")))
#else
#define BB_PREDECODE_AVX2_AVAILABLE 0
#endif

namespace beyond_blossom::structured {
#if BB_PREDECODE_AVX2_AVAILABLE && BB_STRUCTURED_STORAGE_DIRECTIONS == 12

template <typename Box, typename Stencil, size_t First, size_t... Lane>
BB_PREDECODE_AVX2_TARGET inline __m256i one_hop_radius_batch8(const weight_int* radii, std::index_sequence<Lane...>) {
    return _mm256_setr_epi32(radii[Box::template offset<CircuitStorage<Stencil>::directions[First + Lane]>()]...);
}

template <typename Box, typename Stencil, size_t First, size_t... Lane>
BB_PREDECODE_AVX2_TARGET inline __m128i one_hop_radius_batch4(const weight_int* radii, std::index_sequence<Lane...>) {
    return _mm_setr_epi32(radii[Box::template offset<CircuitStorage<Stencil>::directions[First + Lane]>()]...);
}

template <typename Box, typename Stencil>
BB_PREDECODE_AVX2_TARGET weight_int one_hop_radius_limit_avx2(const OneHopPredecoder::Grid& w, size_t cell) {
    const auto* radii = w.predecode.radii.data() + cell;
    const auto* weights = w.weights[cell].data();
    const auto bits8 = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    const auto bits4 = _mm_setr_epi32(256, 512, 1024, 2048);
    const auto present8 = _mm256_set1_epi32(w.present[cell]);
    const auto present4 = _mm256_castsi256_si128(present8);
    const auto valid8 = _mm256_cmpeq_epi32(_mm256_and_si256(present8, bits8), bits8);
    const auto valid4 = _mm_cmpeq_epi32(_mm_and_si128(present4, bits4), bits4);
    auto limits8 = _mm256_sub_epi32(
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights)),
        one_hop_radius_batch8<Box, Stencil, 0>(radii, std::make_index_sequence<8>{}));
    auto limits4 = _mm_sub_epi32(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + 8)),
        one_hop_radius_batch4<Box, Stencil, 8>(radii, std::make_index_sequence<4>{}));
    // All slots are read. Absent slots are made UINT32_MAX before reduction.
    limits8 = _mm256_or_si256(limits8, _mm256_xor_si256(valid8, _mm256_set1_epi32(-1)));
    limits4 = _mm_or_si128(limits4, _mm_xor_si128(valid4, _mm_set1_epi32(-1)));
    auto minimum =
        _mm_min_epu32(_mm_min_epu32(_mm256_castsi256_si128(limits8), _mm256_extracti128_si256(limits8, 1)), limits4);
    minimum = _mm_min_epu32(minimum, _mm_shuffle_epi32(minimum, _MM_SHUFFLE(1, 0, 3, 2)));
    minimum = _mm_min_epu32(minimum, _mm_shuffle_epi32(minimum, _MM_SHUFFLE(2, 3, 0, 1)));
    return static_cast<weight_int>(_mm_cvtsi128_si32(minimum));
}

template <typename Box, typename Stencil, size_t First>
BB_PREDECODE_AVX2_TARGET inline __m128i one_hop_limits4(
    const weight_int* radii, const weight_int* weights, __m128i present) {
    static_assert(First == 0 || First == 4 || First == 8);
    const auto bits = _mm_setr_epi32(1 << First, 2 << First, 4 << First, 8 << First);
    const auto valid = _mm_cmpeq_epi32(_mm_and_si128(present, bits), bits);
    const auto limits = _mm_sub_epi32(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + First)),
        one_hop_radius_batch4<Box, Stencil, First>(radii, std::make_index_sequence<4>{}));
    return _mm_or_si128(limits, _mm_xor_si128(valid, _mm_set1_epi32(-1)));
}

template <typename Box, typename Stencil>
BB_PREDECODE_AVX2_TARGET weight_int one_hop_radius_limit_avx2_3x128(const OneHopPredecoder::Grid& w, size_t cell) {
    const auto* radii = w.predecode.radii.data() + cell;
    const auto* weights = w.weights[cell].data();
    const auto present = _mm_set1_epi32(w.present[cell]);
    // Each group reads exactly four weights and four halo-safe neighbor radii.
    // Keeping the groups separate avoids a 256-bit pack followed by extraction.
    const auto limits0 = one_hop_limits4<Box, Stencil, 0>(radii, weights, present);
    const auto limits4 = one_hop_limits4<Box, Stencil, 4>(radii, weights, present);
    const auto limits8 = one_hop_limits4<Box, Stencil, 8>(radii, weights, present);
    auto minimum = _mm_min_epu32(_mm_min_epu32(limits0, limits4), limits8);
    minimum = _mm_min_epu32(minimum, _mm_shuffle_epi32(minimum, _MM_SHUFFLE(1, 0, 3, 2)));
    minimum = _mm_min_epu32(minimum, _mm_shuffle_epi32(minimum, _MM_SHUFFLE(2, 3, 0, 1)));
    return static_cast<weight_int>(_mm_cvtsi128_si32(minimum));
}

template <size_t First>
BB_PREDECODE_AVX2_TARGET inline __m128i one_hop_weights4(const weight_int* weights, __m128i present) {
    const auto bits = _mm_setr_epi32(1 << First, 2 << First, 4 << First, 8 << First);
    const auto valid = _mm_cmpeq_epi32(_mm_and_si128(present, bits), bits);
    return _mm_or_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + First)),
        _mm_xor_si128(valid, _mm_set1_epi32(-1)));
}

BB_PREDECODE_AVX2_TARGET inline weight_int one_hop_radius_limit_sparse_avx2(
    const OneHopPredecoder::Grid& w, size_t cell, uint16_t candidates) {
    const auto* weights = w.weights[cell].data();
    const auto present = _mm_set1_epi32(w.present[cell]);
    auto minimum = _mm_min_epu32(
        _mm_min_epu32(one_hop_weights4<0>(weights, present), one_hop_weights4<4>(weights, present)),
        one_hop_weights4<8>(weights, present));
    minimum = _mm_min_epu32(minimum, _mm_shuffle_epi32(minimum, _MM_SHUFFLE(1, 0, 3, 2)));
    minimum = _mm_min_epu32(minimum, _mm_shuffle_epi32(minimum, _MM_SHUFFLE(2, 3, 0, 1)));
    return one_hop_active_radius_limit(w, cell, candidates, static_cast<weight_int>(_mm_cvtsi128_si32(minimum)));
}
#endif
}  // namespace beyond_blossom::structured
#endif
