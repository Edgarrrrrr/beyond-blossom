// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef PYMATCHING_STRUCTURED_STATIC_GRID_KERNELS_H
#define PYMATCHING_STRUCTURED_STATIC_GRID_KERNELS_H

#include "pymatching/sparse_blossom/structured/flooder/detector_node.h"
#include "pymatching/sparse_blossom/structured/grid/circuit_stencil.h"
#include "pymatching/sparse_blossom/structured/grid/static_grid.h"
#include "pymatching/sparse_blossom/structured/search/search_detector_node.h"

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define PM_GRID_AVX512 1
#define PM_GRID_AVX512_TARGET __attribute__((target("avx512f,avx512dq,avx512vl")))
#else
#define PM_GRID_AVX512 0
#endif

namespace pm::structured {

using MatchingGridComponent = StaticGridComponent<DetectorNode, obs_int>;
using SearchGridComponent = StaticGridComponent<SearchDetectorNode, std::vector<size_t>>;

enum class GridSourceGrowth { frozen, growing };

template <GridSourceGrowth>
struct GridSourceCollision;

template <>
struct GridSourceCollision<GridSourceGrowth::growing> {
    template <typename Box, typename Stencil, size_t D>
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    static inline void consider(
        const GridCollisionState* states,
        const StructuredGridWeightRow& weights,
        uint16_t present,
        const VaryingCT* radii,
        cumulative_time_int source_intercept,
        uint32_t source_id,
        GridNextEvent& best) {
        constexpr size_t S = CircuitStorage<Stencil>::template slot<D>;
        if (!(present & (uint16_t{1} << S)))
            return;
        const auto& other = states[Box::template offset<D>()];
        if (source_id == other.top_region_id)
            return;
        const auto r2 = radii[other.top_region_id];
        if (r2.is_shrinking())
            return;
        cumulative_time_int time = cumulative_time_int(weights[S]) - source_intercept -
                                   r2.y_intercept() - other.wrapped_radius_cached;
        time >>= unsigned(r2.is_growing());
        if (time < best.second)
            best = {S, time};
    }
};

template <>
struct GridSourceCollision<GridSourceGrowth::frozen> {
    template <typename Box, typename Stencil, size_t D>
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    static inline void consider(
        const GridCollisionState* states,
        const StructuredGridWeightRow& weights,
        uint16_t present,
        const VaryingCT* radii,
        cumulative_time_int source_intercept,
        uint32_t source_id,
        GridNextEvent& best) {
        constexpr size_t S = CircuitStorage<Stencil>::template slot<D>;
        if (!(present & (uint16_t{1} << S)))
            return;
        const auto& other = states[Box::template offset<D>()];
        if (source_id == other.top_region_id)
            return;
        const auto r2 = radii[other.top_region_id];
        if (!r2.is_growing())
            return;
        const cumulative_time_int time = cumulative_time_int(weights[S]) - source_intercept -
                                         r2.y_intercept() - other.wrapped_radius_cached;
        if (time < best.second)
            best = {S, time};
    }
};

template <typename Box, typename Stencil, GridSourceGrowth Growth, size_t... D>
GridNextEvent grid_matching_scalar_impl(
    const GridCollisionState* states,
    const StructuredGridWeightRow& weights,
    uint16_t present,
    const VaryingCT* radii,
    cumulative_time_int source_intercept,
    uint32_t source_id,
    std::index_sequence<D...>) {
    GridNextEvent best{SIZE_MAX, GRID_NO_TIME};
    // Equal events are revisited at the same time; the first direction wins.
    (GridSourceCollision<Growth>::template consider<Box, Stencil, D>(
         states, weights, present, radii, source_intercept, source_id, best), ...);
    return best;
}

template <typename Box, typename Stencil>
GridNextEvent grid_matching_scalar(const MatchingGridComponent& c, const DetectorNode& source, const VaryingCT* radii) {
    const size_t cell = source.grid_cell;
    const auto* states = c.collision_states.data() + cell;
    const auto& state = *states;
    const auto radius = radii[state.top_region_id];
    const auto source_intercept = radius.y_intercept() + state.wrapped_radius_cached;
    const auto& weights = c.weights[cell];
    const uint16_t present = c.present[cell];
    // Hoist source-state tests out of all twelve candidate bodies. Shrinking
    // sources cannot collide; frozen/empty and growing scans are distinct code.
    if (radius.is_shrinking())
        return {SIZE_MAX, GRID_NO_TIME};
    if (radius.is_growing())
        return grid_matching_scalar_impl<Box, Stencil, GridSourceGrowth::growing>(
            states, weights, present, radii, source_intercept, state.top_region_id, typename Stencil::Directions{});
    return grid_matching_scalar_impl<Box, Stencil, GridSourceGrowth::frozen>(
        states, weights, present, radii, source_intercept, state.top_region_id, typename Stencil::Directions{});
}

template <typename Box, typename Stencil, size_t D>
inline void grid_consider_search(
    const SearchGridComponent& c,
    const SearchDetectorNode& source,
    cumulative_time_int now,
    bool boundary,
    GridNextEvent& best) {
    constexpr size_t S = CircuitStorage<Stencil>::template slot<D>;
    if (!(c.present[source.grid_cell] & (uint16_t{1} << S)))
        return;
    size_t cell = ptrdiff_t(source.grid_cell) + Box::template offset<D>();
    const auto& other = c.nodes[cell];
    cumulative_time_int time;
    if (c.boundary_mask[source.grid_cell] & (uint16_t{1} << S)) {
        if (!boundary)
            return;
        time = cumulative_time_int(c.weights[source.grid_cell][S]) + source.distance_from_source;
    } else if (source.reached_from_source == other.reached_from_source) {
        return;
    } else if (!other.reached_from_source) {
        time = cumulative_time_int(c.weights[source.grid_cell][S]) + source.distance_from_source;
    } else {
        time = now + (cumulative_time_int(c.weights[source.grid_cell][S]) - (now - source.distance_from_source) -
                      (now - other.distance_from_source)) /
                         2;
    }
    if (time < best.second) {
        best = {S, time};
    }
}

template <typename Box, typename Stencil, size_t... D>
GridNextEvent grid_search_impl(
    const SearchGridComponent& c,
    const SearchDetectorNode& source,
    cumulative_time_int now,
    bool boundary,
    std::index_sequence<D...>) {
    GridNextEvent best{SIZE_MAX, GRID_NO_TIME};
    (grid_consider_search<Box, Stencil, D>(c, source, now, boundary, best), ...);
    return best;
}

template <typename Box, typename Stencil>
GridNextEvent grid_search(
    const SearchGridComponent& c, const SearchDetectorNode& source, cumulative_time_int now, bool boundary) {
    return grid_search_impl<Box, Stencil>(c, source, now, boundary, typename Stencil::Directions{});
}

}  // namespace pm::structured

#include "pymatching/sparse_blossom/structured/grid/static_grid_simd_kernels.h"
#endif
