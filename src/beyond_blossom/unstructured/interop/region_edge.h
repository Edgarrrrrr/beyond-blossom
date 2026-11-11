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

#ifndef BEYOND_BLOSSOM2_REGION_EDGE_H
#define BEYOND_BLOSSOM2_REGION_EDGE_H

#include <tuple>

#include "beyond_blossom/unstructured/interop/compressed_edge.h"

namespace beyond_blossom {

struct GraphFillRegion;

struct RegionEdge {
    GraphFillRegion* region;
    CompressedEdge edge;
    bool operator==(const RegionEdge& rhs) const;
    bool operator!=(const RegionEdge& rhs) const;
};

}  // namespace beyond_blossom

#endif  // BEYOND_BLOSSOM2_REGION_EDGE_H
