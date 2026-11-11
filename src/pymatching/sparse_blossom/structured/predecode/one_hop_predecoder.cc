// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#include "pymatching/sparse_blossom/structured/predecode/one_hop_predecoder.h"

#include "pymatching/sparse_blossom/structured/predecode/one_hop_neighborhood_simd.h"
#include "pymatching/sparse_blossom/structured/predecode/one_hop_bound_kernel.h"
#include "pymatching/sparse_blossom/structured/predecode/one_hop_simd_kernels.h"
#include "pymatching/sparse_blossom/structured/predecode/one_hop_arm_kernels.h"

#ifndef PM_PREDECODE_SIMD
#define PM_PREDECODE_SIMD 0
#endif

#include <algorithm>
#include <cassert>
#include <utility>

namespace pm::structured {
namespace {
#if PM_TAILRESET_PREDECODE_MODE == 2
template <typename Box, typename Stencil>
uint32_t coordinate_neighborhood(const OneHopPredecoder::Grid& grid, size_t cell, uint16_t) {
    return one_hop_syndrome_mask<Box, Stencil>(grid, cell);
}
#endif
template <size_t NX, typename Stencil>
void bind_kernels(OneHopPredecoder::Grid& w) {
    // Only the row/slice strides enter these kernels. Time extent is immaterial.
    using Box = StaticGridBox<NX, NX, 1>;
#if PM_TAILRESET_PREDECODE_MODE == 2
    w.predecode.neighborhood = coordinate_neighborhood<Box, Stencil>;
#else
    w.predecode.neighborhood = one_hop_neighborhood_portable<Box, Stencil>;
#if PM_PREDECODE_POPCNT_AVAILABLE
    if (__builtin_cpu_supports("popcnt"))
        w.predecode.neighborhood = one_hop_neighborhood_popcnt<Box, Stencil>;
#endif
#if PM_PREDECODE_NEIGHBORHOOD_SIMD && PM_PREDECODE_AVX2_AVAILABLE && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
    if (grid_avx2_available()) {
        w.predecode.neighborhood = one_hop_neighborhood_avx2_portable<Box, Stencil>;
        if (__builtin_cpu_supports("popcnt"))
            w.predecode.neighborhood = one_hop_neighborhood_avx2_popcnt<Box, Stencil>;
    }
#endif
#endif
#if PM_PREDECODE_SIMD && PM_STRUCTURED_STORAGE_DIRECTIONS == 12 && \
    !PM_PREDECODE_CAP_CACHE && !PM_STRUCTURED_PROFILE_COUNTERS && !PM_STRUCTURED_PROFILE_TIMERS && \
    !PM_PREDECODE_DIAGNOSTICS && PM_TAILRESET_PREDECODE_MODE == 0
#if PM_PREDECODE_AVX2_AVAILABLE
    if (grid_avx2_available() && __builtin_cpu_supports("popcnt"))
        w.predecode.run_bound_range = &run_bound_one_hop<Box, Stencil>;
#elif defined(__APPLE__) && defined(__aarch64__)
    w.predecode.run_bound_range = &run_bound_one_hop<Box, Stencil>;
#endif
#endif
    w.predecode.radius_limit = one_hop_radius_limit<Box, Stencil>;
    w.predecode.sparse_radius_limit = one_hop_radius_limit_sparse<Stencil>;
#if PM_PREDECODE_SIMD && defined(__aarch64__) && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
    w.predecode.radius_limit = one_hop_radius_limit_arm_neon<Box, Stencil>;
#endif
#if PM_PREDECODE_SIMD && PM_PREDECODE_AVX2_AVAILABLE && PM_STRUCTURED_STORAGE_DIRECTIONS == 12
    if (grid_avx2_available()) {
        w.predecode.sparse_radius_limit = one_hop_radius_limit_sparse_avx2;
#if PM_PREDECODE_CAP_3X128
        w.predecode.radius_limit = one_hop_radius_limit_avx2_3x128<Box, Stencil>;
#else
        w.predecode.radius_limit = one_hop_radius_limit_avx2<Box, Stencil>;
#endif
    }
#endif
}

template <typename Stencil>
void bind_width(OneHopPredecoder::Grid& w) {
    switch (w.nx) {
#define PM_PREDECODE_WIDTH(N) \
    case N:                   \
        return bind_kernels<N, Stencil>(w)
        PM_PREDECODE_WIDTH(5);
        PM_PREDECODE_WIDTH(7);
        PM_PREDECODE_WIDTH(9);
        PM_PREDECODE_WIDTH(11);
        PM_PREDECODE_WIDTH(13);
        PM_PREDECODE_WIDTH(15);
        PM_PREDECODE_WIDTH(17);
        PM_PREDECODE_WIDTH(19);
        PM_PREDECODE_WIDTH(21);
        PM_PREDECODE_WIDTH(23);
        PM_PREDECODE_WIDTH(25);
        PM_PREDECODE_WIDTH(27);
        PM_PREDECODE_WIDTH(31);
        PM_PREDECODE_WIDTH(35);
#undef PM_PREDECODE_WIDTH
        default:
            throw std::invalid_argument("Unsupported predecoder grid width.");
    }
}
}  // namespace

void initialize_predecode_grid(OneHopPredecoder::Grid& grid) {
    auto& state = grid.predecode;
    state.active.resize(grid.kinds.size());
    state.radii.resize(grid.kinds.size());
    for (size_t slot = 0; slot < STRUCTURED_STORAGE_DIRECTIONS; slot++)
        state.offsets[slot] = grid.selected_offset(slot);
    switch (grid.stencil_kind) {
        case CircuitStencilKind::x:
            bind_width<CircuitStencil<CircuitStencilKind::x>>(grid);
            break;
        case CircuitStencilKind::z:
            bind_width<CircuitStencil<CircuitStencilKind::z>>(grid);
            break;
        case CircuitStencilKind::unrotated:
            bind_width<CircuitStencil<CircuitStencilKind::unrotated>>(grid);
            break;
    }
}

OneHopPredecoder::OneHopPredecoder(OneHopPredecoder&& other) noexcept : OneHopPredecoder() {
    *this = std::move(other);
}

OneHopPredecoder& OneHopPredecoder::operator=(OneHopPredecoder&& other) noexcept {
    if (this == &other)
        return *this;
    // The target may still own live cells; retire them before replacing its
    // view or destroying its owned seed allocation. std::allocator vectors
    // transfer their storage, so an owned span retains its original address.
    clear_cell_state();
    seed_view = std::exchange(other.seed_view, {});
    cell_state_live = std::exchange(other.cell_state_live, false);
    complete = std::exchange(other.complete, false);
    result_pending = std::exchange(other.result_pending, false);
    result = std::exchange(other.result, {});
    stats = std::exchange(other.stats, {});
    owned_seeds = std::move(other.owned_seeds);
    candidate_masks = std::move(other.candidate_masks);
    order = std::move(other.order);
    degrees = std::move(other.degrees);
    scratch_ranges = other.scratch_ranges;
    scratch_range_count = std::exchange(other.scratch_range_count, 0);
    fixed_scratch_ranges = other.fixed_scratch_ranges;
    fixed_scratch_count = std::exchange(other.fixed_scratch_count, 0);
    serial_scratch = std::exchange(other.serial_scratch, false);
    return *this;
}

void OneHopPredecoder::clear_cell_state() noexcept {
    if (!cell_state_live)
        return;
    for (const auto& range : seed_view.active_ranges())
        clear_cell_state_range(range.begin, range.end);
    finish_cell_state_clear();
}

void OneHopPredecoder::clear_cell_state_range(size_t begin, size_t end) noexcept {
    assert(cell_state_live);
    assert(begin <= end && end <= seed_view.storage.size());
    for (size_t k = begin; k < end; ++k) {
        const auto& seed = seed_view.storage[k];
        seed.grid->predecode.active[seed.cell] = 0;
        seed.grid->predecode.radii[seed.cell] = 0;
    }
}

void OneHopPredecoder::reset() {
#if PM_PREDECODE_DIAGNOSTICS || PM_STRUCTURED_PROFILE_COUNTERS || PM_STRUCTURED_PROFILE_TIMERS
    stats = {};
#endif
    PredecodePhaseScope<PredecodePhase::Reset> reset_phase(stats);
    clear_cell_state();
    seed_view.retire();
    owned_seeds.clear();
    scratch_range_count = 0;
    result = {};
    complete = false;
    result_pending = false;
}

void OneHopPredecoder::append_seed(std::vector<PredecodeSeed>& target, DetectorNode& node) {
    auto& active = node.grid_component->predecode.active[node.grid_cell];
    if (active)
        throw std::invalid_argument("Duplicate detection event in predecoder input.");
    PredecodeSeed seed{uint32_t(node.detector_id), uint32_t(node.grid_cell)};
    seed.grid = node.grid_component;
    target.push_back(seed);
    active = uint32_t(target.size());
}

void OneHopPredecoder::toggle_seed(std::vector<PredecodeSeed>& target, DetectorNode& node) {
    auto& active = node.grid_component->predecode.active[node.grid_cell];
    if (active) {
        target[active - 1].detector_id = UINT32_MAX;
        active = 0;
    } else {
        append_seed(target, node);
    }
}

void OneHopPredecoder::compact_seeds(std::vector<PredecodeSeed>& target) {
    std::erase_if(target, [](const auto& seed) {
        return seed.detector_id == UINT32_MAX;
    });
}

void OneHopPredecoder::bind_seeds_inactive(std::span<PredecodeSeed> prepared) noexcept {
    assert(!cell_state_live);
    seed_view.bind_dense(prepared);
#if PM_STRUCTURED_PROFILE_COUNTERS
    stats.normalized_seeds = seed_view.active_count();
    stats.residual_seeds = seed_view.active_count();
#endif
}

void OneHopPredecoder::bind_seeds(std::span<PredecodeSeed> prepared, bool build_index) {
    // Preserve the existing immediate-binding contract. Only the staged
    // owner-scatter entry requires an inactive view before binding.
    seed_view.bind_dense(prepared);
    cell_state_live = true;
    if (build_index)
        rebuild_cell_index();
#if PM_STRUCTURED_PROFILE_COUNTERS
    stats.normalized_seeds = seed_view.active_count();
    stats.residual_seeds = seed_view.active_count();
#endif
}

void OneHopPredecoder::rebuild_cell_index() {
    cell_state_live = true;
    for (const auto& range : seed_view.active_ranges())
        rebuild_cell_index_range(range.begin, range.end, stats);
}

void OneHopPredecoder::rebuild_cell_index_range(
    size_t begin, size_t end, PredecodeTimingStats& range_stats) {
    assert(cell_state_live);
    assert(begin <= end && end <= seed_view.storage.size());
    PredecodePhaseScope<PredecodePhase::CellIndex> phase(range_stats);
    for (uint32_t k = begin; k < end; k++)
        seed_view.storage[k].grid->predecode.active[seed_view.storage[k].cell] = k + 1;
}

void OneHopPredecoder::prepare(
    const MatchingGraph& graph, const std::vector<uint64_t>& events, const std::vector<uint64_t>& negative_events) {
#if PM_PREDECODE_ENABLED
    PredecodePhaseScope<PredecodePhase::Prepare> prepare_phase(stats);
#endif
    reset();
#if PM_STRUCTURED_PROFILE_COUNTERS
    stats.input_events = events.size();
#endif
    try {
        {
            PredecodePhaseScope<PredecodePhase::SeedBuild> phase(stats);
            for (auto detector : events) {
                if (detector >= graph.num_nodes)
                    throw std::invalid_argument("Detection event index is outside the structured graph.");
                if (detector < graph.is_user_graph_boundary_node.size() && graph.is_user_graph_boundary_node[detector])
                    continue;
                append_seed(owned_seeds, const_cast<DetectorNode&>(graph.node(detector)));
            }
        }
        {
            PredecodePhaseScope<PredecodePhase::Normalize> phase(stats);
            for (auto detector : negative_events)
                toggle_seed(owned_seeds, const_cast<DetectorNode&>(graph.node(detector)));
            if (!negative_events.empty())
                compact_seeds(owned_seeds);
        }
        bind_seeds(owned_seeds);
    } catch (...) {
        // Partial inputs own touched cells before a view has been published.
        for (const auto& seed : owned_seeds) {
            seed.grid->predecode.active[seed.cell] = 0;
            seed.grid->predecode.radii[seed.cell] = 0;
        }
        reset();
        throw;
    }
}

void OneHopPredecoder::grow(uint32_t index, PartialResult& partial) {
    grow_with_mask<false>(index, 0, partial);
}

template <bool Sparse>
inline void OneHopPredecoder::grow_with_mask(uint32_t index, uint16_t candidates, PartialResult& partial) {
    auto& seed = seed_view.storage[index];
#if PM_STRUCTURED_PROFILE_COUNTERS
    ++partial.stats.cap_requests;
    partial.stats.cap_repeat_finite_requests += seed.profile_finite_grow;
#endif
#if PM_PREDECODE_CAP_CACHE
    // Neighbor radii only increase. After a finite cap is rounded down to the
    // even lattice, dual feasibility prevents any later increase of this seed.
    if (seed.cap_saturated) {
#if PM_STRUCTURED_PROFILE_COUNTERS
        ++partial.stats.cap_unchanged;
#endif
        return;
    }
#endif
    auto& w = *seed.grid;
#if PM_STRUCTURED_PROFILE_COUNTERS
    ++partial.stats.cap_executions;
    const auto prior_radius = seed.radius;
#endif
    const weight_int limit = [&] {
        if constexpr (Sparse)
            return w.predecode.sparse_radius_limit(w, seed.cell, candidates);
        else
            return w.predecode.radius_limit(w, seed.cell);
    }();
    // Isolated vertices have no finite bound and remain for the full solver.
    if (limit != UINT32_MAX) {
#if PM_PREDECODE_CAP_CACHE
        // UINT32_MAX is also the no-edge sentinel; leave it uncached so that a
        // real maximum-weight edge can become finite as its neighbor grows.
        seed.cap_saturated = true;
#endif
#if PM_STRUCTURED_PROFILE_COUNTERS
        ++partial.stats.cap_finite_executions;
        seed.profile_finite_grow = true;
#endif
        // Round down to the common even time lattice required by two growing
        // fronts. This is exact for the even weights produced by UserGraph;
        // direct odd-weight low-level graphs retain dual feasibility as well.
        seed.radius = limit & ~weight_int{1};
        w.predecode.radii[seed.cell] = seed.radius;
    }
#if PM_STRUCTURED_PROFILE_COUNTERS
    partial.stats.cap_radius_increases += seed.radius > prior_radius;
    partial.stats.cap_unchanged += seed.radius == prior_radius;
#endif
}

void OneHopPredecoder::pair(uint32_t source, uint32_t target, uint8_t slot, PartialResult& partial) {
    auto& seed = seed_view.storage[source];
#if PM_STRUCTURED_PROFILE_COUNTERS
    if (target == PredecodeSeed::BOUNDARY) {
        ++partial.stats.matched_boundaries;
    } else {
        ++partial.stats.matched_pairs;
    }
#endif
    seed.mate = target;
    seed.match_slot = slot;
    if (target != UINT32_MAX - 1) {
        seed_view.storage[target].mate = source;
        seed_view.storage[target].match_slot = slot ^ 1;
    }
    const auto& grid = *seed.grid;
    partial.result.weight += grid.weights[seed.cell][slot];
    partial.result.obs_mask ^= grid.observables[seed.cell][slot];
}

void OneHopPredecoder::prepare_scratch() {
    PredecodePhaseScope<PredecodePhase::Scratch> phase(stats);
    if (serial_scratch) {
        assert(seed_view.storage.size() <= fixed_scratch_ranges[0].seed_end);
        scratch_ranges[0] = fixed_scratch_ranges[0];
        scratch_ranges[0].seed_end = seed_view.storage.size();
        scratch_range_count = 1;
        return;
    }
    if (!fixed_scratch_count) {
        candidate_masks.resize(seed_view.storage.size());
        degrees.resize(seed_view.storage.size());
        order.resize(seed_view.storage.size());
    }
    scratch_ranges[0] = {
        0, seed_view.storage.size(), reinterpret_cast<uintptr_t>(candidate_masks.data()),
        reinterpret_cast<uintptr_t>(degrees.data()), reinterpret_cast<uintptr_t>(order.data())};
    scratch_range_count = 1;
}

void OneHopPredecoder::prepare_partition_scratch(std::span<const size_t> offsets) {
    PredecodePhaseScope<PredecodePhase::Scratch> phase(stats);
    assert(offsets.size() >= 2 && offsets.size() <= scratch_ranges.size() + 1);
    assert(offsets.front() == 0 && offsets.back() == seed_view.storage.size());
    assert(!serial_scratch || offsets.back() <= fixed_scratch_ranges[0].seed_end);
    const size_t buckets = offsets.size() - 1;
    constexpr size_t line = 64;
    if (!fixed_scratch_count) {
        candidate_masks.resize(seed_view.storage.size() + buckets * line / sizeof(uint16_t));
        degrees.resize(seed_view.storage.size() + buckets * line / sizeof(uint8_t));
        order.resize(seed_view.storage.size() + buckets * line / sizeof(uint32_t));
    }
    const auto aligned_address = [](const auto& storage, size_t begin, size_t bucket) {
        const auto address = reinterpret_cast<uintptr_t>(storage.data() + begin) + bucket * line;
        return (address + line - 1) & ~uintptr_t(line - 1);
    };
    for (size_t b = 0; b < buckets; ++b) {
        assert(offsets[b] <= offsets[b + 1]);
        scratch_ranges[b] = {
            offsets[b], offsets[b + 1], aligned_address(candidate_masks, offsets[b], b),
            aligned_address(degrees, offsets[b], b), aligned_address(order, offsets[b], b)};
    }
    scratch_range_count = buckets;
}

void OneHopPredecoder::bind_fixed_scratch(std::span<const size_t> offsets) {
    assert(!fixed_scratch_count && !cell_state_live);
    // Constructor-only allocation. Dense fallback never ends these objects.
    const size_t capacity = offsets.back();
    constexpr size_t line = 64;
    const size_t buckets = offsets.size() - 1;
    if (capacity > SIZE_MAX - buckets * line ||
        capacity + buckets * line / sizeof(uint16_t) > candidate_masks.max_size() ||
        capacity + buckets * line / sizeof(uint8_t) > degrees.max_size() ||
        capacity + buckets * line / sizeof(uint32_t) > order.max_size())
        throw std::length_error("Fixed predecode scratch exceeds its allocation range.");
    candidate_masks.resize(capacity + buckets * line / sizeof(uint16_t));
    degrees.resize(capacity + buckets * line / sizeof(uint8_t));
    order.resize(capacity + buckets * line / sizeof(uint32_t));
    for (size_t b = 0; b < buckets; ++b) {
        const auto align = [b](const auto& storage, size_t begin) {
            const auto address = reinterpret_cast<uintptr_t>(storage.data() + begin) + b * line;
            return (address + line - 1) & ~uintptr_t(line - 1);
        };
        fixed_scratch_ranges[b] = {offsets[b], offsets[b + 1], align(candidate_masks, offsets[b]),
            align(degrees, offsets[b]), align(order, offsets[b])};
    }
    fixed_scratch_count = buckets;
}

void OneHopPredecoder::activate_fixed_scratch() noexcept {
    assert(fixed_scratch_count);
    scratch_ranges = fixed_scratch_ranges;
    scratch_range_count = fixed_scratch_count;
}

void OneHopPredecoder::bind_serial_scratch(size_t capacity) {
    if (serial_scratch) {
        assert(fixed_scratch_ranges[0].seed_end == capacity);
        return;
    }
    assert(!fixed_scratch_count && !cell_state_live);
    constexpr size_t line = 64;
    // The same predecoder can later consume public P2/P4/P8 scratch layouts.
    // Reserve their maximum alignment padding now, without per-shot growth.
    const size_t padding = scratch_ranges.size() * line;
    if (capacity > SIZE_MAX - padding || capacity + padding / sizeof(uint16_t) > candidate_masks.max_size() ||
        capacity + padding / sizeof(uint8_t) > degrees.max_size() ||
        capacity + padding / sizeof(uint32_t) > order.max_size())
        throw std::length_error("Serial predecode scratch exceeds its allocation range.");
    // Publish only after every allocation succeeds. The padding retains all
    // capacity elements after rounding each array's base to a cache line.
    std::vector<uint16_t> masks(capacity + padding / sizeof(uint16_t));
    std::vector<uint8_t> new_degrees(capacity + padding / sizeof(uint8_t));
    std::vector<uint32_t> new_order(capacity + padding / sizeof(uint32_t));
    const auto aligned = [](const auto& storage) {
        return (reinterpret_cast<uintptr_t>(storage.data()) + line - 1) & ~uintptr_t(line - 1);
    };
    const ScratchLayout layout{0, capacity, aligned(masks), aligned(new_degrees), aligned(new_order)};
    candidate_masks = std::move(masks);
    degrees = std::move(new_degrees);
    order = std::move(new_order);
    scratch_ranges[0] = fixed_scratch_ranges[0] = layout;
    scratch_range_count = fixed_scratch_count = 1;
    serial_scratch = true;
}

void OneHopPredecoder::initialize_seed_counts() noexcept {
#if PM_STRUCTURED_PROFILE_COUNTERS
    stats.input_events = stats.normalized_seeds = stats.residual_seeds = seed_view.active_count();
#endif
}

OneHopPredecoder::PreparedOrder OneHopPredecoder::prepare_order(
    size_t begin, size_t end, PartialResult& partial) {
    if (serial_scratch) {
        const auto& layout = fixed_scratch_ranges[0];
        assert(end <= layout.seed_end);
        return prepare_order_with_scratch(begin, end, partial,
            reinterpret_cast<uint16_t*>(layout.mask_address),
            reinterpret_cast<uint8_t*>(layout.degree_address), reinterpret_cast<uint32_t*>(layout.order_address));
    }
    return prepare_order_with_scratch(begin, end, partial, candidate_masks.data(), degrees.data(), order.data());
}

OneHopPredecoder::PreparedOrder OneHopPredecoder::prepare_partition_order(size_t bucket, PartialResult& partial) {
    assert(bucket < scratch_range_count);
    const auto& range = scratch_ranges[bucket];
    return prepare_partition_order(bucket, range.seed_end - range.seed_begin, partial);
}

OneHopPredecoder::PreparedOrder OneHopPredecoder::prepare_partition_order(
    size_t bucket, size_t count, PartialResult& partial) {
    assert(bucket < scratch_range_count);
    const auto& range = scratch_ranges[bucket];
    assert(count <= range.seed_end - range.seed_begin);
    // Padding puts every address at or beyond its original seed offset. Each
    // biased pointer therefore remains inside its allocated array.
    const auto biased_pointer = [&](auto& storage, uintptr_t address) {
        const size_t offset =
            (address - reinterpret_cast<uintptr_t>(storage.data())) / sizeof(storage.front());
        assert(offset >= range.seed_begin);
        return storage.data() + (offset - range.seed_begin);
    };
    return prepare_order_with_scratch(
        range.seed_begin, range.seed_begin + count, partial,
        biased_pointer(candidate_masks, range.mask_address),
        biased_pointer(degrees, range.degree_address), biased_pointer(order, range.order_address));
}

OneHopPredecoder::PreparedOrder OneHopPredecoder::prepare_order_with_scratch(
    size_t begin, size_t end, PartialResult& partial,
    uint16_t* masks_by_seed, uint8_t* degrees_by_seed, uint32_t* order_by_seed) {
    assert(begin <= end && end <= seed_view.storage.size());
#if PM_TAILRESET_PREDECODE_MODE == 1
    return {{}, masks_by_seed};
#elif PM_TAILRESET_PREDECODE_MODE == 2
    if (begin == end) return {{}, masks_by_seed};
    for (uint32_t k = begin; k < end; ++k) {
        const auto& seed = seed_view.storage[k];
        const auto& grid = *seed.grid;
        masks_by_seed[k] = uint16_t(grid.predecode.neighborhood(grid, seed.cell, grid.boundary_mask[seed.cell]));
        order_by_seed[k] = k;
    }
    // Sort source indices only; the shared seed backing retains its identity.
    // Linear cells encode (t, gy, gx), including rotated-grid embedding.
    const auto less = [&](uint32_t a, uint32_t b) {
        const auto& lhs = seed_view.storage[a];
        const auto& rhs = seed_view.storage[b];
        if (lhs.grid != rhs.grid && lhs.grid->group != rhs.grid->group)
            return lhs.grid->group < rhs.grid->group;
        return lhs.cell < rhs.cell;
    };
    auto* first = order_by_seed + begin;
    auto* last = order_by_seed + end;
    if (!std::is_sorted(first, last, less)) std::sort(first, last, less);
    return {std::span<const uint32_t>(first, end - begin), masks_by_seed};
#else
    std::array<size_t, CIRCUIT_SCAN_DIRECTIONS + 1> counts{};
    {
        PredecodePhaseScope<PredecodePhase::Degree> degree_phase(partial.stats);
        for (size_t k = begin; k < end; k++) {
            const auto& w = *seed_view.storage[k].grid;
            const auto neighborhood = w.predecode.neighborhood(w, seed_view.storage[k].cell, w.boundary_mask[seed_view.storage[k].cell]);
            const auto degree = neighborhood >> 16;
            masks_by_seed[k] = uint16_t(neighborhood);
            degrees_by_seed[k] = uint8_t(degree);
            counts[degree]++;
        }
    }
    {
        PredecodePhaseScope<PredecodePhase::Sort> sort_phase(partial.stats);
        size_t position = begin;
        for (auto& count : counts) {
            const auto size = count;
            count = position;
            position += size;
        }
        // Stable counting sort: bounded degree, no comparisons or per-shot malloc.
        for (uint32_t k = begin; k < end; k++)
            order_by_seed[counts[degrees_by_seed[k]]++] = k;
    }
    const auto* first = begin ? order_by_seed + begin : order_by_seed;
    return {std::span<const uint32_t>(first, end - begin), masks_by_seed};
#endif
}

inline void OneHopPredecoder::run_leaf(uint32_t index, uint16_t candidates, PartialResult& partial) {
    auto& seed = seed_view.storage[index];
    auto& grid = *seed.grid;
    grow_with_mask<true>(index, candidates, partial);
    if (candidates) {
        // Degree includes boundary slots, so a syndrome leaf has exactly one
        // candidate and no boundary alternative to scan after this attempt.
        const uint8_t slot = std::countr_zero(candidates);
        const auto other_cell = ptrdiff_t(seed.cell) + grid.predecode.offsets[slot];
        const auto other = grid.predecode.active[other_cell] - 1;
        if (seed_view.storage[other].mate != PredecodeSeed::UNMATCHED)
            return;
        if (seed.radius + seed_view.storage[other].radius < grid.weights[seed.cell][slot])
            grow(other, partial);
        if (seed.radius + seed_view.storage[other].radius == grid.weights[seed.cell][slot])
            pair(index, other, slot, partial);
    } else if (const auto boundaries = grid.boundary_mask[seed.cell]) {
        const uint8_t slot = std::countr_zero(boundaries);
        if (seed.radius == grid.weights[seed.cell][slot])
            pair(index, PredecodeSeed::BOUNDARY, slot, partial);
    }
}

void OneHopPredecoder::run_one(uint32_t index, uint16_t candidates, PartialResult& partial) {
    auto& seed = seed_view.storage[index];
    auto& w = *seed.grid;
    grow_with_mask<true>(index, candidates, partial);
    while (candidates) {
        const uint8_t slot = std::countr_zero(candidates);
        candidates &= candidates - 1;
        const auto other_cell = ptrdiff_t(seed.cell) + w.predecode.offsets[slot];
        const auto other = w.predecode.active[other_cell] - 1;
        if (seed_view.storage[other].mate != UINT32_MAX)
            continue;
        if (seed.radius + seed_view.storage[other].radius < w.weights[seed.cell][slot])
            grow(other, partial);
        if (seed.radius + seed_view.storage[other].radius == w.weights[seed.cell][slot]) {
            pair(index, other, slot, partial);
            break;
        }
    }
    if (seed.mate == UINT32_MAX) {
        auto boundaries = w.boundary_mask[seed.cell];
        while (boundaries) {
            const uint8_t slot = std::countr_zero(boundaries);
            boundaries &= boundaries - 1;
            if (seed.radius == w.weights[seed.cell][slot]) {
                pair(index, UINT32_MAX - 1, slot, partial);
                break;
            }
        }
    }
}

void OneHopPredecoder::run_order(const PreparedOrder& source_order, PartialResult& partial) {
    PredecodePhaseScope<PredecodePhase::GrowMatch> grow_phase(partial.stats);
    for (uint32_t index : source_order.sources) {
        if (seed_view.storage[index].mate == PredecodeSeed::UNMATCHED)
            run_one(index, source_order.masks_by_seed[index], partial);
    }
}

void OneHopPredecoder::merge(const PartialResult& partial) {
    result += partial.result;
#if PM_PREDECODE_DIAGNOSTICS || PM_STRUCTURED_PROFILE_COUNTERS || PM_STRUCTURED_PROFILE_TIMERS
    stats.degree_ns += partial.stats.degree_ns;
    stats.sort_ns += partial.stats.sort_ns;
    stats.grow_match_ns += partial.stats.grow_match_ns;
    stats.cell_index_ns += partial.stats.cell_index_ns;
#endif
#if PM_STRUCTURED_PROFILE_TIMELINE
    for (const auto phase : {
             PredecodePhase::CellIndex, PredecodePhase::Degree, PredecodePhase::Sort, PredecodePhase::GrowMatch}) {
        const auto index = static_cast<size_t>(phase);
        stats.timeline[index].merge(partial.stats.timeline[index]);
    }
#endif
#if PM_STRUCTURED_PROFILE_COUNTERS
    stats.matched_pairs += partial.stats.matched_pairs;
    stats.matched_boundaries += partial.stats.matched_boundaries;
    stats.residual_seeds -= 2 * partial.stats.matched_pairs + partial.stats.matched_boundaries;
    stats.cap_requests += partial.stats.cap_requests;
    stats.cap_executions += partial.stats.cap_executions;
    stats.cap_finite_executions += partial.stats.cap_finite_executions;
    stats.cap_repeat_finite_requests += partial.stats.cap_repeat_finite_requests;
    stats.cap_radius_increases += partial.stats.cap_radius_increases;
    stats.cap_unchanged += partial.stats.cap_unchanged;
#endif
}

void OneHopPredecoder::finish() {
    PredecodePhaseScope<PredecodePhase::Complete> phase(stats);
    complete = true;
    for (const auto& range : seed_view.active_ranges()) {
        const auto seeds = seed_view.storage.subspan(range.begin, range.count());
        if (!std::all_of(seeds.begin(), seeds.end(), [](const auto& seed) {
                return seed.mate != PredecodeSeed::UNMATCHED;
            })) {
            complete = false;
            break;
        }
    }
    result_pending = complete;
}

void OneHopPredecoder::run_partition_fused(size_t bucket, PartialResult& partial, Grid* known_grid) {
    assert(bucket < scratch_range_count);
    const auto& range = scratch_ranges[bucket];
    run_partition_fused(bucket, range.seed_end - range.seed_begin, partial, known_grid);
}

void OneHopPredecoder::run_partition_fused(size_t bucket, size_t count, PartialResult& partial, Grid* known_grid) {
    assert(bucket < scratch_range_count);
    const auto& range = scratch_ranges[bucket];
    assert(count <= range.seed_end - range.seed_begin);
    assert(range.seed_begin + count <= seed_view.storage.size());
    assert(range.seed_begin + count <= UINT32_MAX);
    // Use the same in-allocation bias as the explicit sorted API. This maps
    // global physical seed slots to this bucket's private aligned scratch.
    const auto biased_pointer = [&](auto& storage, uintptr_t address) {
        const size_t offset = (address - reinterpret_cast<uintptr_t>(storage.data())) / sizeof(storage.front());
        assert(offset >= range.seed_begin);
        return storage.data() + (offset - range.seed_begin);
    };
    run_fused_range(uint32_t(range.seed_begin), uint32_t(range.seed_begin + count), partial,
        biased_pointer(candidate_masks, range.mask_address), biased_pointer(order, range.order_address), known_grid);
}

void OneHopPredecoder::run_fused_range(uint32_t begin, uint32_t end, PartialResult& partial,
                                      uint16_t* masks, uint32_t* links, Grid* known_grid) {
#if PM_TAILRESET_PREDECODE_MODE == 1
    return;
#elif PM_TAILRESET_PREDECODE_MODE == 2
    const auto ordered = prepare_order_with_scratch(begin, end, partial, masks, nullptr, links);
    run_order(ordered, partial);
#else
    if (try_run_bound_range(known_grid, begin, end, partial, masks, links))
        return;
    std::array<uint32_t, CIRCUIT_SCAN_DIRECTIONS + 1> heads, tails;
    heads.fill(end);
    for (uint32_t index = begin; index < end; ++index) {
        const auto& seed = seed_view.storage[index];
        if (seed.mate != PredecodeSeed::UNMATCHED)
            continue;
        uint32_t neighborhood;
        {
            PredecodePhaseScope<PredecodePhase::Degree> phase(partial.stats);
            const auto& grid = *seed.grid;
            neighborhood = grid.predecode.neighborhood(grid, seed.cell, grid.boundary_mask[seed.cell]);
        }
        const auto degree = neighborhood >> 16;
        if (degree <= 1) {
            // Degree-zero seeds have no syndrome or boundary neighbors, so
            // their growth commutes with every other seed. Degree-one seeds
            // retain the original stable order and may retire a later source
            // before its neighborhood is loaded.
            PredecodePhaseScope<PredecodePhase::GrowMatch> phase(partial.stats);
            run_leaf(index, uint16_t(neighborhood), partial);
        } else {
            // Defer higher degrees in stable per-degree chains. Each deferred
            // source writes one link; leaves need no mask/order scratch.
            PredecodePhaseScope<PredecodePhase::Sort> phase(partial.stats);
            masks[index] = uint16_t(neighborhood);
            if (heads[degree] == end)
                heads[degree] = index;
            else
                links[tails[degree]] = index;
            tails[degree] = index;
        }
    }
    {
        PredecodePhaseScope<PredecodePhase::GrowMatch> phase(partial.stats);
        for (size_t degree = 2; degree < heads.size(); ++degree) {
            if (heads[degree] == end)
                continue;
            links[tails[degree]] = end;
            for (uint32_t index = heads[degree]; index != end; index = links[index]) {
                if (seed_view.storage[index].mate == PredecodeSeed::UNMATCHED)
                    run_one(index, masks[index], partial);
            }
        }
    }
#endif
}

bool OneHopPredecoder::try_run_bound_range(Grid* known_grid, uint32_t begin, uint32_t end,
                                          PartialResult& partial, uint16_t* masks, uint32_t* links) {
    if (!known_grid || !known_grid->predecode.run_bound_range)
        return false;
    OneHopBoundRange range{*known_grid, seed_view.storage.data(), begin, end, masks, links, partial.result};
    known_grid->predecode.run_bound_range(range);
    return true;
}

void OneHopPredecoder::run(Grid* known_grid) {
#if PM_TAILRESET_PREDECODE_MODE == 1
    // Normalized seeds remain unmatched with zero radii for normal import/MWPM.
    // Fixed serial scratch still publishes this shot's active length, including
    // a first empty input, instead of retaining constructor or prior-shot size.
    if (serial_scratch) prepare_scratch();
    complete = seed_view.empty();
    result_pending = complete;
    return;
#endif
    if (seed_view.empty()) {
        // A first empty component must publish the active length, not the
        // constructor capacity retained by its fixed scratch allocation.
        if (serial_scratch) prepare_scratch();
        complete = true;
        result_pending = true;
        return;
    }
    assert(seed_view.active_ranges().size() == 1 && seed_view.active_count() == seed_view.storage.size());
    prepare_scratch();
    PartialResult partial;
    const auto& layout = scratch_ranges[0];
    assert(seed_view.storage.size() <= UINT32_MAX);
    run_fused_range(0, uint32_t(seed_view.storage.size()), partial,
        reinterpret_cast<uint16_t*>(layout.mask_address), reinterpret_cast<uint32_t*>(layout.order_address), known_grid);
    merge(partial);
    finish();
}

}  // namespace pm::structured
