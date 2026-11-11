// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include <array>
#include <random>

#include "gtest/gtest.h"

#include "pymatching/sparse_blossom/structured/predecode/one_hop_simd_kernels.h"
#include "pymatching/sparse_blossom/structured/predecode/one_hop_arm_kernels.h"

#ifndef PM_PREDECODE_SIMD
#define PM_PREDECODE_SIMD 0
#endif

namespace {
#if (PM_PREDECODE_AVX2_AVAILABLE || defined(__aarch64__)) && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
template <size_t Width, pm::structured::CircuitStencilKind Kind>
void compare_caps(const std::array<size_t, 12>& directions) {
    using Box = pm::StaticGridBox<Width, Width, 3>;
    using Stencil = pm::structured::CircuitStencil<Kind>;
    constexpr size_t source = Width * Width + (Width / 2) * Width + Width / 2;
    pm::structured::OneHopPredecoder::Grid grid;
    grid.present.resize(source + 1);
    // The tested row is last in the allocation, so sanitizer builds also check
    // that the final four-lane load does not extend beyond the twelve weights.
    grid.weights.resize(source + 1);
    auto& w = grid;
    w.predecode.radii.resize(Box::volume);
    std::mt19937_64 rng(2026100402 + Width * 3 + unsigned(Kind));
    auto neighbor = [&](size_t slot) {
        const auto d = pm::SURFACE_OFFSETS[directions[slot]];
        return size_t(ptrdiff_t(source) + d[0] + ptrdiff_t(Width) * d[1] + ptrdiff_t(Width * Width) * d[2]);
    };
    auto check = [&] {
        // Independent reference uses explicit global directions and scalar
        // uint64 subtraction, without calling the template slot/offset mapper.
        pm::weight_int expected = UINT32_MAX;
        for (size_t slot = 0; slot < 12; slot++) {
            if (!(grid.present[source] & (uint16_t{1} << slot)))
                continue;
            const auto weight = grid.weights[source][slot];
            const auto radius = w.predecode.radii[neighbor(slot)];
            ASSERT_GE(weight, radius);
            expected = std::min(expected, pm::weight_int(uint64_t(weight) - radius));
        }
        EXPECT_EQ((pm::structured::one_hop_radius_limit<Box, Stencil>(w, source)), expected);
#if PM_PREDECODE_AVX2_AVAILABLE
        EXPECT_EQ((pm::structured::one_hop_radius_limit_avx2<Box, Stencil>(w, source)), expected);
        EXPECT_EQ((pm::structured::one_hop_radius_limit_avx2_3x128<Box, Stencil>(w, source)), expected);
#elif defined(__aarch64__)
        EXPECT_EQ((pm::structured::one_hop_radius_limit_arm_neon<Box, Stencil>(w, source)), expected);
#endif
    };
    for (uint16_t mask = 0; mask < 4096; mask++) {
        SCOPED_TRACE(::testing::Message() << "width=" << Width << " stencil=" << unsigned(Kind) << " mask=" << mask);
        grid.present[source] = mask;
        for (size_t slot = 0; slot < 12; slot++) {
            const pm::weight_int weight =
                std::array<pm::weight_int, 5>{0, 2, 0x80000000U, UINT32_MAX, pm::weight_int(rng())}[(mask + slot) % 5];
            grid.weights[source][slot] = weight;
            // Absent lanes deliberately include underflowing subtractions;
            // present lanes satisfy the kernel's dual-feasibility contract.
            w.predecode.radii[neighbor(slot)] =
                (mask & (uint16_t{1} << slot)) ? pm::weight_int(rng() % (uint64_t(weight) + 1)) : UINT32_MAX;
        }
        check();
    }
    grid.present[source] = 4095;
    for (size_t winner = 0; winner < 12; winner++) {
        for (pm::weight_int minimum : {0U, 2U, 0x80000000U}) {
            SCOPED_TRACE(::testing::Message() << "winner=" << winner << " minimum=" << minimum);
            for (size_t slot = 0; slot < 12; slot++) {
                const auto radius = pm::weight_int(slot + 1);
                w.predecode.radii[neighbor(slot)] = radius;
                grid.weights[source][slot] = radius + (slot == winner ? minimum : 0xf0000000U);
            }
            check();
        }
    }
    // All twelve finite candidates tie, including a high-bit unsigned value.
    for (pm::weight_int minimum : {0U, 0x80000000U}) {
        for (size_t slot = 0; slot < 12; slot++) {
            const auto radius = pm::weight_int(slot + 1);
            w.predecode.radii[neighbor(slot)] = radius;
            grid.weights[source][slot] = radius + minimum;
        }
        check();
    }
}

template <size_t Width>
void compare_stencils() {
    compare_caps<Width, pm::structured::CircuitStencilKind::x>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13});
    compare_caps<Width, pm::structured::CircuitStencilKind::z>({0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15});
    compare_caps<Width, pm::structured::CircuitStencilKind::unrotated>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
}
#endif
}  // namespace

TEST(OneHopCap, BothSimdLayoutsMatchIndependentOracleAtAllRegisteredWidths) {
#if (PM_PREDECODE_AVX2_AVAILABLE || defined(__aarch64__)) && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
#if PM_PREDECODE_AVX2_AVAILABLE
    if (!pm::grid_avx2_available())
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
    pm::structured::MatchingGraph graph(2, 1);
    graph.add_edge(0, 1, 2, {});
    pm::StaticGridSpec spec{3, 3, false, {}, {}, pm::GridSimd::scalar};
    spec.coordinates = {{{0, 0, 0}}, {{2, 0, 0}}};
    graph.configure_grid(spec);
    pm::structured::OneHopPredecoder decoder(graph);
    ASSERT_EQ(graph.grid->components.size(), 1U);
    const auto& w = *graph.grid->components[0];
    ASSERT_EQ(w.nx, 5U);
    using Box = pm::StaticGridBox<5, 5, 1>;
    ASSERT_EQ(w.stencil_kind, pm::structured::CircuitStencilKind::x);
    using Stencil = pm::structured::CircuitStencil<pm::structured::CircuitStencilKind::x>;
    auto expected = pm::structured::one_hop_radius_limit<Box, Stencil>;
#if PM_PREDECODE_SIMD && defined(__aarch64__) && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
    expected = pm::structured::one_hop_radius_limit_arm_neon<Box, Stencil>;
#endif
#if PM_PREDECODE_SIMD && PM_PREDECODE_AVX2_AVAILABLE && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
    if (pm::grid_avx2_available()) {
#if PM_PREDECODE_CAP_3X128
        expected = pm::structured::one_hop_radius_limit_avx2_3x128<Box, Stencil>;
#else
        expected = pm::structured::one_hop_radius_limit_avx2<Box, Stencil>;
#endif
    }
#endif
    EXPECT_EQ(w.predecode.radius_limit, expected);
}
