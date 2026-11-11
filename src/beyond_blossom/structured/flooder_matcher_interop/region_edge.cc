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

#include "beyond_blossom/structured/flooder_matcher_interop/region_edge.h"

namespace beyond_blossom::structured {

bool RegionEdge::operator==(const RegionEdge &rhs) const {
    return region == rhs.region && edge == rhs.edge;
}

bool RegionEdge::operator!=(const RegionEdge &rhs) const {
    return !(rhs == *this);
}

}  // namespace beyond_blossom::structured
