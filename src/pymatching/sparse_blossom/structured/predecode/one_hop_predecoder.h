// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_ONE_HOP_PREDECODER_H
#define PYMATCHING_STRUCTURED_ONE_HOP_PREDECODER_H

#include <array>
#include <span>

#include "pymatching/sparse_blossom/matcher/mwpm.h"
#include "pymatching/sparse_blossom/structured/flooder/graph.h"
#include "pymatching/sparse_blossom/structured/predecode/predecode_diagnostics.h"
#include "pymatching/sparse_blossom/structured/predecode/seed_view.h"

namespace pm::structured {

void initialize_predecode_grid(StaticGridComponent<DetectorNode, obs_int>& grid);

// One component view into the shot's sole seed vector. Dense cell state is owned
// by the grid; direct Mwpm callers alone use owned_seeds as their shot store.
class OneHopPredecoder {
   public:
    using Grid = StaticGridComponent<DetectorNode, obs_int>;
    // Distinct buckets update results concurrently; keep them off shared cache lines.
    struct alignas(64) PartialResult {
        pm::MatchingResult result;
        PredecodeTimingStats stats;
    };
    struct PreparedOrder {
        std::span<const uint32_t> sources;
        // Biased within the backing allocation so sources retain seed IDs.
        const uint16_t* masks_by_seed;
    };
    struct ScratchLayout {
        size_t seed_begin = 0, seed_end = 0;
        uintptr_t mask_address = 0, degree_address = 0, order_address = 0;
    };
    SeedView seed_view;
    bool complete = false;
    bool result_pending = false;
    pm::MatchingResult result;
    PredecodeTimingStats stats;

    OneHopPredecoder() = default;
    explicit OneHopPredecoder(const MatchingGraph&) {
    }
    OneHopPredecoder(const OneHopPredecoder&) = delete;
    OneHopPredecoder& operator=(const OneHopPredecoder&) = delete;
    OneHopPredecoder(OneHopPredecoder&& other) noexcept;
    OneHopPredecoder& operator=(OneHopPredecoder&& other) noexcept;

    // Input validation/negative-edge normalization precede run. ComponentDecoder
    // writes its final shared vector directly and calls bind_seeds after reorder.
    void prepare(
        const MatchingGraph& graph, const std::vector<uint64_t>& events, const std::vector<uint64_t>& negative_events);
    void bind_seeds(std::span<PredecodeSeed> prepared, bool build_index = true);
    void bind_seeds_inactive(std::span<PredecodeSeed> prepared) noexcept;
    void publish_cell_state() noexcept {
        assert(!cell_state_live);
        cell_state_live = true;
    }
    static void append_seed(std::vector<PredecodeSeed>& target, DetectorNode& node);
    static void toggle_seed(std::vector<PredecodeSeed>& target, DetectorNode& node);
    static void compact_seeds(std::vector<PredecodeSeed>& target);
    void rebuild_cell_index();
    // Caller binds seeds and allocates scratch before dispatch. Disjoint
    // ranges may publish indices using separate timing stats without touching
    // the coordinator-owned cell-state lifetime flag.
    void rebuild_cell_index_range(size_t begin, size_t end, PredecodeTimingStats& range_stats);
    size_t owned_seed_capacity() const {
        return owned_seeds.capacity();
    }

    // Serial entry has no bucket preparation. Parallel callers allocate scratch
    // once, sort each contiguous bucket, run disjoint source/write footprints,
    // then merge private results and finish on the coordinator.
    // A non-null grid is an internal construction-bound ownership guarantee
    // for all seeds in this invocation. The default preserves mixed-grid input.
    void run(Grid* known_grid = nullptr);
    void prepare_scratch();
    // Allocate once before dispatch. Every bucket begins on a separate cache
    // line in each array; the seed span and global seed indices stay compact.
    void prepare_partition_scratch(std::span<const size_t> offsets);
    void bind_fixed_scratch(std::span<const size_t> offsets);
    void bind_serial_scratch(size_t capacity);
    void activate_fixed_scratch() noexcept;
    void initialize_seed_counts() noexcept;
    PreparedOrder prepare_order(size_t begin, size_t end, PartialResult& partial);
    PreparedOrder prepare_partition_order(size_t bucket, PartialResult& partial);
    PreparedOrder prepare_partition_order(size_t bucket, size_t count, PartialResult& partial);
    void run_order(const PreparedOrder& source_order, PartialResult& partial);
    // Requires published active indices for every source/read footprint. Each
    // concurrent caller owns a disjoint bucket scratch and write footprint;
    // join all interiors before running the conflict bucket. Only the valid
    // prefix is visited when fixed capacities leave holes between buckets.
    void run_partition_fused(size_t bucket, PartialResult& partial, Grid* known_grid = nullptr);
    void run_partition_fused(size_t bucket, size_t count, PartialResult& partial, Grid* known_grid = nullptr);
    // Read addresses outside timed intervals; this never reads scratch values.
    std::span<const ScratchLayout> scratch_layout() const {
        return std::span<const ScratchLayout>(scratch_ranges).first(scratch_range_count);
    }
    void merge(const PartialResult& partial);
    void finish();
    // Clear grid cells after final extraction while retaining shot diagnostics,
    // seed identities, and extracted results until the next reset.
    void clear_cell_state() noexcept;
    bool has_live_cell_state() const noexcept {
        return cell_state_live;
    }
    // Disjoint ranges clear only required cells. Join every range before
    // finishing the clear or retiring the seed span.
    void clear_cell_state_range(size_t begin, size_t end) noexcept;
    void finish_cell_state_clear() noexcept {
        cell_state_live = false;
    }
    void reset();

   private:
    friend struct OneHopPredecoderTestAccess;
    friend struct SerialSeedStorageTestAccess;
    bool cell_state_live = false;
    std::vector<PredecodeSeed> owned_seeds;
    std::vector<uint16_t> candidate_masks;
    std::vector<uint32_t> order;
    std::vector<uint8_t> degrees;
    std::array<ScratchLayout, 9> scratch_ranges{};
    size_t scratch_range_count = 0;
    std::array<ScratchLayout, 9> fixed_scratch_ranges{};
    size_t fixed_scratch_count = 0;
    bool serial_scratch = false;
    PreparedOrder prepare_order_with_scratch(
        size_t begin, size_t end, PartialResult& partial,
        uint16_t* masks_by_seed, uint8_t* degrees_by_seed, uint32_t* order_by_seed);
    bool try_run_bound_range(Grid* known_grid, uint32_t begin, uint32_t end,
        PartialResult& partial, uint16_t* masks, uint32_t* links);
    void grow(uint32_t index, PartialResult& partial);
    template <bool Sparse>
    void grow_with_mask(uint32_t index, uint16_t candidates, PartialResult& partial);
    void run_leaf(uint32_t index, uint16_t candidates, PartialResult& partial);
    void run_one(uint32_t index, uint16_t candidates, PartialResult& partial);
    void run_fused_range(uint32_t begin, uint32_t end, PartialResult& partial,
                         uint16_t* masks_by_seed, uint32_t* links_by_seed, Grid* known_grid = nullptr);
    void pair(uint32_t source, uint32_t target, uint8_t slot, PartialResult& partial);
};

}  // namespace pm::structured
#endif
