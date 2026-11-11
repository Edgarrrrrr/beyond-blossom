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

#include "search_detector_node.h"

#include "beyond_blossom/structured/grid/static_grid.h"

namespace beyond_blossom::structured {

size_t SearchDetectorNode::index_of_neighbor(SearchDetectorNode *target) const {
    size_t h = grid_component->index_of_neighbor(grid_cell, target);
    if (h != SIZE_MAX)
        return h;
    throw std::invalid_argument("Failed to find grid neighbor.");
}

void SearchDetectorNode::reset() {
    reached_from_source = nullptr;
    index_of_predecessor = SIZE_MAX;
    node_event_tracker.clear();
}

}  // namespace beyond_blossom::structured
