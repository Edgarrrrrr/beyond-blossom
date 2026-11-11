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

#include "beyond_blossom/structured/matcher/alternating_tree.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "beyond_blossom/structured/flooder/graph_fill_region.h"

namespace beyond_blossom::structured {

bool AltTreeNode::operator==(const AltTreeNode &rhs) const {
    return tree_equal(rhs);
}

bool AltTreeNode::operator!=(const AltTreeNode &rhs) const {
    return !(rhs == *this);
}

bool AltTreeNode::tree_equal(const AltTreeNode &other) const {
    if (region_pool != other.region_pool || inner_region != other.inner_region || outer_region != other.outer_region ||
        inner_to_outer_edge != other.inner_to_outer_edge || children.size() != other.children.size()) {
        return false;
    }
    if (children.empty())
        return true;
    for (size_t i = 0; i < children.size(); i++) {
        if (children[i].edge != other.children[i].edge)
            return false;
        if (!children[i].alt_tree_node->tree_equal(*other.children[i].alt_tree_node))
            return false;
    }
    return true;
}

std::vector<AltTreeNode *> AltTreeNode::all_nodes_in_tree() {
    std::vector<AltTreeNode *> all_nodes;
    size_t k = 0;
    all_nodes.push_back(this);
    while (k < all_nodes.size()) {
        for (auto &c : all_nodes[k]->children) {
            all_nodes.push_back(c.alt_tree_node);
        }
        k++;
    }
    return all_nodes;
}

size_t AltTreeNode::count_nodes_in_tree() const {
    std::vector<const AltTreeNode *> all_nodes;
    size_t k = 0;
    all_nodes.push_back(this);
    while (k < all_nodes.size()) {
        for (const auto &c : all_nodes[k]->children) {
            all_nodes.push_back(c.alt_tree_node);
        }
        k++;
    }
    return all_nodes.size();
}

}  // namespace beyond_blossom::structured
