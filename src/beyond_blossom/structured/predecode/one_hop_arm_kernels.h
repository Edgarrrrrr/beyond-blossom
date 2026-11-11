// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_ONE_HOP_ARM_KERNELS_H
#define BB_STRUCTURED_ONE_HOP_ARM_KERNELS_H

#include "beyond_blossom/structured/predecode/one_hop_kernels.h"

#if defined(__aarch64__) && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
#include <arm_neon.h>

namespace beyond_blossom::structured {

template <typename Box, typename Stencil, size_t First, size_t... Lane>
[[gnu::always_inline]] inline uint32x4_t one_hop_arm_radii4(
    const weight_int* radii, std::index_sequence<Lane...>) {
    // Every lane has a compile-time stencil offset. The readable grid halo
    // and partition guards cover all twelve reads, including absent edges.
    return uint32x4_t{radii[Box::template offset<CircuitStorage<Stencil>::directions[First + Lane]>()]...};
}

template <typename Box, typename Stencil, size_t First>
[[gnu::always_inline]] inline uint32x4_t one_hop_arm_limits4(
    const weight_int* radii, const weight_int* weights, uint32x4_t present) {
    const uint32x4_t bits{1U << First, 2U << First, 4U << First, 8U << First};
    const auto valid = vtstq_u32(present, bits);
    const auto limits = vsubq_u32(
        vld1q_u32(weights + First), one_hop_arm_radii4<Box, Stencil, First>(radii, std::make_index_sequence<4>{}));
    // Absent lanes become UINT32_MAX after subtraction. Present lanes retain
    // the scalar feasibility contract weight >= neighbor radius.
    return vorrq_u32(limits, vmvnq_u32(valid));
}

template <typename Box, typename Stencil>
weight_int one_hop_radius_limit_arm_neon(const OneHopPredecoder::Grid& w, size_t cell) {
    const auto* radii = w.predecode.radii.data() + cell;
    const auto* weights = w.weights[cell].data();
    const auto present = vdupq_n_u32(w.present[cell]);
    const auto a = one_hop_arm_limits4<Box, Stencil, 0>(radii, weights, present);
    const auto b = one_hop_arm_limits4<Box, Stencil, 4>(radii, weights, present);
    const auto c = one_hop_arm_limits4<Box, Stencil, 8>(radii, weights, present);
    return vminvq_u32(vminq_u32(vminq_u32(a, b), c));
}

template <size_t First>
[[gnu::always_inline]] inline uint32x4_t one_hop_arm_weights4(
    const weight_int* weights, uint32x4_t present) {
    const uint32x4_t bits{1U << First, 2U << First, 4U << First, 8U << First};
    return vorrq_u32(vld1q_u32(weights + First), vmvnq_u32(vtstq_u32(present, bits)));
}

[[gnu::always_inline]] inline weight_int one_hop_arm_minimum_weight(
    const weight_int* weights, uint16_t present_mask) {
    const auto present = vdupq_n_u32(present_mask);
    const auto a = one_hop_arm_weights4<0>(weights, present);
    const auto b = one_hop_arm_weights4<4>(weights, present);
    const auto c = one_hop_arm_weights4<8>(weights, present);
    return vminvq_u32(vminq_u32(vminq_u32(a, b), c));
}

}  // namespace beyond_blossom::structured
#endif
#endif
