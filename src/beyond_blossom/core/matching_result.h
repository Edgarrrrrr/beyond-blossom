// Copyright 2022 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BB_CORE_MATCHING_RESULT_H
#define BB_CORE_MATCHING_RESULT_H

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>
#include "beyond_blossom/core/ints.h"

namespace beyond_blossom {

// Value types shared by both decoding backends. Combining independent
// components XORs their observable parities and adds their objective weights.
struct MatchingResult {
    obs_int obs_mask = 0;
    total_weight_int weight = 0;

    constexpr MatchingResult() = default;
    constexpr MatchingResult(obs_int observables, total_weight_int objective)
        : obs_mask(observables), weight(objective) {}
    constexpr bool operator==(const MatchingResult&) const = default;
    constexpr MatchingResult& operator+=(const MatchingResult& rhs) {
        obs_mask ^= rhs.obs_mask;
        weight += rhs.weight;
        return *this;
    }
    constexpr MatchingResult operator+(const MatchingResult& rhs) const {
        auto result = *this;
        result += rhs;
        return result;
    }
};

struct ExtendedMatchingResult {
    std::vector<uint8_t> obs_crossed;
    total_weight_int weight = 0;

    ExtendedMatchingResult() = default;
    explicit ExtendedMatchingResult(size_t count) : obs_crossed(count, 0) {}
    ExtendedMatchingResult(std::vector<uint8_t> observables, total_weight_int objective)
        : obs_crossed(std::move(observables)), weight(objective) {}
    bool operator==(const ExtendedMatchingResult&) const = default;
    void reset() {
        std::fill(obs_crossed.begin(), obs_crossed.end(), 0);
        weight = 0;
    }
    ExtendedMatchingResult& operator+=(const ExtendedMatchingResult& rhs) {
        assert(obs_crossed.size() == rhs.obs_crossed.size());
        for (size_t k = 0; k < obs_crossed.size(); ++k)
            obs_crossed[k] ^= rhs.obs_crossed[k];
        weight += rhs.weight;
        return *this;
    }
    ExtendedMatchingResult operator+(const ExtendedMatchingResult& rhs) const {
        auto result = *this;
        result += rhs;
        return result;
    }
};

}  // namespace beyond_blossom
#endif
