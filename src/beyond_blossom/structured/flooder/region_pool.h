// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_REGION_POOL_H
#define BB_STRUCTURED_REGION_POOL_H
#include <cassert>
#include <memory>
#include <vector>

#include "beyond_blossom/structured/flooder/region_id.h"
#include "beyond_blossom/structured/flooder/region_radius_store.h"
#include "beyond_blossom/structured/predecode/seed_view.h"
namespace beyond_blossom::structured {
struct GraphFillRegion;
class DetectorNode;
struct Match;
// An allocator-owned array of stable object addresses. Untouched matched seeds
// have a compact representation until a caller requests their full object.
// Materialized objects never move, and index zero is never constructed.
class RegionPool {
    friend struct RegionPoolTestAccess;
    enum SlotState : uint8_t {
        free_raw = 0, reserved_raw = 1, live = 2, free_neutral = 3, reserved_neutral = 4,
        live_seed_match = 5, free_seed_match = 6, reserved_seed_match = 7,
        lazy_raw = 8, lazy_neutral = 9, lazy_seed_match = 10, free_lazy_raw = 11
    };
    static bool is_lazy_state(uint8_t state) noexcept {
        return state >= lazy_raw && state <= lazy_seed_match;
    }
    static bool has_object(uint8_t state) noexcept {
        return state == live || state == live_seed_match || state == free_neutral ||
               state == reserved_neutral || state == free_seed_match || state == reserved_seed_match ||
               state == lazy_neutral || state == lazy_seed_match;
    }
    std::allocator<GraphFillRegion> allocator_;
    GraphFillRegion* regions_ = nullptr;
    size_t capacity_;
    RegionId next_ = 1;
    // next_ may shrink between shots; object lifetimes above it still exist.
    RegionId lifetime_end_ = 1;
    RegionId neutral_reset_end_ = 0;
    bool contiguous_shot_ = false;
    std::vector<RegionId> available_;
    // Object lifetime and event liveness are separate. Not vector<bool>: distinct
    // reserved slots can be constructed concurrently without shared-word writes.
    // Resolving a compact seed is logically const: the mathematical region
    // remains unchanged, while its physical representation becomes an object.
    mutable std::vector<uint8_t> states_;
    std::vector<RegionGeneration> generations_;
    RegionId reserve_one();
    GraphFillRegion& materialize_seed(RegionId id) const noexcept;

   public:
    // Compact extraction data stays separate from the larger region objects.
    // A live_seed_match is a frozen singleton that has NEVER changed since
    // import. Invalidation is monotonic until the next lifetime/reservation.
    struct SeedExtraction {
        obs_int obs_mask;
        weight_int radius;
        RegionId mate;
    };
   private:
    static_assert(sizeof(SeedExtraction) == 16);
    mutable std::vector<SeedExtraction> seed_extraction_;
    // Direct imports initially write two detector IDs. Materialization replaces
    // them with cached pointers in the same storage. The mate detector ID is
    // local to this descriptor, so its slot need not have been imported yet.
    union SeedEndpoints {
        struct Pointers { DetectorNode* node; DetectorNode* mate; } pointers;
        struct Detectors { uint32_t node; uint32_t mate; } detectors;
        SeedEndpoints() noexcept : pointers{nullptr, nullptr} {}
    };
    static_assert(sizeof(SeedEndpoints::Detectors) == 8);
    static_assert(sizeof(SeedEndpoints) == 2 * sizeof(DetectorNode*));
    mutable std::vector<SeedEndpoints> seed_endpoints_;
    DetectorNode* compact_node_base_ = nullptr;
    size_t compact_node_count_ = 0;
    static constexpr uint8_t node_deferred = 1;
    static constexpr uint8_t compact_endpoints = 2;
    static constexpr uint8_t borrowed_endpoints = 4;
    // The internal packed lifecycle keeps the backing immutable through final
    // extraction. Copy ranges by value: moving Mwpm must not invalidate them.
    SeedView borrowed_seeds_;
    mutable std::vector<RegionId> changed_initial_seeds_;
    DetectorNode* seed_mate(RegionId id) const noexcept;
    // An untouched imported singleton may also leave its DetectorNode clean.
    // Preserve this fact after retirement so deferred cleanup can skip it.
    mutable std::vector<uint8_t> seed_node_flags_;
   public:
    bool is_unchanged_seed(RegionId id) const noexcept {
        return id && id < next_ && (states_[id] == live_seed_match || is_lazy_state(states_[id]));
    }
    bool is_lazy_seed(RegionId id) const noexcept {
        return id && id < next_ && is_lazy_state(states_[id]);
    }
    bool is_seed_node_deferred(RegionId id) const noexcept {
        assert(id && id <= capacity_);
        return (seed_node_flags_[id] & node_deferred) != 0;
    }
    const SeedExtraction& seed_extraction(RegionId id) const noexcept;
    bool has_borrowed_seeds() const noexcept { return !borrowed_seeds_.empty(); }
    const SeedView& borrowed_seeds() const noexcept { return borrowed_seeds_; }
    const PredecodeSeed& borrowed_seed(RegionId id) const noexcept {
        assert(id && id <= borrowed_seeds_.active_count());
        return borrowed_seeds_.storage[borrowed_seeds_.slot_of(id - 1)];
    }
    std::span<const RegionId> changed_initial_seeds() const noexcept { return changed_initial_seeds_; }
    void borrow_seeds(const SeedView& seeds);
    void record_initial_root(RegionId id) noexcept {
        assert(has_borrowed_seeds() && id && id <= borrowed_seeds_.active_count());
        assert(changed_initial_seeds_.size() < changed_initial_seeds_.capacity());
        changed_initial_seeds_.push_back(id);
    }
    void publish_reserved_borrowed_seed(RegionId id, VaryingCT radius) noexcept {
        assert(has_borrowed_seeds() && id && id <= borrowed_seeds_.active_count());
        assert(states_[id] == reserved_raw || states_[id] == reserved_neutral || states_[id] == reserved_seed_match);
        assert(!radius.is_growing() && !radius.is_shrinking());
        seed_node_flags_[id] = node_deferred | borrowed_endpoints;
        radii.data()[id] = radius;
        states_[id] = states_[id] == reserved_neutral ? lazy_neutral :
                      states_[id] == reserved_seed_match ? lazy_seed_match : lazy_raw;
    }
    // Called only after all affected regions have been extracted. Retire an
    // untouched compact singleton and discard its borrowed endpoint identity,
    // retaining node_deferred for the later double-buffer cleanup.
    bool finish_borrowed_seed(RegionId id) noexcept {
        const bool untouched = is_lazy_seed(id);
        if (untouched) release_unchanged_seed(id);
        seed_node_flags_[id] &= ~borrowed_endpoints;
        return untouched;
    }
    void finish_borrowed_seeds() noexcept {
        assert(empty());
        borrowed_seeds_.retire();
        changed_initial_seeds_.clear();
    }
    // Extraction can ask for the mate after its slot has been retired.
    DetectorNode* seed_node(RegionId id) const noexcept;
    void invalidate_seed_match(RegionId id) noexcept {
        if (is_lazy_state(states_[id])) materialize_seed(id);
        if (states_[id] == live_seed_match) states_[id] = live;
    }
    // Queue consumption has ended. Preserve the physical representation without
    // touching the region: it can be raw, neutral, or a known safe singleton.
    void release_unchanged_seed(RegionId id) noexcept {
        assert(is_unchanged_seed(id));
        const auto state = states_[id];
        states_[id] = state == lazy_raw ? free_lazy_raw :
                      state == lazy_neutral ? free_neutral : free_seed_match;
        radii.data()[id] = VaryingCT{0};
        available_.push_back(id);
    }
    bool is_cached_seed_slot(RegionId id) const noexcept {
        return states_[id] == free_seed_match;
    }
    bool is_deferred_seed_slot(RegionId id) const noexcept {
        return states_[id] == free_lazy_raw;
    }
    RegionRadiusStore radii;
    explicit RegionPool(size_t capacity);
    ~RegionPool();
    RegionPool(const RegionPool&) = delete;
    RegionPool& operator=(const RegionPool&) = delete;
    GraphFillRegion& operator[](RegionId id);
    const GraphFillRegion& operator[](RegionId id) const;
    GraphFillRegion* get(RegionId id);
    const GraphFillRegion* get(RegionId id) const;
    GraphFillRegion* allocate();
    // Serial reservation precedes parallel construction. Each reserved id has
    // one writer; all workers join before publication, release, or another reserve.
    std::vector<RegionId> reserve_ids(size_t count);
    // At an idle shot boundary, reserve IDs [1, count] without an ID list.
    // Throws before mutation unless every previous reservation is released.
    RegionId reserve_contiguous_ids(size_t count);
    GraphFillRegion& construct_reserved(RegionId id) noexcept;
    // Import already knows the final radius, match, and singleton shell.
    // Definition follows GraphFillRegion so this construction can inline.
    GraphFillRegion& construct_reserved_seed(
        RegionId id, VaryingCT radius, DetectorNode* node, const Match& match) noexcept;
    // Publish a frozen singleton without touching its full region object.
    // The complete local endpoint can be resolved before its mate is imported.
    void publish_reserved_matched_seed(
        RegionId id, VaryingCT radius, DetectorNode* node, const Match& match, bool defer_node = false) noexcept;
    // Constructor binding is permitted only after the full detector-ID mapping
    // has been checked. Low-level eager imports keep their pointer-based API.
    void bind_compact_seed_nodes(DetectorNode* nodes, size_t count) noexcept;
    void publish_reserved_compact_seed(RegionId id, VaryingCT radius, uint32_t detector,
        uint32_t mate_detector, RegionId mate, obs_int observable) noexcept;
    void release(RegionId id) noexcept;
    void reset() noexcept;
    // Only a successful final extraction grants this one-shot reset token.
    // Aborts, new shots and hard resets cancel it. Taking it precedes owner jobs.
    void mark_completed_shot() noexcept;
    void cancel_neutral_reset() noexcept {
        neutral_reset_end_ = 0;
        contiguous_shot_ = false;
    }
    bool has_pending_neutral_reset() const noexcept {
        return neutral_reset_end_ != 0;
    }
    RegionId take_neutral_reset_end() noexcept;
    // At an idle completed-shot boundary, disjoint owner ranges may construct
    // free raw slots. Untouched lazy slots, neutral objects and radii are left alone.
    // All owners join before any reservation, hard reset, or destruction.
    void prepare_neutral_range(RegionId begin, RegionId end) noexcept;
    bool empty() const noexcept;
    size_t capacity() const noexcept {
        return capacity_;
    }
    RegionGeneration generation(RegionId id) const noexcept {
        return generations_[id];
    }
    bool is_live(RegionId id, RegionGeneration generation) const noexcept {
        return id && id < next_ &&
               (states_[id] == live || states_[id] == live_seed_match || is_lazy_state(states_[id])) &&
               generations_[id] == generation;
    }
};
}  // namespace beyond_blossom::structured
#endif
