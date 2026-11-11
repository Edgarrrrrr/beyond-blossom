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

#ifndef BB_CORE_OBSERVABLE_BITS_H
#define BB_CORE_OBSERVABLE_BITS_H

#include <cstddef>
#include <stdexcept>
#include <vector>
#include "beyond_blossom/core/ints.h"

namespace beyond_blossom {

inline void fill_bit_vector_from_obs_mask(obs_int mask, uint8_t* output, size_t count) {
    if (count > sizeof(obs_int) * 8)
        throw std::invalid_argument("Too many observables");
    for (size_t k = 0; k < count; ++k)
        output[k] ^= static_cast<uint8_t>((mask >> k) & obs_int{1});
}

inline obs_int bit_vector_to_obs_mask(const std::vector<uint8_t>& bits) {
    if (bits.size() > sizeof(obs_int) * 8)
        throw std::invalid_argument("Too many observables");
    obs_int mask = 0;
    for (size_t k = 0; k < bits.size(); ++k)
        mask ^= static_cast<obs_int>(bits[k]) << k;
    return mask;
}

}  // namespace beyond_blossom
#endif
