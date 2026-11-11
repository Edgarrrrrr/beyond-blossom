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

#ifndef BB_FILL_MATCH_MATCH_H
#define BB_FILL_MATCH_MATCH_H

#include <vector>

#include "beyond_blossom/unstructured/interop/compressed_edge.h"

namespace beyond_blossom {

struct GraphFillRegion;

/// A Match is a partnered graph fill region.
///
/// When one graph fill region is matched to another, it's guaranteed that (if the matching
/// process terminated) two of their detection events will be matched to each other. The
/// specific detection events to match are identified by `edge.loc_from` and `edge.loc_to`.
struct Match {
    /// The region being matched to (or nullptr if matched to the boundary).
    beyond_blossom::GraphFillRegion* region;
    /// A summary of the low-level path from this region to that region, including the
    /// start/end detection events and the observables that were crossed.
    beyond_blossom::CompressedEdge edge;
    bool operator==(const Match& rhs) const;
    bool operator!=(const Match& rhs) const;

    inline void clear() {
        region = nullptr;
        edge.clear();
    }
};

}  // namespace beyond_blossom

#endif  // BB_FILL_MATCH_MATCH_H
