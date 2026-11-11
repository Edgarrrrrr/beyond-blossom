// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include <array>
#include <random>

#include "gtest/gtest.h"

#include "pymatching/sparse_blossom/structured/predecode/one_hop_simd_kernels.h"

namespace {
#if PM_STRUCTURED_STORAGE_DIRECTIONS == 12
template <pm::structured::CircuitStencilKind Kind>
void compare_sparse_caps(const std::array<size_t, 12>& directions) {
    using Stencil = pm::structured::CircuitStencil<Kind>;
    std::mt19937_64 random(20261102 + unsigned(Kind));
    for (size_t width : {5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35}) {
        const size_t source = width * width + (width / 2) * width + width / 2;
        pm::structured::OneHopPredecoder::Grid grid;
        grid.present.resize(source + 1);
        // Keep this row last to catch SIMD loads beyond twelve weights.
        grid.weights.resize(source + 1);
        grid.predecode.radii.resize(3 * width * width);
        for (size_t slot = 0; slot < 12; ++slot) {
            const auto delta = pm::SURFACE_OFFSETS[directions[slot]];
            grid.predecode.offsets[slot] = delta[0] + ptrdiff_t(width) * delta[1] + ptrdiff_t(width * width) * delta[2];
        }
        for (uint16_t present = 0; present < 4096; ++present) {
            grid.present[source] = present;
            for (uint16_t candidates : std::array<uint16_t, 4>{
                     0, present, uint16_t(random() & present), uint16_t(present & uint16_t(-present))}) {
                pm::weight_int expected = UINT32_MAX;
                for (size_t slot = 0; slot < 12; ++slot) {
                    const auto weight = std::array<pm::weight_int, 5>{
                        0, 2, 0x80000000U, UINT32_MAX, pm::weight_int(random())}[(present + slot) % 5];
                    grid.weights[source][slot] = weight;
                    const size_t neighbor = ptrdiff_t(source) + grid.predecode.offsets[slot];
                    const auto radius = candidates & (1 << slot)
                                            ? pm::weight_int(random() % (uint64_t(weight) + 1))
                                            : 0;
                    grid.predecode.radii[neighbor] = present & (1 << slot) ? radius : UINT32_MAX;
                    if (present & (1 << slot))
                        expected = std::min(expected, pm::weight_int(uint64_t(weight) - radius));
                }
                EXPECT_EQ(pm::structured::one_hop_radius_limit_sparse<Stencil>(grid, source, candidates), expected)
                    << "width=" << width << " stencil=" << unsigned(Kind) << " present=" << present
                    << " candidates=" << candidates;
#if PM_PREDECODE_AVX2_AVAILABLE
                if (pm::grid_avx2_available())
                    EXPECT_EQ(pm::structured::one_hop_radius_limit_sparse_avx2(grid, source, candidates), expected)
                        << "width=" << width << " stencil=" << unsigned(Kind) << " present=" << present
                        << " candidates=" << candidates;
#endif
            }
        }
    }
}
#endif
}  // namespace

TEST(OneHopSparseCap, DynamicWeightsAndAllMasksMatchFullRadiusOracle) {
#if PM_STRUCTURED_STORAGE_DIRECTIONS == 12
    compare_sparse_caps<pm::structured::CircuitStencilKind::x>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13});
    compare_sparse_caps<pm::structured::CircuitStencilKind::z>({0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15});
    compare_sparse_caps<pm::structured::CircuitStencilKind::unrotated>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
#else
    GTEST_SKIP() << "This explicit twelve-slot oracle checks the compact layout.";
#endif
}
