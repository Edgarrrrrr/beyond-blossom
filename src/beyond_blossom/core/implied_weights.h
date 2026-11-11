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

#ifndef BEYOND_BLOSSOM2_IMPLIED_WEIGHT_UNCONVERTED_H
#define BEYOND_BLOSSOM2_IMPLIED_WEIGHT_UNCONVERTED_H

#include "beyond_blossom/core/ints.h"

namespace beyond_blossom {

struct ImpliedWeightUnconverted {
    size_t node1;
    size_t node2;
    double implied_weight;
    bool operator==(const ImpliedWeightUnconverted& other) const {
        return (this->node1 == other.node1) && (this->node2 == other.node2) &&
               (this->implied_weight == other.implied_weight);
    }
};

struct ImpliedWeight {
    weight_int* edge0_ptr;
    weight_int* edge1_ptr;
    weight_int implied_weight;
};

}  // namespace beyond_blossom

#endif  // BEYOND_BLOSSOM2_IMPLIED_WEIGHT_UNCONVERTED_H
