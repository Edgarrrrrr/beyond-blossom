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

#ifndef BEYOND_BLOSSOM2_GRAPH_FILL_REGION_TEST_H
#define BEYOND_BLOSSOM2_GRAPH_FILL_REGION_TEST_H

#include "beyond_blossom/core/arena.h"
#include "beyond_blossom/unstructured/flooder/graph.h"
#include "beyond_blossom/unstructured/flooder/graph_fill_region.h"

namespace beyond_blossom {
struct GraphFillTestData {
    std::vector<DetectorNode> detectors;
    Arena<GraphFillRegion> arena;
    explicit GraphFillTestData(int num_elements) {
        detectors.resize(num_elements);
    };
    RegionEdge b(int loc_from, int loc_to, std::vector<RegionEdge> edges, bool root = false) {
        auto r = arena.alloc_default_constructed();
        r->blossom_children = std::move(edges);
        for (auto c : r->blossom_children) {
            c.region->wrap_into_blossom(r);
        }
        auto ce =
            root ? CompressedEdge{nullptr, nullptr, 0} : CompressedEdge{&detectors[loc_from], &detectors[loc_to], 0};
        auto region_edge = RegionEdge{r, ce};
        return region_edge;
    }
};

}  // namespace beyond_blossom

#endif  // BEYOND_BLOSSOM2_GRAPH_FILL_REGION_TEST_H
