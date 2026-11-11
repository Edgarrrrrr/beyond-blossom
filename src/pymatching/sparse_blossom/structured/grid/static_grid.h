// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_STATIC_GRID_H
#define PYMATCHING_STRUCTURED_STATIC_GRID_H
#include "pymatching/sparse_blossom/grid/grid_spec.h"
#include "pymatching/sparse_blossom/structured/grid/circuit_stencil.h"
namespace pm::structured {
struct alignas(STRUCTURED_STORAGE_DIRECTIONS == 12 ? 16 : 64) StructuredGridWeightRow {
    std::array<weight_int, STRUCTURED_STORAGE_DIRECTIONS> values{};
    weight_int& operator[](size_t slot) {
        return values[slot];
    }
    const weight_int& operator[](size_t slot) const {
        return values[slot];
    }
    const weight_int* data() const {
        return values.data();
    }
};
static_assert(sizeof(StructuredGridWeightRow) == STRUCTURED_STORAGE_DIRECTIONS * sizeof(weight_int));
static_assert(alignof(StructuredGridWeightRow) == (STRUCTURED_STORAGE_DIRECTIONS == 12 ? 16 : 64));

struct OneHopBoundRange;
class RegionPool;
class DetectorNode;
class SearchDetectorNode;
struct MatchingGraphConstructionNode;
template <typename Node, typename Observable>
struct StaticGridComponent {
    // Matching nodes are compact, real-detector-only records. Search nodes keep
    // their existing cell-indexed layout; its scan has different state needs.
    RegionPool* region_pool = nullptr;
    std::vector<Node> nodes;
    std::vector<GridCollisionState> collision_states;
    std::vector<uint32_t> cell_node_indices;
    std::vector<GridNodeKind> kinds;
    std::vector<uint16_t> present;
    std::vector<StructuredGridWeightRow> weights;
    std::vector<std::array<Observable, STRUCTURED_STORAGE_DIRECTIONS>> observables;
    // Topology shared by predecode, neighbor lookup, and shortest-path search.
    std::vector<uint16_t> boundary_mask;
    struct PredecodeState {
        void (*run_bound_range)(OneHopBoundRange&) = nullptr;
        std::vector<uint32_t> active;
        std::vector<weight_int> radii;
        std::array<ptrdiff_t, STRUCTURED_STORAGE_DIRECTIONS> offsets{};
        uint32_t (*neighborhood)(const StaticGridComponent&, size_t, uint16_t) = nullptr;
        weight_int (*radius_limit)(const StaticGridComponent&, size_t) = nullptr;
        weight_int (*sparse_radius_limit)(const StaticGridComponent&, size_t, uint16_t) = nullptr;
    } predecode;
    // Construction-bound owner for the global component decoder's shot store.
    uint32_t decoder_component = 0;
    std::vector<std::array<uint8_t, STRUCTURED_STORAGE_DIRECTIONS>> markers;
    std::unordered_map<size_t, std::vector<ImpliedWeight>> implied_weights;
    size_t nx = 0, ny = 0, nt = 0;
    uint32_t group = 0;
    GridSimd selected_simd = GridSimd::scalar;
    CircuitStencilKind stencil_kind = CircuitStencilKind::x;

    // Dispatch once per node. The instantiated scan uses compile-time offsets.
    GridNextEvent (*matching_next)(const StaticGridComponent&, const Node&, const VaryingCT*) = nullptr;
    GridNextEvent (*search_next)(const StaticGridComponent&, const Node&, cumulative_time_int, bool) = nullptr;
    ptrdiff_t (*selected_offset)(size_t) = nullptr;

    const Node& node_at_cell(size_t cell) const {
        if constexpr (std::is_same_v<Node, DetectorNode>)
            return nodes[cell_node_indices[cell]];
        else
            return nodes[cell];
    }

    Node* neighbor(size_t cell, size_t d) const {
        if (d >= STRUCTURED_STORAGE_DIRECTIONS || !(present[cell] & (uint16_t{1} << d)))
            throw std::out_of_range("Missing grid edge.");
        if (boundary_mask[cell] & (uint16_t{1} << d))
            return nullptr;
        size_t other = ptrdiff_t(cell) + selected_offset(d);
        return const_cast<Node*>(&node_at_cell(other));
    }
    size_t index_of_neighbor(size_t cell, const Node* other) const {
        uint16_t mask = present[cell];
        while (mask) {
            size_t d = std::countr_zero(mask);
            mask &= mask - 1;
            if (neighbor(cell, d) == other)
                return d;
        }
        return SIZE_MAX;
    }
};

template <typename Node, typename Observable>
struct StaticGridStorage {
    std::vector<std::unique_ptr<StaticGridComponent<Node, Observable>>> components;
    std::vector<Node*> detector_nodes;
};

template <typename Node, typename Observable, typename BuildNode>
std::unique_ptr<StaticGridStorage<Node, Observable>> build_static_grid(
    std::vector<BuildNode>& original, const StaticGridSpec& spec);

extern template std::unique_ptr<StaticGridStorage<DetectorNode, obs_int>>
build_static_grid<DetectorNode, obs_int, MatchingGraphConstructionNode>(
    std::vector<MatchingGraphConstructionNode>&, const StaticGridSpec&);
extern template std::unique_ptr<StaticGridStorage<SearchDetectorNode, std::vector<size_t>>>
build_static_grid<SearchDetectorNode, std::vector<size_t>, SearchDetectorNode>(
    std::vector<SearchDetectorNode>&, const StaticGridSpec&);

}  // namespace pm::structured
#endif
