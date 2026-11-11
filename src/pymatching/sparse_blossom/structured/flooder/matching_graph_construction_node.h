// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_MATCHING_GRAPH_CONSTRUCTION_NODE_H
#define PYMATCHING_STRUCTURED_MATCHING_GRAPH_CONSTRUCTION_NODE_H

#include <vector>

#include "pymatching/sparse_blossom/driver/implied_weights.h"
#include "pymatching/sparse_blossom/ints.h"

namespace pm::structured {

// Temporary adjacency used only while constructing a matching grid. These
// pointers address records in the same fixed-size construction vector; they
// never point at the compact DetectorNode records used by the decoder.
struct MatchingGraphConstructionNode {
    std::vector<MatchingGraphConstructionNode*> neighbors;
    std::vector<weight_int> neighbor_weights;
    std::vector<obs_int> neighbor_observables;
    std::vector<std::vector<ImpliedWeight>> neighbor_implied_weights;
};

}  // namespace pm::structured
#endif
