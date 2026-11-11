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

#ifndef BB_STRUCTURED_MWPM_H
#define BB_STRUCTURED_MWPM_H

#include <span>

#include "beyond_blossom/core/matching_result.h"
#include "beyond_blossom/structured/arena.h"
#include "beyond_blossom/structured/flooder/graph_flooder.h"
#include "beyond_blossom/structured/matcher/alternating_tree.h"
#include "beyond_blossom/structured/predecode/one_hop_predecoder.h"
#include "beyond_blossom/structured/predecode/predecode_state.h"
#include "beyond_blossom/structured/search/search_flooder.h"

namespace beyond_blossom::structured {

class AltTreeNode;

using MatchingResult = beyond_blossom::MatchingResult;

struct PredecodeImportRoot {
    DetectorNode* node;
    RegionId region;
    cumulative_time_int event_time = 0;
    bool has_event = false;
};

// One independently owned scratch vector per importer lane. Reserve capacity
// outside worker jobs; only unmatched node/rank references are retained.
struct alignas(64) PredecodeImportScratch {
    std::vector<PredecodeImportRoot> roots;
    size_t boundary_count = 0;
    size_t pair_count = 0;
    uint64_t neighbor_scans = 0;

    void clear() noexcept {
        roots.clear();
        boundary_count = pair_count = neighbor_scans = 0;
    }
};

struct Mwpm {
    GraphFlooder flooder;
    StructuredArena<AltTreeNode> node_arena;
    SearchFlooder search_flooder;
    OneHopPredecoder predecoder;
    OneHopPredecoder::Grid* single_predecode_grid() const noexcept {
        return bound_predecode_grid_;
    }

    Mwpm();
    explicit Mwpm(GraphFlooder flooder);
    Mwpm(GraphFlooder flooder, SearchFlooder search_flooder);
    Mwpm(Mwpm&& other) noexcept;
    Mwpm& operator=(Mwpm&& other) noexcept;

    // Cold binding selects one complete preparation/timeline specialization.
    // Both variants coexist in the same library; the event loop has no strategy branch.
    void configure_predecode(bool enabled);
    bool predecode_enabled() const noexcept { return predecode_enabled_; }
    using PreparedTimelineEntry = void (*)(Mwpm&, bool, bool, bool);
    void prepare_bound_timeline(bool run_predecoder, bool state_imported, bool packed);
    void process_bound_timeline(bool run_predecoder, bool state_imported, bool packed);

    GraphFillRegion* region_for(RegionId id) const {
        return flooder.regions->get(id);
    }

    AltTreeNode* make_child(
        AltTreeNode& parent,
        GraphFillRegion* child_inner_region,
        GraphFillRegion* child_outer_region,
        const CompressedEdge& child_inner_to_outer_edge,
        const CompressedEdge& child_compressed_edge);
    void process_event(const beyond_blossom::structured::MwpmEvent& event);
    void handle_blossom_shattering(const BlossomShatterEventData& event);
    void shatter_descendants_into_matches_and_freeze(AltTreeNode& alt_tree_node);
    void handle_tree_hitting_boundary(const RegionHitBoundaryEventData& event);
    void handle_region_hit_region(const MwpmEvent event);
    void handle_tree_hitting_match(
        GraphFillRegion* unmatched_region,
        GraphFillRegion* matched_region,
        const CompressedEdge& unmatched_to_matched_edge);
    void handle_tree_hitting_boundary_match(
        GraphFillRegion* unmatched_region,
        GraphFillRegion* matched_region,
        const CompressedEdge& unmatched_to_matched_edge);
    void handle_tree_hitting_self(const RegionHitRegionEventData& event, AltTreeNode* common_ancestor);
    void handle_tree_hitting_other_tree(const RegionHitRegionEventData& event);
    GraphFillRegion* pair_and_shatter_subblossoms_and_extract_matches(GraphFillRegion* region, MatchingResult& res);
    MatchingResult shatter_blossom_and_extract_matches(GraphFillRegion* region);
    // The split lifecycle keeps seed spans alive until joined post-shot reset.
    MatchingResult shatter_blossom_and_extract_matches_deferring_seed_nodes(GraphFillRegion* region);

    GraphFillRegion* pair_and_shatter_subblossoms_and_extract_match_edges(
        GraphFillRegion* region, std::vector<CompressedEdge>& match_edges);
    void shatter_blossom_and_extract_match_edges(GraphFillRegion* region, std::vector<CompressedEdge>& match_edges);
    void extract_paths_from_match_edges(
        const std::vector<CompressedEdge>& match_edges, uint8_t* obs_begin_ptr, beyond_blossom::total_weight_int& weight);

    void verify_invariants() const;

    void create_detection_event(DetectorNode* node);
    // Precondition: idle MWPM, unique valid detector/cell pairs, even feasible
    // radii, reciprocal tight matches and valid match slots. Matched regions
    // remain in the graph and may be rematched by the ordinary MWPM timeline.
    void import_predecode_state(std::span<const PredecodeSeed> seeds);
    // Internal packed lifecycles own stable seed backing through extract/reset.
    // Public span/range/captained imports retain their owned-metadata contract.
    void import_packed_predecode_state();
    RegionId prepare_packed_fixed_predecode_import();
    template <bool DeferSeedNodes>
    bool try_extract_borrowed_seed_result(MatchingResult& result);
    // Reserve once, construct disjoint ranges, then join all workers before finish.
    // Keep the seed span immutable throughout. On any worker exception, join every
    // worker and call reset() to reclaim constructed and unconstructed reservations.
    RegionId prepare_predecode_import(std::span<const PredecodeSeed> seeds);
    void import_predecode_range(std::span<const PredecodeSeed> seeds, RegionId first, size_t begin, size_t end);
    void finish_predecode_import(std::span<const PredecodeSeed> seeds, RegionId first);
    RegionId prepare_predecode_import(size_t count);
    template <size_t Partitions>
    void import_fixed_predecode_range(const SeedView& seeds, RegionId first, size_t begin, size_t end);
    void finish_fixed_predecode_import(const SeedView& seeds, RegionId first);
    // Optional second parallel stage: collect residual references during region
    // import, join, reserve roots, construct all roots without throwing, then
    // calculate initial events. Join and commit EVERY root before propagating
    // an event-preparation error. Only the captain publishes the event queue.
    void import_predecode_range_preparing(std::span<const PredecodeSeed> seeds, RegionId first,
        size_t begin, size_t end, PredecodeImportScratch& scratch);
    template <size_t Partitions>
    void import_fixed_predecode_range_preparing(const SeedView& seeds, RegionId first,
        size_t begin, size_t end, PredecodeImportScratch& scratch);
    // Captain-only fused import: construct each region/node and its optional
    // root in active-rank order. Helpers may subsequently calculate initial
    // events from disjoint scratch spans, but never write MWPM-owned objects.
    // Reserve scratch.roots for end - begin entries before entering this call.
    template <size_t Partitions, bool Packed = false>
    void import_fixed_predecode_range_captained(const SeedView& seeds, RegionId first,
        size_t begin, size_t end, PredecodeImportScratch& scratch);
    // Same captain-owned persistent writes, without sharing event scratch with
    // a helper that concurrently prepares events from immutable predecode state.
    template <size_t Partitions, bool Packed = false>
    void import_fixed_predecode_range_captained_no_scratch(
        const SeedView& seeds, RegionId first, size_t begin, size_t end);
    // Requires completed predecode and stable active indices/radii. Reads no
    // mutable MWPM node, collision or region state, so it may overlap the above
    // construction. Reserve scratch.roots for end - begin before dispatch.
    // Scratch roots retain active-rank order and can be published after joining.
    void prepare_fixed_predecode_events_from_seeds(const SeedView& seeds, RegionId first,
        size_t begin, size_t end, PredecodeImportScratch& scratch);
    std::span<AltTreeNode*> reserve_predecode_import_roots(size_t count);
    void construct_predecode_import_roots(
        const PredecodeImportScratch& scratch, std::span<AltTreeNode*> storage) noexcept;
    void prepare_predecode_import_events(PredecodeImportScratch& scratch);
    // Writes only the supplied scratch entries and returns this lane's profile
    // count; caller joins all readers before publishing events or running MWPM.
    uint64_t prepare_predecode_import_events(std::span<PredecodeImportRoot> roots);
    void commit_predecode_import_roots(size_t count) noexcept;
    void publish_predecode_import_events(std::span<const PredecodeImportScratch> lanes, size_t total_seeds);
#if BB_ENABLE_PROFILE
    void record_primal_tree_snapshot() const;
#else
    void record_primal_tree_snapshot() const {}
#endif
    void reset();

    // Number of unmatched alternating-tree roots, not the number of '+' nodes.
    // Only the component owner changes this state, including joined import.
    size_t active_tree_count() const noexcept {
        return num_active_trees;
    }

    bool has_pending_seed_cleanup() const noexcept {
        return pending_seed_cleanup;
    }
    void mark_seed_cleanup_pending() noexcept;
    // Disjoint importer ranges; caller joins before finishing/retiring spans.
    void reset_seed_nodes_range(size_t begin, size_t end) noexcept;
    void finish_seed_node_cleanup() noexcept {
        pending_seed_cleanup = false;
    }
    void clear_pending_seed_nodes() noexcept;

   private:
    bool predecode_enabled_ = BB_PREDECODE_ENABLED;
    PreparedTimelineEntry prepare_timeline_entry_ = nullptr;
    PreparedTimelineEntry process_timeline_entry_ = nullptr;
    friend struct SerialImportTestAccess;
    using ImportEntry = void (Mwpm::*)(std::span<const PredecodeSeed>);
    template <bool DirectNodes, bool Borrow = false>
    void import_predecode_state_impl(std::span<const PredecodeSeed> seeds);
    ImportEntry import_entry = &Mwpm::import_predecode_state_impl<false>;
    ImportEntry packed_import_entry = &Mwpm::import_predecode_state_impl<false>;
    using FixedPrepareEntry = RegionId (Mwpm::*)();
    template <bool Borrow> RegionId prepare_packed_fixed_import_impl();
    FixedPrepareEntry packed_fixed_prepare_entry = &Mwpm::prepare_packed_fixed_import_impl<false>;
    using CaptainedImportEntry = void (Mwpm::*)(const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
    using CaptainedNoScratchEntry = void (Mwpm::*)(const SeedView&, RegionId, size_t, size_t);
    template <size_t Partitions, bool DirectNodes, bool Borrow = false>
    void import_fixed_captained_impl(const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
    template <size_t Partitions, bool DirectNodes, bool Borrow = false>
    void import_fixed_captained_no_scratch_impl(const SeedView&, RegionId, size_t, size_t);
    std::array<CaptainedImportEntry, 3> captained_import_entries{
        &Mwpm::import_fixed_captained_impl<2, false>, &Mwpm::import_fixed_captained_impl<4, false>,
        &Mwpm::import_fixed_captained_impl<8, false>};
    std::array<CaptainedNoScratchEntry, 3> captained_no_scratch_entries{
        &Mwpm::import_fixed_captained_no_scratch_impl<2, false>, &Mwpm::import_fixed_captained_no_scratch_impl<4, false>,
        &Mwpm::import_fixed_captained_no_scratch_impl<8, false>};
    std::array<CaptainedImportEntry, 3> packed_captained_import_entries = captained_import_entries;
    std::array<CaptainedNoScratchEntry, 3> packed_captained_no_scratch_entries = captained_no_scratch_entries;
    DetectorNode* direct_import_nodes = nullptr;
    OneHopPredecoder::Grid* bound_predecode_grid_ = nullptr;
    void bind_predecode_import() noexcept;
    std::vector<DetectorNode*> serial_import_residual_nodes_;
    std::vector<uint64_t> changed_seed_bits_;
    size_t num_active_trees = 0;
    bool pending_seed_cleanup = false;
    template <bool Prepare = false, bool Captained = false, bool DirectNodes = false, bool Serial = false, bool Borrow = false, typename MateRank>
    void import_predecode_slots(std::span<const PredecodeSeed> seeds, RegionId first,
        size_t begin, size_t end, size_t rank_begin, const MateRank& mate_rank,
        PredecodeImportScratch* scratch = nullptr);
    template <bool defer_seed_nodes>
    GraphFillRegion* pair_and_shatter_subblossoms_and_extract_matches_impl(GraphFillRegion* region, MatchingResult& res);
    template <bool defer_seed_nodes>
    MatchingResult shatter_blossom_and_extract_matches_impl(GraphFillRegion* region);
};
}  // namespace beyond_blossom::structured

#include "beyond_blossom/structured/matcher/mwpm.inl"

#endif  // BB_STRUCTURED_MWPM_H
