// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_PREDECODE_DIAGNOSTICS_H
#define BB_STRUCTURED_PREDECODE_DIAGNOSTICS_H

#include <cstdint>
#ifndef BB_PREDECODE_CAP_CACHE
#define BB_PREDECODE_CAP_CACHE 0
#endif
#ifndef BB_PREDECODE_ENABLED
#define BB_PREDECODE_ENABLED 1
#endif
// AE ablation builds only. Zero retains the adopted fused degree-order path.
#ifndef BB_TAILRESET_PREDECODE_MODE
#define BB_TAILRESET_PREDECODE_MODE 0
#endif
#if BB_TAILRESET_PREDECODE_MODE < 0 || BB_TAILRESET_PREDECODE_MODE > 3
#error "AE predecode mode must be 0 (degree), 1 (skip), 2 (coordinate), or 3 (simple equal-share)"
#endif
#ifndef BB_TAILRESET_SYNDROME_STATS
#define BB_TAILRESET_SYNDROME_STATS 0
#endif
#ifndef BB_PREDECODE_DIAGNOSTICS
#define BB_PREDECODE_DIAGNOSTICS 0
#endif
#ifndef BB_STRUCTURED_PROFILE_COUNTERS
#define BB_STRUCTURED_PROFILE_COUNTERS 0
#endif
#ifndef BB_STRUCTURED_PROFILE_TIMERS
#define BB_STRUCTURED_PROFILE_TIMERS 0
#endif
#ifndef BB_STRUCTURED_PROFILE_TIMELINE
#define BB_STRUCTURED_PROFILE_TIMELINE 0
#endif
#if BB_STRUCTURED_PROFILE_TIMELINE && !BB_STRUCTURED_PROFILE_TIMERS
#error "Structured timeline requires phase timers"
#endif
#if BB_STRUCTURED_PROFILE_TIMELINE
#include <array>
#include <cstddef>
#define BB_TIMELINE_ARG(interval_pointer) , interval_pointer
#else
#define BB_TIMELINE_ARG(interval_pointer)
#endif
#if BB_PREDECODE_DIAGNOSTICS || BB_STRUCTURED_PROFILE_TIMERS
#include <chrono>
#endif

namespace beyond_blossom::structured {
#if BB_STRUCTURED_PROFILE_TIMELINE
// Endpoints share steady_clock's process-wide epoch. A repeated phase is an
// envelope: its interior gaps must not be interpreted as continuous activity.
struct PhaseInterval {
    uint64_t start_ns = 0, end_ns = 0;
    bool present = false, repeated = false;
    void merge(const PhaseInterval& other) noexcept {
        if (!other.present)
            return;
        if (!present) {
            *this = other;
            return;
        }
        if (other.start_ns < start_ns)
            start_ns = other.start_ns;
        if (other.end_ns > end_ns)
            end_ns = other.end_ns;
        repeated = true;
    }
    void record(std::chrono::steady_clock::time_point begin,
                std::chrono::steady_clock::time_point end) noexcept {
        PhaseInterval next;
        next.start_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count());
        next.end_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count());
        next.present = true;
        merge(next);
    }
};
#endif
struct PredecodeTimingStats {
    uint64_t prepare_ns = 0, handoff_ns = 0, mwpm_ns = 0;
#if BB_PREDECODE_DIAGNOSTICS || BB_STRUCTURED_PROFILE_COUNTERS || BB_STRUCTURED_PROFILE_TIMERS
    uint64_t reset_ns = 0, seed_build_ns = 0, normalize_ns = 0;
    uint64_t degree_ns = 0, sort_ns = 0, grow_match_ns = 0;
    uint64_t handoff_regions_ns = 0, handoff_matches_ns = 0, handoff_schedule_ns = 0;
    uint64_t initialization_ns = 0, extract_ns = 0;
    uint64_t scratch_ns = 0, cell_index_ns = 0, complete_ns = 0;
    uint64_t import_reserve_ns = 0, import_construct_ns = 0;
    uint64_t input_events = 0, normalized_seeds = 0, residual_seeds = 0;
    uint64_t matched_pairs = 0, matched_boundaries = 0, mwpm_entered = 0;
    uint64_t cap_requests = 0, cap_executions = 0, cap_finite_executions = 0;
    uint64_t cap_repeat_finite_requests = 0, cap_radius_increases = 0, cap_unchanged = 0;
    uint64_t import_regions = 0, import_roots = 0, import_pairs = 0;
    uint64_t import_boundaries = 0, import_scheduled = 0;
#endif
#if BB_STRUCTURED_PROFILE_TIMELINE
    std::array<PhaseInterval, 19> timeline{};
#endif
};

enum class PredecodePhase {
    Prepare,
    Handoff,
    Mwpm,
    Reset,
    SeedBuild,
    Normalize,
    Degree,
    Sort,
    GrowMatch,
    HandoffRegions,
    HandoffMatches,
    HandoffSchedule,
    Initialization,
    Extract,
    Scratch,
    CellIndex,
    Complete,
    ImportReserve,
    ImportConstruct
};

// Only phase boundaries read the clock. Counter-only builds never instantiate
// a clock read. Parent Prepare/Handoff windows include their detailed children.
template <PredecodePhase Phase>
class PredecodePhaseScope {
#if BB_PREDECODE_DIAGNOSTICS || BB_STRUCTURED_PROFILE_TIMERS
    static constexpr bool enabled =
        BB_STRUCTURED_PROFILE_TIMERS || (BB_PREDECODE_DIAGNOSTICS && Phase <= PredecodePhase::Mwpm);
    uint64_t* elapsed_ns;
    std::chrono::steady_clock::time_point started;
#if BB_STRUCTURED_PROFILE_TIMELINE
    PhaseInterval* interval;
#endif
    static uint64_t& counter(PredecodeTimingStats& s) noexcept {
        if constexpr (Phase == PredecodePhase::Prepare)
            return s.prepare_ns;
        else if constexpr (Phase == PredecodePhase::Handoff)
            return s.handoff_ns;
        else if constexpr (Phase == PredecodePhase::Mwpm)
            return s.mwpm_ns;
        else if constexpr (Phase == PredecodePhase::Reset)
            return s.reset_ns;
        else if constexpr (Phase == PredecodePhase::SeedBuild)
            return s.seed_build_ns;
        else if constexpr (Phase == PredecodePhase::Normalize)
            return s.normalize_ns;
        else if constexpr (Phase == PredecodePhase::Degree)
            return s.degree_ns;
        else if constexpr (Phase == PredecodePhase::Sort)
            return s.sort_ns;
        else if constexpr (Phase == PredecodePhase::GrowMatch)
            return s.grow_match_ns;
        else if constexpr (Phase == PredecodePhase::HandoffRegions)
            return s.handoff_regions_ns;
        else if constexpr (Phase == PredecodePhase::HandoffMatches)
            return s.handoff_matches_ns;
        else if constexpr (Phase == PredecodePhase::HandoffSchedule)
            return s.handoff_schedule_ns;
        else if constexpr (Phase == PredecodePhase::Initialization)
            return s.initialization_ns;
        else if constexpr (Phase == PredecodePhase::Scratch)
            return s.scratch_ns;
        else if constexpr (Phase == PredecodePhase::CellIndex)
            return s.cell_index_ns;
        else if constexpr (Phase == PredecodePhase::Complete)
            return s.complete_ns;
        else if constexpr (Phase == PredecodePhase::ImportReserve)
            return s.import_reserve_ns;
        else if constexpr (Phase == PredecodePhase::ImportConstruct)
            return s.import_construct_ns;
        else
            return s.extract_ns;
    }

   public:
    explicit PredecodePhaseScope(PredecodeTimingStats& stats) noexcept {
        if constexpr (enabled) {
            elapsed_ns = &counter(stats);
#if BB_STRUCTURED_PROFILE_TIMELINE
            interval = &stats.timeline[static_cast<size_t>(Phase)];
#endif
            started = std::chrono::steady_clock::now();
        }
    }
    ~PredecodePhaseScope() {
        if constexpr (enabled) {
            const auto ended = std::chrono::steady_clock::now();
            const auto elapsed = ended - started;
            *elapsed_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
#if BB_STRUCTURED_PROFILE_TIMELINE
            // Prepare may contain reset(), which clears the destination stats.
            interval->record(started, ended);
#endif
        }
    }
#else
   public:
    explicit PredecodePhaseScope(PredecodeTimingStats&) noexcept {
    }
#endif
    PredecodePhaseScope(const PredecodePhaseScope&) = delete;
    PredecodePhaseScope& operator=(const PredecodePhaseScope&) = delete;
};

struct ComponentDecodeTimingStats {
    uint64_t partition_ns = 0, component_decode_ns = 0, merge_ns = 0;
    uint64_t prefix_ns = 0, seed_count_ns = 0, seed_layout_ns = 0, seed_scatter_ns = 0, seed_bind_ns = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    uint64_t decode_ns = 0;
    PhaseInterval decode_interval, partition_interval;
    PhaseInterval prefix_interval, seed_count_interval, seed_layout_interval, seed_scatter_interval, seed_bind_interval;
    std::array<PhaseInterval, 2> component_intervals{}, merge_intervals{};
    PhaseInterval* component_interval(size_t c) noexcept {
        return c < component_intervals.size() ? &component_intervals[c] : nullptr;
    }
    PhaseInterval* merge_interval(size_t c) noexcept {
        return c < merge_intervals.size() ? &merge_intervals[c] : nullptr;
    }
#endif
};
#if BB_STRUCTURED_PROFILE_TIMERS
class ComponentDecodePhaseScope {
    uint64_t& counter;
    std::chrono::steady_clock::time_point started;
#if BB_STRUCTURED_PROFILE_TIMELINE
    PhaseInterval* interval = nullptr;
#endif

   public:
    explicit ComponentDecodePhaseScope(uint64_t& counter)
        : counter(counter), started(std::chrono::steady_clock::now()) {
    }
#if BB_STRUCTURED_PROFILE_TIMELINE
    ComponentDecodePhaseScope(uint64_t& counter, PhaseInterval* interval)
        : counter(counter), started(std::chrono::steady_clock::now()), interval(interval) {
    }
#endif
    ~ComponentDecodePhaseScope() {
        const auto ended = std::chrono::steady_clock::now();
        counter += std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count();
#if BB_STRUCTURED_PROFILE_TIMELINE
        if (interval)
            interval->record(started, ended);
#endif
    }
};
#endif

// This records logical queue operations, not retired instructions or allocator
// memcpy traffic. A bucket-1 vector swap moves zero individual event records.
struct QueueProfileStats {
    uint64_t pushes = 0, pops = 0, empty_pops = 0, cleared = 0, peak_size = 0;
    uint64_t valid = 0, stale_superseded = 0, stale_cancelled = 0, deferred_requeues = 0;
    uint64_t rebucket_passes = 0, rebucket_swap_passes = 0, rebucket_source_events = 0;
    uint64_t rebucket_moves = 0, min_scan_comparisons = 0;
};
}  // namespace beyond_blossom::structured
#endif
