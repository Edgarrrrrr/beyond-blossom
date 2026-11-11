// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_ONE_HOP_BOUND_KERNEL_H
#define BB_ONE_HOP_BOUND_KERNEL_H
#include "beyond_blossom/structured/predecode/one_hop_neighborhood_simd.h"
#include "beyond_blossom/structured/predecode/one_hop_arm_kernels.h"

namespace beyond_blossom::structured {
// Internal callers supply a grid whose construction owns the entire range.
// Physical seed indices remain global, including fixed-bucket capacity gaps.
struct OneHopBoundRange {
    OneHopPredecoder::Grid& grid;
    PredecodeSeed* seeds;
    uint32_t begin, end;
    uint16_t* masks;
    uint32_t* links;
    MatchingResult& output;
};
#if BB_STRUCTURED_STORAGE_DIRECTIONS == 12 && \
    (BB_PREDECODE_AVX2_AVAILABLE || (defined(__APPLE__) && defined(__aarch64__)))
#if BB_PREDECODE_AVX2_AVAILABLE
#define BB_PREDECODE_BOUND_TARGET __attribute__((target("avx2,popcnt")))
#else
#define BB_PREDECODE_BOUND_TARGET
#endif
// One invocation handles one known grid. Bind these arrays once per shot so
// degree, growth and pair publication can share their pointers and edge reads.
template <typename Box, typename Stencil>
struct BoundShotKernel {
    using Storage = CircuitStorage<Stencil>;
    const uint32_t* active;
    weight_int* radii;
    const uint16_t* present;
    const uint16_t* boundary;
    const StructuredGridWeightRow* weights;
    const std::array<obs_int, 12>* obs;
    const ptrdiff_t* offsets;
    PredecodeSeed* seeds;
    MatchingResult output{};

#if BB_PREDECODE_AVX2_AVAILABLE
    template <size_t First, size_t... I>
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) inline unsigned empty4(
        const uint32_t* cells, std::index_sequence<I...>) const {
        auto bits = _mm_setr_epi32(cells[Box::template offset<Storage::directions[First + I]>()]...);
        return unsigned(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(bits, _mm_setzero_si128()))));
    }
#endif
    template <size_t... I>
    inline uint16_t scalar_mask(const uint32_t* cells, std::index_sequence<I...>) const {
        return ((uint16_t(cells[Box::template offset<Storage::directions[I]>()] != 0) << I) | ...);
    }
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) inline uint16_t mask(uint32_t cell) const {
        const auto* cells = active + cell;
        #if BB_PREDECODE_NEIGHBORHOOD_SIMD && BB_PREDECODE_AVX2_AVAILABLE
        const auto empty = empty4<0>(cells, std::make_index_sequence<4>{}) |
                           (empty4<4>(cells, std::make_index_sequence<4>{}) << 4) |
                           (empty4<8>(cells, std::make_index_sequence<4>{}) << 8);
        return present[cell] & ~empty;
        #else
        return present[cell] & scalar_mask(cells, std::make_index_sequence<12>{});
        #endif
    }
#if BB_PREDECODE_AVX2_AVAILABLE
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) static inline uint32_t minimum4(__m128i m) {
        m = _mm_min_epu32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 0, 3, 2)));
        m = _mm_min_epu32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(2, 3, 0, 1)));
        return uint32_t(_mm_cvtsi128_si32(m));
    }
#endif
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) inline uint32_t edge_min(uint32_t cell) const {
#if BB_PREDECODE_AVX2_AVAILABLE
        const auto* row = weights[cell].data();
        const auto valid = _mm_set1_epi32(present[cell]);
        return minimum4(_mm_min_epu32(
            _mm_min_epu32(one_hop_weights4<0>(row, valid), one_hop_weights4<4>(row, valid)),
            one_hop_weights4<8>(row, valid)));
#else
        return one_hop_arm_minimum_weight(weights[cell].data(), present[cell]);
#endif
    }
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) inline uint32_t full_limit(uint32_t cell) const {
#if BB_PREDECODE_AVX2_AVAILABLE
        const auto* row = weights[cell].data();
        const auto valid = _mm_set1_epi32(present[cell]);
        const auto* rs = radii + cell;
        #if BB_PREDECODE_CAP_3X128
        return minimum4(_mm_min_epu32(
            _mm_min_epu32(
                one_hop_limits4<Box, Stencil, 0>(rs, row, valid), one_hop_limits4<Box, Stencil, 4>(rs, row, valid)),
            one_hop_limits4<Box, Stencil, 8>(rs, row, valid)));
        #else
        // Retain the parent's 8+4 full-cap configuration. Sparse cap already
        // used the 3x128 weight-only minimum before the 1103 port.
        const auto bits8 = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
        const auto bits4 = _mm_setr_epi32(256, 512, 1024, 2048);
        const auto present8 = _mm256_set1_epi32(present[cell]);
        const auto valid8 = _mm256_cmpeq_epi32(_mm256_and_si256(present8, bits8), bits8);
        const auto valid4 = _mm_cmpeq_epi32(_mm_and_si128(valid, bits4), bits4);
        auto limits8 = _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(row)),
            one_hop_radius_batch8<Box, Stencil, 0>(rs, std::make_index_sequence<8>{}));
        auto limits4 = _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + 8)),
            one_hop_radius_batch4<Box, Stencil, 8>(rs, std::make_index_sequence<4>{}));
        limits8 = _mm256_or_si256(limits8, _mm256_xor_si256(valid8, _mm256_set1_epi32(-1)));
        limits4 = _mm_or_si128(limits4, _mm_xor_si128(valid4, _mm_set1_epi32(-1)));
        return minimum4(_mm_min_epu32(
            _mm_min_epu32(_mm256_castsi256_si128(limits8), _mm256_extracti128_si256(limits8, 1)), limits4));
        #endif
#else
        const auto* row = weights[cell].data();
        const auto valid = vdupq_n_u32(present[cell]);
        const auto* rs = radii + cell;
        return vminvq_u32(vminq_u32(
            vminq_u32(one_hop_arm_limits4<Box, Stencil, 0>(rs, row, valid),
                       one_hop_arm_limits4<Box, Stencil, 4>(rs, row, valid)),
            one_hop_arm_limits4<Box, Stencil, 8>(rs, row, valid)));
#endif
    }
    inline void grow(PredecodeSeed& s, uint32_t limit) {
        if (limit != UINT32_MAX) {
            s.radius = limit & ~weight_int{1};
            radii[s.cell] = s.radius;
        }
    }
    inline void pair(uint32_t i, uint32_t j, unsigned slot, weight_int weight) {
        auto& s = seeds[i];
        s.mate = j;
        s.match_slot = slot;
        if (j != PredecodeSeed::BOUNDARY) {
            seeds[j].mate = i;
            seeds[j].match_slot = slot ^ 1;
        }
        output.weight += weight;
        output.obs_mask ^= obs[s.cell][slot];
    }
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) inline bool try_neighbor(
        uint32_t i, uint32_t other_cell, unsigned slot, weight_int weight) {
        const auto j = active[other_cell] - 1;
        auto& other = seeds[j];
        if (other.mate != PredecodeSeed::UNMATCHED)
            return false;
        const auto radius = seeds[i].radius;
        if (radius + other.radius < weight)
            grow(other, full_limit(other_cell));
        if (radius + other.radius != weight)
            return false;
        pair(i, j, slot, weight);
        return true;
    }
    template <bool Leaf>
    BB_PREDECODE_BOUND_TARGET __attribute__((always_inline)) inline void visit(uint32_t i, uint16_t neighbors) {
        auto& s = seeds[i];
        auto cell = s.cell;
        const auto* row = weights[cell].data();
        auto limit = edge_min(cell);
        if constexpr (Leaf) {
            // With exactly one active neighbor, the same edge limits growth
            // and is the only ordinary edge on which this seed can match.
            if (neighbors) {
                const unsigned slot = std::countr_zero(neighbors);
                const uint32_t other_cell = uint32_t(cell + offsets[slot]);
                const auto weight = row[slot];
                grow(s, std::min(limit, weight - radii[other_cell]));
                try_neighbor(i, other_cell, slot, weight);
                return;
            }
            grow(s, limit);
        } else {
            for (auto bits = neighbors; bits; bits &= bits - 1) {
                const unsigned slot = std::countr_zero(bits);
                limit = std::min(limit, row[slot] - radii[cell + offsets[slot]]);
            }
            grow(s, limit);
            for (auto bits = neighbors; bits; bits &= bits - 1) {
                const unsigned slot = std::countr_zero(bits);
                if (try_neighbor(i, uint32_t(cell + offsets[slot]), slot, row[slot]))
                    return;
            }
        }
        for (auto bits = boundary[cell]; bits; bits &= bits - 1) {
            const unsigned slot = std::countr_zero(bits);
            if (s.radius == row[slot]) {
                pair(i, PredecodeSeed::BOUNDARY, slot, row[slot]);
                return;
            }
        }
    }
};

template <typename Box, typename Stencil>
BB_PREDECODE_BOUND_TARGET void run_bound_one_hop(OneHopBoundRange& range) {
    auto& w = range.grid;
    BoundShotKernel<Box, Stencil> k{
        w.predecode.active.data(), w.predecode.radii.data(), w.present.data(),
        w.boundary_mask.data(), w.weights.data(), w.observables.data(),
        w.predecode.offsets.data(), range.seeds};
    const auto end = range.end;
    uint32_t heads[13], tails[13];
    uint16_t used = 0;
    auto* masks = range.masks;
    auto* links = range.links;
    for (uint32_t i = range.begin; i < end; ++i) {
        const auto& seed = k.seeds[i];
        assert(seed.grid == &w);
        if (seed.mate != PredecodeSeed::UNMATCHED)
            continue;
        const auto mask = k.mask(seed.cell);
        const auto degree = std::popcount(uint16_t(mask | k.boundary[seed.cell]));
        if (degree <= 1)
            k.template visit<true>(i, mask);
        else {
            masks[i] = mask;
            const auto bit = uint16_t(1 << degree);
            if (used & bit)
                links[tails[degree]] = i;
            else {
                used |= bit;
                heads[degree] = i;
            }
            tails[degree] = i;
        }
    }
    for (; used; used &= used - 1) {
        const unsigned degree = std::countr_zero(used);
        links[tails[degree]] = end;
        for (auto i = heads[degree]; i != end; i = links[i])
            if (k.seeds[i].mate == PredecodeSeed::UNMATCHED)
                k.template visit<false>(i, masks[i]);
    }
    range.output += k.output;
}
#undef BB_PREDECODE_BOUND_TARGET
#endif
}  // namespace beyond_blossom::structured
#endif
