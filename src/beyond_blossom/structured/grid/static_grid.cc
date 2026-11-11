// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#include <algorithm>
#include <map>

#include "beyond_blossom/structured/flooder/matching_graph_construction_node.h"
#include "beyond_blossom/structured/grid/static_grid_kernels.h"
#include "beyond_blossom/structured/predecode/one_hop_predecoder.h"

namespace beyond_blossom {
bool grid_avx512_available() {
#if BB_GRID_AVX512
    return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq") &&
           __builtin_cpu_supports("avx512vl");
#else
    return false;
#endif
}

}

namespace beyond_blossom::structured {


template <typename Box, typename Stencil, size_t... D>
ptrdiff_t select_offset(size_t slot, std::index_sequence<D...>) {
    ptrdiff_t result = 0;
    ((slot == CircuitStorage<Stencil>::template slot<D> ? result = Box::template offset<D>() : 0), ...);
    return result;
}

template <typename Box, typename Stencil, typename Node, typename Observable>
void configure_component(StaticGridComponent<Node, Observable>& c, GridSimd simd) {
    c.nx = Box::nx;
    c.ny = Box::ny;
    c.nt = Box::nt;
    c.kinds.resize(Box::volume, GridNodeKind::absent);
    c.present.resize(Box::volume, 0);
    c.weights.resize(Box::volume);
    c.observables.resize(Box::volume);
    c.boundary_mask.resize(Box::volume);
    if constexpr (std::is_same_v<Node, SearchDetectorNode>) {
        c.nodes.resize(Box::volume);
        c.markers.resize(Box::volume);
        c.search_next = grid_search<Box, Stencil>;
    } else {
        c.collision_states.resize(Box::volume);
        c.cell_node_indices.resize(Box::volume, UINT32_MAX);
        c.matching_next = grid_matching_scalar<Box, Stencil>;
        if (simd == GridSimd::avx512 && !grid_avx512_available())
            throw std::invalid_argument("AVX-512 grid kernel is unavailable on this CPU/compiler.");
        if (simd == GridSimd::avx2 && !grid_avx2_available())
            throw std::invalid_argument("AVX2 grid kernel is unavailable on this CPU/compiler.");
        if (simd == GridSimd::avx512vl && !grid_avx512vl_available())
            throw std::invalid_argument("AVX-512VL grid kernel is unavailable on this CPU/compiler.");
#if BB_GRID_AVX512
        if (simd == GridSimd::avx512 && grid_avx512_available()) {
            c.matching_next = grid_matching_avx512<Box, Stencil, GridCellAccess::constructed>;
            c.selected_simd = GridSimd::avx512;
        }
#endif
#if BB_GRID_AVX2
        if (simd == GridSimd::avx2) {
            c.matching_next = grid_matching_avx2<Box, Stencil, GridCellAccess::constructed>;
            c.selected_simd = GridSimd::avx2;
        }
#endif
#if BB_GRID_AVX512VL
        if (simd == GridSimd::avx512vl) {
            c.matching_next = grid_matching_avx512vl<Box, Stencil, GridCellAccess::constructed>;
            c.selected_simd = GridSimd::avx512vl;
        }
#endif
    }
    c.selected_offset = [](size_t d) {
        return select_offset<Box, Stencil>(d, typename Stencil::Directions{});
    };
    for (size_t cell = 0; cell < Box::volume; cell++) {
        if constexpr (std::is_same_v<Node, SearchDetectorNode>) {
            c.nodes[cell].grid_component = &c;
            c.nodes[cell].grid_cell = cell;
        }
    }
}

template <size_t Distance, typename Stencil, typename Node, typename Observable>
void configure_distance(StaticGridComponent<Node, Observable>& c, const StaticGridSpec& spec) {
    if (spec.rounds == Distance)
        configure_component<StaticGridBox<Distance + 2, Distance + 2, Distance + 3>, Stencil>(c, spec.simd);
    else if (spec.rounds == 3 * Distance)
        configure_component<StaticGridBox<Distance + 2, Distance + 2, 3 * Distance + 3>, Stencil>(c, spec.simd);
    else
        throw std::invalid_argument("Static grid registry supports rounds=d or rounds=3*d.");
}

template <typename Stencil, typename Node, typename Observable>
void configure_registered(StaticGridComponent<Node, Observable>& c, const StaticGridSpec& spec) {
    switch (spec.distance) {
        case 3:
            return configure_distance<3, Stencil>(c, spec);
        case 5:
            return configure_distance<5, Stencil>(c, spec);
        case 7:
            return configure_distance<7, Stencil>(c, spec);
        case 9:
            return configure_distance<9, Stencil>(c, spec);
        case 11:
            return configure_distance<11, Stencil>(c, spec);
        case 13:
            return configure_distance<13, Stencil>(c, spec);
        case 15:
            return configure_distance<15, Stencil>(c, spec);
        case 17:
            return configure_distance<17, Stencil>(c, spec);
        case 19:
            return configure_distance<19, Stencil>(c, spec);
        case 21:
            return configure_distance<21, Stencil>(c, spec);
        case 23:
            return configure_distance<23, Stencil>(c, spec);
        case 25:
            return configure_distance<25, Stencil>(c, spec);
        case 29:
            return configure_distance<29, Stencil>(c, spec);
        case 33:
            return configure_distance<33, Stencil>(c, spec);
        default:
            throw std::invalid_argument("Static grid registry supports odd distances 3 through 25, 29, and 33.");
    }
}

// These references point at compile-time tables; no mapping table is retained
// in a component after graph construction.
struct CircuitConstructionStencil {
    const std::array<size_t, CIRCUIT_SCAN_DIRECTIONS>& boundary_directions;
    const std::array<size_t, GRID_DIRECTIONS>& global_to_slot;
};

template <typename Node, typename Observable>
CircuitConstructionStencil configure_stencil(
    StaticGridComponent<Node, Observable>& c, const StaticGridSpec& spec, uint16_t ordinary_directions) {
    c.stencil_kind = circuit_stencil_for_mask(ordinary_directions);
    switch (c.stencil_kind) {
        case CircuitStencilKind::x:
            configure_registered<CircuitStencil<CircuitStencilKind::x>>(c, spec);
            return {CircuitStencil<CircuitStencilKind::x>::boundary_directions,
                    CircuitStorage<CircuitStencil<CircuitStencilKind::x>>::global_to_slot};
        case CircuitStencilKind::z:
            configure_registered<CircuitStencil<CircuitStencilKind::z>>(c, spec);
            return {CircuitStencil<CircuitStencilKind::z>::boundary_directions,
                    CircuitStorage<CircuitStencil<CircuitStencilKind::z>>::global_to_slot};
        case CircuitStencilKind::unrotated:
            configure_registered<CircuitStencil<CircuitStencilKind::unrotated>>(c, spec);
            return {CircuitStencil<CircuitStencilKind::unrotated>::boundary_directions,
                    CircuitStorage<CircuitStencil<CircuitStencilKind::unrotated>>::global_to_slot};
    }
    throw std::logic_error("Invalid circuit stencil.");
}

template <typename Node, typename Observable, typename BuildNode>
std::unique_ptr<StaticGridStorage<Node, Observable>> build_static_grid(
    std::vector<BuildNode>& original, const StaticGridSpec& spec) {
    const size_t count = original.size();
    if (!count || spec.coordinates.size() != count || count >= (INT32_MAX - 2) / 2 ||
        (!spec.groups.empty() && spec.groups.size() != count))
        throw std::invalid_argument("Static grid requires coordinates and optional groups for every detector.");
    std::vector<size_t> roots(count);
    std::iota(roots.begin(), roots.end(), 0);
    auto find = [&](size_t u) {
        while (u != roots[u]) {
            roots[u] = roots[roots[u]];
            u = roots[u];
        }
        return u;
    };
    for (size_t u = 0; u < count; u++) {
        if constexpr (std::is_same_v<BuildNode, Node>) {
            if (original[u].reached_from_source || original[u].grid_component)
                throw std::logic_error("Configure the static grid before decoding starts.");
        }
        for (const auto& rules : original[u].neighbor_implied_weights)
            if (!rules.empty())
                throw std::logic_error("Configure the static grid before converting implied weight pointers.");
        for (auto* v : original[u].neighbors) {
            if (!v)
                continue;
            size_t j = v - original.data();
            if (j >= count || j == u)
                throw std::invalid_argument("Invalid ordinary grid edge.");
            if (!spec.groups.empty() && spec.groups[u] != spec.groups[j])
                throw std::invalid_argument("An ordinary edge connects different X/Z groups.");
            size_t a = find(u), b = find(j);
            roots[std::max(a, b)] = std::min(a, b);
        }
    }
    std::map<size_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < count; i++)
        groups[spec.groups.empty() ? find(i) : spec.groups[i]].push_back(i);
    if (groups.size() > 2)
        throw std::invalid_argument("Static surface grid needs at most two groups; supply explicit X/Z labels.");

    auto result = std::make_unique<StaticGridStorage<Node, Observable>>();
    result->detector_nodes.resize(count);
    std::vector<std::array<int64_t, 3>> transformed(count);
    for (size_t i = 0; i < count; i++) {
        auto p = spec.coordinates[i];
        for (auto v : p)
            if (v < -(int64_t{1} << 30) || v > (int64_t{1} << 30))
                throw std::invalid_argument("Static grid coordinate is out of range.");
        transformed[i] = spec.rotated ? std::array<int64_t, 3>{p[0] + p[1], p[1] - p[0], p[2]} : p;
    }
    for (const auto& [label, ids] : groups) {
        auto component = std::make_unique<StaticGridComponent<Node, Observable>>();
        auto& c = *component;
        if constexpr (std::is_same_v<Node, DetectorNode>)
            c.nodes.reserve(ids.size());
        c.group = static_cast<uint32_t>(label);
        auto origin = transformed[ids[0]];
        for (size_t id : ids)
            for (size_t k = 0; k < 3; k++)
                origin[k] = std::min(origin[k], transformed[id][k]);
        std::vector<std::array<int64_t, 3>> positions(count);
        const std::array<int64_t, 3> scale{spec.rotated ? 4 : 2, spec.rotated ? 4 : 2, 1};
        for (size_t id : ids) {
            auto p = transformed[id];
            for (size_t k = 0; k < 3; k++) {
                if ((p[k] - origin[k]) % scale[k])
                    throw std::invalid_argument("Detector coordinates do not fit the selected lattice embedding.");
                p[k] = (p[k] - origin[k]) / scale[k] + 1;
            }
            positions[id] = p;
        }
        uint16_t ordinary_directions = 0;
        for (size_t id : ids) {
            for (const auto* other : original[id].neighbors) {
                if (!other)
                    continue;
                size_t j = other - original.data();
                std::array<int, 3> delta;
                for (size_t k = 0; k < 3; k++) {
                    int64_t step = positions[j][k] - positions[id][k];
                    if (step < -1 || step > 1)
                        throw std::invalid_argument("Edge is outside the static surface stencil.");
                    delta[k] = int(step);
                }
                auto it = std::find(SURFACE_OFFSETS.begin(), SURFACE_OFFSETS.end(), delta);
                if (it == SURFACE_OFFSETS.end())
                    throw std::invalid_argument("Edge direction is outside the static surface stencil.");
                ordinary_directions |= uint16_t{1} << (it - SURFACE_OFFSETS.begin());
            }
        }
        const auto stencil = configure_stencil(c, spec, ordinary_directions);
        for (size_t id : ids) {
            const auto& p = positions[id];
            if (p[0] >= int64_t(c.nx - 1) || p[1] >= int64_t(c.ny - 1) || p[2] >= int64_t(c.nt - 1))
                throw std::invalid_argument("Detector coordinates exceed the selected static shape.");
            size_t cell = p[0] + c.nx * (p[1] + c.ny * p[2]);
            if (c.kinds[cell] != GridNodeKind::absent)
                throw std::invalid_argument("Two detectors occupy one grid cell.");
            c.kinds[cell] = GridNodeKind::detector;
            Node* node;
            if constexpr (std::is_same_v<Node, DetectorNode>) {
                c.cell_node_indices[cell] = c.nodes.size();
                node = &c.nodes.emplace_back();
            } else {
                node = &c.nodes[cell];
            }
            node->grid_component = &c;
            node->grid_cell = cell;
            node->detector_id = id;
            result->detector_nodes[id] = node;
        }
        auto install = [&](size_t id, size_t direction, size_t rank) {
            size_t d = stencil.global_to_slot[direction];
            if (d == SIZE_MAX)
                throw std::invalid_argument("Edge direction is outside the selected circuit stencil.");
            auto& old = original[id];
            size_t cell = result->detector_nodes[id]->grid_cell;
            if (c.present[cell] & (uint16_t{1} << d))
                throw std::invalid_argument("Multiple edges occupy one stencil direction.");
            if (rank >= UINT8_MAX)
                throw std::invalid_argument("Too many original grid neighbors.");
            c.present[cell] |= uint16_t{1} << d;
            c.weights[cell][d] = old.neighbor_weights[rank];
            if constexpr (std::is_same_v<Node, DetectorNode>) {
                c.observables[cell][d] = old.neighbor_observables[rank];
            } else {
                c.observables[cell][d] = old.neighbor_observable_indices[rank];
                c.markers[cell][d] = old.neighbor_markers[rank];
            }
        };
        // Ordinary geometry is installed before assigning virtual boundary cells.
        for (size_t id : ids) {
            for (size_t rank = 0; rank < original[id].neighbors.size(); rank++) {
                const auto* other = original[id].neighbors[rank];
                if (!other)
                    continue;
                size_t j = other - original.data();
                std::array<int, 3> delta;
                for (size_t k = 0; k < 3; k++) {
                    int64_t step = positions[j][k] - positions[id][k];
                    if (step < -1 || step > 1)
                        throw std::invalid_argument("Edge is outside the static surface stencil.");
                    delta[k] = int(step);
                }
                auto it = std::find(SURFACE_OFFSETS.begin(), SURFACE_OFFSETS.end(), delta);
                if (it == SURFACE_OFFSETS.end())
                    throw std::invalid_argument("Edge direction is outside the static surface stencil.");
                install(id, it - SURFACE_OFFSETS.begin(), rank);
            }
        }
        for (size_t id : ids) {
            if (original[id].neighbors.empty() || original[id].neighbors[0] != nullptr)
                continue;
            size_t cell = result->detector_nodes[id]->grid_cell;
            bool installed = false;
            for (size_t d : stencil.boundary_directions) {
                if (c.present[cell] & (uint16_t{1} << stencil.global_to_slot[d]))
                    continue;
                auto p = positions[id];
                for (size_t k = 0; k < 3; k++)
                    p[k] += SURFACE_OFFSETS[d][k];
                if (p[0] < 0 || p[1] < 0 || p[2] < 0 || p[0] >= int64_t(c.nx) || p[1] >= int64_t(c.ny) ||
                    p[2] >= int64_t(c.nt))
                    continue;
                size_t next = p[0] + c.nx * (p[1] + c.ny * p[2]);
                if (c.kinds[next] == GridNodeKind::detector)
                    continue;
                c.kinds[next] = GridNodeKind::boundary;
                install(id, d, 0);
                c.boundary_mask[cell] |= uint16_t{1} << stencil.global_to_slot[d];
                installed = true;
                break;
            }
            if (!installed)
                throw std::invalid_argument("No virtual boundary cell is available in the selected stencil.");
        }
        if constexpr (std::is_same_v<Node, DetectorNode>)
            initialize_predecode_grid(c);
        result->components.push_back(std::move(component));
    }
    // All source topology is construction-only. Failure above leaves it intact.
    std::vector<BuildNode>().swap(original);
    return result;
}

template std::unique_ptr<StaticGridStorage<DetectorNode, obs_int>>
build_static_grid<DetectorNode, obs_int, MatchingGraphConstructionNode>(
    std::vector<MatchingGraphConstructionNode>&, const StaticGridSpec&);
template std::unique_ptr<StaticGridStorage<SearchDetectorNode, std::vector<size_t>>>
build_static_grid<SearchDetectorNode, std::vector<size_t>, SearchDetectorNode>(
    std::vector<SearchDetectorNode>&, const StaticGridSpec&);

}  // namespace beyond_blossom::structured
