// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/core/matching_result.h"
#include "beyond_blossom/core/observable_bits.h"
#include "gtest/gtest.h"

TEST(MatchingResult, IndependentComponentComposition) {
    constexpr beyond_blossom::MatchingResult a{0b101, -3}, b{0b110, 7};
    static_assert(a + b == beyond_blossom::MatchingResult{0b011, 4});
    beyond_blossom::ExtendedMatchingResult wide(std::vector<uint8_t>{1, 0, 1}, -3);
    wide += beyond_blossom::ExtendedMatchingResult(std::vector<uint8_t>{1, 1, 0}, 7);
    EXPECT_EQ(wide, (beyond_blossom::ExtendedMatchingResult(std::vector<uint8_t>{0, 1, 1}, 4)));
    wide.reset();
    EXPECT_EQ(wide, beyond_blossom::ExtendedMatchingResult(3));
}

TEST(ObservableBits, RoundTripAllBitsAndXorExistingOutput) {
    for (size_t bit = 0; bit < 64; ++bit) {
        const auto mask = beyond_blossom::obs_int{1} << bit;
        std::vector<uint8_t> values(64);
        beyond_blossom::fill_bit_vector_from_obs_mask(mask, values.data(), values.size());
        EXPECT_EQ(beyond_blossom::bit_vector_to_obs_mask(values), mask);
        beyond_blossom::fill_bit_vector_from_obs_mask(mask, values.data(), values.size());
        EXPECT_EQ(beyond_blossom::bit_vector_to_obs_mask(values), 0);
    }
    std::vector<uint8_t> too_wide(65);
    EXPECT_THROW(beyond_blossom::bit_vector_to_obs_mask(too_wide), std::invalid_argument);
    EXPECT_THROW(beyond_blossom::fill_bit_vector_from_obs_mask(0, too_wide.data(), 65), std::invalid_argument);
}
