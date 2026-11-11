// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include "gtest/gtest.h"

#include "beyond_blossom/structured/predecode/one_hop_neighborhood_simd.h"

namespace {
uint32_t reference_degree(uint16_t bits) {
    uint32_t count = 0;
    for (size_t bit = 0; bit < 16; bit++)
        count += (bits >> bit) & 1U;
    return count;
}

template <size_t NX, beyond_blossom::structured::CircuitStencilKind Kind>
void check_neighborhood(const std::array<size_t, 12>& directions) {
    using Box = beyond_blossom::StaticGridBox<NX, NX, 3>;
    using Stencil = beyond_blossom::structured::CircuitStencil<Kind>;
    constexpr size_t cell = NX * NX + NX + 1;
    beyond_blossom::structured::OneHopPredecoder::Grid grid;
    grid.present.resize(Box::volume);
    auto& workspace = grid;
    workspace.predecode.active.resize(Box::volume);
    for (uint16_t active_bits = 0; active_bits < 4096; active_bits++) {
        SCOPED_TRACE(::testing::Message() << "nx=" << NX << " stencil=" << unsigned(Kind) << " active=" << active_bits);
        uint16_t present = 0;
        uint16_t expected_mask = 0;
        const uint16_t present_bits = ((active_bits << 3) | (active_bits >> 9)) ^ 0x925U;
        for (size_t lane = 0; lane < 12; lane++) {
            const auto direction = directions[lane];
            const auto delta = beyond_blossom::SURFACE_OFFSETS[direction];
            const auto other = ptrdiff_t(cell) + delta[0] + ptrdiff_t(NX) * delta[1] + ptrdiff_t(NX * NX) * delta[2];
            const size_t slot = beyond_blossom::structured::STRUCTURED_STORAGE_DIRECTIONS == 12 ? lane : direction;
            const bool active = active_bits & (1U << lane);
            // Nonzero IDs above INT32_MAX still represent active detectors.
            workspace.predecode.active[other] =
                active ? std::array<uint32_t, 3>{1, 0x80000000U, UINT32_MAX}[lane % 3] : 0;
            if (present_bits & (1U << lane)) {
                present |= uint16_t{1} << slot;
                if (active)
                    expected_mask |= uint16_t{1} << slot;
            }
        }
        // The generated grid keeps high storage bits clear. Deliberately set
        // them here to verify that candidate formation still uses 12 lanes.
        grid.present[cell] = present | (beyond_blossom::structured::STRUCTURED_STORAGE_DIRECTIONS == 12 ? 0xF000U : 0U);
        // Include empty, full, alternating, and overlapping boundary unions.
        for (const uint16_t boundary :
             std::array<uint16_t, 4>{0, present, uint16_t(present & 0x5555U), expected_mask}) {
            const auto expected_degree = reference_degree(expected_mask | boundary);
            const uint32_t expected = expected_mask | (expected_degree << 16);
            const auto portable =
                beyond_blossom::structured::one_hop_neighborhood_portable<Box, Stencil>(workspace, cell, boundary);
            EXPECT_EQ(portable, expected);
            EXPECT_EQ(uint16_t(portable), expected_mask);
#if BB_PREDECODE_AVX2_AVAILABLE && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
            if (beyond_blossom::grid_avx2_available()) {
                EXPECT_EQ(
                    (beyond_blossom::structured::one_hop_neighborhood_avx2_portable<Box, Stencil>(workspace, cell, boundary)),
                    expected);
                if (__builtin_cpu_supports("popcnt"))
                    EXPECT_EQ(
                        (beyond_blossom::structured::one_hop_neighborhood_avx2_popcnt<Box, Stencil>(workspace, cell, boundary)),
                        expected);
            }
#endif
#if BB_PREDECODE_POPCNT_AVAILABLE
            if (__builtin_cpu_supports("popcnt"))
                EXPECT_EQ(
                    (beyond_blossom::structured::one_hop_neighborhood_popcnt<Box, Stencil>(workspace, cell, boundary)), expected);
#endif
        }
    }
}
}  // namespace

TEST(PredecodeNeighborhood, PortableDegreeEnumeratesAllSixteenBitValues) {
    for (uint32_t mask = 0; mask < 65536; mask++)
        EXPECT_EQ(beyond_blossom::structured::one_hop_degree_portable(uint16_t(mask)), reference_degree(uint16_t(mask))) << mask;
}

template <size_t NX>
void check_neighborhood_width() {
    check_neighborhood<NX, beyond_blossom::structured::CircuitStencilKind::x>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13});
    check_neighborhood<NX, beyond_blossom::structured::CircuitStencilKind::z>({0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15});
    check_neighborhood<NX, beyond_blossom::structured::CircuitStencilKind::unrotated>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
}
TEST(PredecodeNeighborhood, EveryActiveMaskAndBoundaryUnionAcrossWidthsAndStencils) {
    check_neighborhood_width<5>();
    check_neighborhood_width<7>();
    check_neighborhood_width<9>();
    check_neighborhood_width<11>();
    check_neighborhood_width<13>();
    check_neighborhood_width<15>();
    check_neighborhood_width<17>();
    check_neighborhood_width<19>();
    check_neighborhood_width<21>();
    check_neighborhood_width<23>();
    check_neighborhood_width<27>();
    check_neighborhood_width<31>();
    check_neighborhood_width<35>();
}
