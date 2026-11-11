// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_ONE_HOP_KERNELS_H
#define PYMATCHING_STRUCTURED_ONE_HOP_KERNELS_H
#include <algorithm>
#include <bit>

#include "pymatching/sparse_blossom/structured/predecode/one_hop_predecoder.h"

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define PM_PREDECODE_POPCNT_AVAILABLE 1
#define PM_PREDECODE_POPCNT_TARGET __attribute__((target("popcnt")))
#else
#define PM_PREDECODE_POPCNT_AVAILABLE 0
#endif

namespace pm::structured {

template <typename Box, typename Stencil, size_t... D>
uint16_t one_hop_syndrome_mask_impl(const OneHopPredecoder::Grid& w, size_t cell, std::index_sequence<D...>) {
    const auto* active = w.predecode.active.data() + cell;
    // The grid builder guarantees a readable halo for all twelve offsets.
    return w.present[cell] &
           ((uint16_t(active[Box::template offset<D>()] != 0) << CircuitStorage<Stencil>::template slot<D>) | ...);
}

template <typename Box, typename Stencil>
uint16_t one_hop_syndrome_mask(const OneHopPredecoder::Grid& w, size_t cell) {
    return one_hop_syndrome_mask_impl<Box, Stencil>(w, cell, typename Stencil::Directions{});
}

// Portable 16-bit SWAR count with fixed bit arithmetic. The constructor binds
// the POPCNT path below on CPUs that support that separate ISA extension.
inline constexpr uint32_t one_hop_degree_portable(uint16_t bits) {
    uint32_t value = bits;
    value -= (value >> 1) & 0x5555U;
    value = (value & 0x3333U) + ((value >> 2) & 0x3333U);
    value = (value + (value >> 4)) & 0x0F0FU;
    return (value + (value >> 8)) & 0x1FU;
}

template <typename Box, typename Stencil>
uint32_t one_hop_neighborhood_portable(const OneHopPredecoder::Grid& w, size_t cell, uint16_t boundary_mask) {
    const uint16_t candidates = one_hop_syndrome_mask<Box, Stencil>(w, cell);
    const auto degree = one_hop_degree_portable(uint16_t(candidates | boundary_mask));
    return uint32_t(candidates) | (degree << 16);
}

#if PM_PREDECODE_POPCNT_AVAILABLE
template <typename Box, typename Stencil>
PM_PREDECODE_POPCNT_TARGET uint32_t
one_hop_neighborhood_popcnt(const OneHopPredecoder::Grid& w, size_t cell, uint16_t boundary_mask) {
    const uint16_t candidates = one_hop_syndrome_mask<Box, Stencil>(w, cell);
    const auto degree = uint32_t(std::popcount(uint16_t(candidates | boundary_mask)));
    return uint32_t(candidates) | (degree << 16);
}
#endif

template <typename Box, typename Stencil, size_t D>
inline weight_int one_hop_edge_limit(const OneHopPredecoder::Grid& w, size_t cell) {
    constexpr size_t slot = CircuitStorage<Stencil>::template slot<D>;
    // Feasibility guarantees subtraction cannot underflow on a present edge.
    const auto radius = w.predecode.radii[ptrdiff_t(cell) + Box::template offset<D>()];
    return w.present[cell] & (uint16_t{1} << slot) ? w.weights[cell][slot] - radius : UINT32_MAX;
}

template <typename Box, typename Stencil, size_t... D>
weight_int one_hop_radius_limit_impl(const OneHopPredecoder::Grid& w, size_t cell, std::index_sequence<D...>) {
    return std::min({one_hop_edge_limit<Box, Stencil, D>(w, cell)...});
}

template <typename Box, typename Stencil>
weight_int one_hop_radius_limit(const OneHopPredecoder::Grid& w, size_t cell) {
    return one_hop_radius_limit_impl<Box, Stencil>(w, cell, typename Stencil::Directions{});
}

template <typename Stencil, size_t... D>
weight_int one_hop_minimum_weight_impl(
    const OneHopPredecoder::Grid& w, size_t cell, std::index_sequence<D...>) {
    return std::min({(w.present[cell] & (uint16_t{1} << CircuitStorage<Stencil>::template slot<D>)
                         ? w.weights[cell][CircuitStorage<Stencil>::template slot<D>]
                         : UINT32_MAX)...});
}

inline weight_int one_hop_active_radius_limit(
    const OneHopPredecoder::Grid& w, size_t cell, uint16_t candidates, weight_int minimum) {
    while (candidates) {
        const auto slot = std::countr_zero(candidates);
        candidates &= candidates - 1;
        const auto neighbor = ptrdiff_t(cell) + w.predecode.offsets[slot];
        minimum = std::min(minimum, w.weights[cell][slot] - w.predecode.radii[neighbor]);
    }
    return minimum;
}

template <typename Stencil>
weight_int one_hop_radius_limit_sparse(const OneHopPredecoder::Grid& w, size_t cell, uint16_t candidates) {
    // Only active syndrome cells have nonzero predecode radii. The all-edge
    // minimum retains every empty-node and boundary constraint; subtracting
    // radii on active edges then gives the exact full-neighborhood minimum.
    return one_hop_active_radius_limit(
        w, cell, candidates, one_hop_minimum_weight_impl<Stencil>(w, cell, typename Stencil::Directions{}));
}

}  // namespace pm::structured
#endif
