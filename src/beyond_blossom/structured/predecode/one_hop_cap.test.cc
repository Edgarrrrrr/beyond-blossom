// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include <array>
#include <random>

#include "gtest/gtest.h"

#include "beyond_blossom/structured/predecode/one_hop_simd_kernels.h"
#include "beyond_blossom/structured/predecode/one_hop_arm_kernels.h"

#ifndef BB_PREDECODE_SIMD
#define BB_PREDECODE_SIMD 0
#endif

namespace {
#if (BB_PREDECODE_AVX2_AVAILABLE || defined(__aarch64__)) && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
template <size_t Width, beyond_blossom::structured::CircuitStencilKind Kind>
void compare_caps(const std::array<size_t, 12>& directions) {
    using Box = beyond_blossom::StaticGridBox<Width, Width, 3>;
    using Stencil = beyond_blossom::structured::CircuitStencil<Kind>;
    constexpr size_t source = Width * Width + (Width / 2) * Width + Width / 2;
    beyond_blossom::structured::OneHopPredecoder::Grid grid;
    grid.present.resize(source + 1);
    // The tested row is last in the allocation, so sanitizer builds also check
    // that the final four-lane load does not extend beyond the twelve weights.
    grid.weights.resize(source + 1);
    auto& w = grid;
    w.predecode.radii.resize(Box::volume);
    std::mt19937_64 rng(2026100402 + Width * 3 + unsigned(Kind));
    auto neighbor = [&](size_t slot) {
        const auto d = beyond_blossom::SURFACE_OFFSETS[directions[slot]];
        return size_t(ptrdiff_t(source) + d[0] + ptrdiff_t(Width) * d[1] + ptrdiff_t(Width * Width) * d[2]);
    };
    auto check = [&] {
        // Independent reference uses explicit global directions and scalar
        // uint64 subtraction, without calling the template slot/offset mapper.
        beyond_blossom::weight_int expected = UINT32_MAX;
        for (size_t slot = 0; slot < 12; slot++) {
            if (!(grid.present[source] & (uint16_t{1} << slot)))
                continue;
            const auto weight = grid.weights[source][slot];
            const auto radius = w.predecode.radii[neighbor(slot)];
            ASSERT_GE(weight, radius);
            expected = std::min(expected, beyond_blossom::weight_int(uint64_t(weight) - radius));
        }
        EXPECT_EQ((beyond_blossom::structured::one_hop_radius_limit<Box, Stencil>(w, source)), expected);
#if BB_PREDECODE_AVX2_AVAILABLE
        EXPECT_EQ((beyond_blossom::structured::one_hop_radius_limit_avx2<Box, Stencil>(w, source)), expected);
        EXPECT_EQ((beyond_blossom::structured::one_hop_radius_limit_avx2_3x128<Box, Stencil>(w, source)), expected);
#elif defined(__aarch64__)
        EXPECT_EQ((beyond_blossom::structured::one_hop_radius_limit_arm_neon<Box, Stencil>(w, source)), expected);
#endif
    };
    for (uint16_t mask = 0; mask < 4096; mask++) {
        SCOPED_TRACE(::testing::Message() << "width=" << Width << " stencil=" << unsigned(Kind) << " mask=" << mask);
        grid.present[source] = mask;
        for (size_t slot = 0; slot < 12; slot++) {
            const beyond_blossom::weight_int weight =
                std::array<beyond_blossom::weight_int, 5>{0, 2, 0x80000000U, UINT32_MAX, beyond_blossom::weight_int(rng())}[(mask + slot) % 5];
            grid.weights[source][slot] = weight;
            // Absent lanes deliberately include underflowing subtractions;
            // present lanes satisfy the kernel's dual-feasibility contract.
            w.predecode.radii[neighbor(slot)] =
                (mask & (uint16_t{1} << slot)) ? beyond_blossom::weight_int(rng() % (uint64_t(weight) + 1)) : UINT32_MAX;
        }
        check();
    }
    grid.present[source] = 4095;
    for (size_t winner = 0; winner < 12; winner++) {
        for (beyond_blossom::weight_int minimum : {0U, 2U, 0x80000000U}) {
            SCOPED_TRACE(::testing::Message() << "winner=" << winner << " minimum=" << minimum);
            for (size_t slot = 0; slot < 12; slot++) {
                const auto radius = beyond_blossom::weight_int(slot + 1);
                w.predecode.radii[neighbor(slot)] = radius;
                grid.weights[source][slot] = radius + (slot == winner ? minimum : 0xf0000000U);
            }
            check();
        }
    }
    // All twelve finite candidates tie, including a high-bit unsigned value.
    for (beyond_blossom::weight_int minimum : {0U, 0x80000000U}) {
        for (size_t slot = 0; slot < 12; slot++) {
            const auto radius = beyond_blossom::weight_int(slot + 1);
            w.predecode.radii[neighbor(slot)] = radius;
            grid.weights[source][slot] = radius + minimum;
        }
        check();
    }
}

template <size_t Width>
void compare_stencils() {
    compare_caps<Width, beyond_blossom::structured::CircuitStencilKind::x>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13});
    compare_caps<Width, beyond_blossom::structured::CircuitStencilKind::z>({0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15});
    compare_caps<Width, beyond_blossom::structured::CircuitStencilKind::unrotated>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
}
#endif
}  // namespace

TEST(OneHopCap, BothSimdLayoutsMatchIndependentOracleAtAllRegisteredWidths) {
#if (BB_PREDECODE_AVX2_AVAILABLE || defined(__aarch64__)) && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
#if BB_PREDECODE_AVX2_AVAILABLE
    if (!beyond_blossom::grid_avx2_available())
        GTEST_SKIP() << "AVX2 is unavailable on this CPU.";
#endif
    compare_stencils<5>();
    compare_stencils<7>();
    compare_stencils<9>();
    compare_stencils<11>();
    compare_stencils<13>();
    compare_stencils<15>();
    compare_stencils<17>();
    compare_stencils<19>();
    compare_stencils<21>();
    compare_stencils<23>();
    compare_stencils<25>();
    compare_stencils<27>();
    compare_stencils<31>();
    compare_stencils<35>();
#else
    GTEST_SKIP() << "The SIMD cap layouts require x86 AVX2 or ARM NEON and twelve-slot storage.";
#endif
}

TEST(OneHopCap, ConstructionBindsConfiguredLayoutOrScalarFallback) {
    beyond_blossom::structured::MatchingGraph graph(2, 1);
    graph.add_edge(0, 1, 2, {});
    beyond_blossom::StaticGridSpec spec{3, 3, false, {}, {}, beyond_blossom::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{2, 0, 0}}};
    graph.configure_grid(spec);
    beyond_blossom::structured::OneHopPredecoder decoder(graph);
    ASSERT_EQ(graph.grid->components.size(), 1U);
    const auto& w = *graph.grid->components[0];
    ASSERT_EQ(w.nx, 5U);
    using Box = beyond_blossom::StaticGridBox<5, 5, 1>;
    ASSERT_EQ(w.stencil_kind, beyond_blossom::structured::CircuitStencilKind::x);
    using Stencil = beyond_blossom::structured::CircuitStencil<beyond_blossom::structured::CircuitStencilKind::x>;
    auto expected = beyond_blossom::structured::one_hop_radius_limit<Box, Stencil>;
#if BB_PREDECODE_SIMD && defined(__aarch64__) && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
    expected = beyond_blossom::structured::one_hop_radius_limit_arm_neon<Box, Stencil>;
#endif
#if BB_PREDECODE_SIMD && BB_PREDECODE_AVX2_AVAILABLE && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
    if (beyond_blossom::grid_avx2_available()) {
#if BB_PREDECODE_CAP_3X128
        expected = beyond_blossom::structured::one_hop_radius_limit_avx2_3x128<Box, Stencil>;
#else
        expected = beyond_blossom::structured::one_hop_radius_limit_avx2<Box, Stencil>;
#endif
    }
#endif
    EXPECT_EQ(w.predecode.radius_limit, expected);
}
