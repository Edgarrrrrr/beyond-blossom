// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_COMPONENT_DECODER_H
#define PYMATCHING_STRUCTURED_COMPONENT_DECODER_H

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include "pymatching/sparse_blossom/grid/grid_spec.h"
#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/structured/driver/time_partition_cuts.h"

namespace pm {
class BackendUserGraph;
}

namespace pm::structured {
class SharedComponentDecoder;

// Every component owns its timeline, queues, search state, regions, and trees.
// Observable IDs and the quantization scale remain global.
class ComponentDecoder {
   public:
    enum class SeedOrdering { reconstruct, cycle_permutation };
    struct Component {
        Mwpm mwpm;
        std::vector<size_t> global_detectors;
        size_t seed_begin = 0;
        size_t seed_end = 0;
        bool has_boundary = false;
        OneHopPredecoder::Grid* seed_grid = nullptr;
    };
    using PartitionedSyndrome = std::vector<std::vector<uint64_t>>;
    struct PartitionSeedLayout {
        const TimePartitionCuts* cuts = nullptr;
        std::array<size_t, 10> offsets{};
    };

   private:
    static constexpr uint16_t NO_COMPONENT = UINT16_MAX, NO_BUCKET = UINT16_MAX;
    struct DetectorAddress {
        uint16_t component = NO_COMPONENT;
        uint16_t bucket = NO_BUCKET;
        uint32_t local = 0;
        uint32_t cell = 0;
        uint32_t last_seen_epoch = 0;
    };
    static_assert(std::is_standard_layout_v<DetectorAddress> && sizeof(DetectorAddress) == 16);
    static_assert(offsetof(DetectorAddress, component) == 0 && offsetof(DetectorAddress, bucket) == 2);
    static_assert(offsetof(DetectorAddress, local) == 4 && offsetof(DetectorAddress, cell) == 8);
    static_assert(offsetof(DetectorAddress, last_seen_epoch) == 12);
    friend struct ComponentDecoderTestAccess;
    friend struct StaticGridComponentTestAccess;
    friend class SharedComponentDecoder;
    // Only the owning Shared constructor can bind and consume this cache.
    // Public dynamic partition APIs never change its membership.
    struct DetectorIdRange { uint64_t min = UINT64_MAX, max = 0; };
    using BucketDetectorRanges = std::array<DetectorIdRange, 9>;
    struct BoundPartitionSeedLayout {
        std::array<size_t, 10> offsets{};
        bool input_ordered = false;
        // Static global-ID bounds contain every detector in an owned bucket.
        // Only strictly increasing input can be narrowed by binary search;
        // the original component/bucket filter still decides which seeds to fill.
        std::span<const uint64_t> bounded_input(std::span<const uint64_t> events, uint32_t mask,
                                               const BucketDetectorRanges& ranges) const noexcept {
            uint64_t min = UINT64_MAX, max = 0;
            for (size_t b = 0; b < ranges.size(); ++b) {
                if ((mask & (uint32_t{1} << b)) && offsets[b] != offsets[b + 1]) {
                    min = std::min(min, ranges[b].min);
                    max = std::max(max, ranges[b].max);
                }
            }
            if (min > max) return {};
            if (!input_ordered) return events;
            const auto begin = std::lower_bound(events.begin(), events.end(), min);
            const auto end = std::upper_bound(begin, events.end(), max);
            return {begin, end};
        }
    };
    // Values, not borrowed pointers, preserve binding under decoder moves.
    std::array<BucketDetectorRanges, 2> bound_detector_ranges{};
    size_t bound_partition_count = 0, bound_conflict_width = 0;
    TimePartitionCuts::Geometry bound_conflict_geometry = TimePartitionCuts::Geometry::symmetric_width;
    bool staged_partition_cached = false;
    bool fixed_seed_storage = false;
    bool serial_seed_storage = false;
    std::vector<uint8_t> serial_detector_routes;
    std::vector<uint32_t> serial_detector_ids;
    size_t logical_seed_count = 0;
    // Detect direct mutable-serial calls that supersede Shared cleanup owners.
    uint64_t seed_storage_generation = 0;
    std::array<std::array<size_t, 10>, 2> fixed_bucket_bases{};
    std::array<size_t, 3> fixed_component_bases{};
    // Constructor-sized shared buckets contain detector IDs, not seeds or
    // thread-owned data. Only each populated prefix belongs to the current shot.
    bool fixed_seed_input_indexed = false;
    std::vector<uint32_t> fixed_bucket_detector_ids;
    // Classification needs only the static bucket route, not the full 16-byte
    // address. Route c*9+b selects a global index span; UINT8_MAX is ignored.
    std::vector<uint8_t> fixed_detector_routes;
    std::array<size_t, 18> fixed_index_starts{}, fixed_index_ends{};
    std::array<std::array<size_t, 9>, 2> fixed_index_counts{};
#if PM_STRUCTURED_PROFILE_TIMERS
    size_t fixed_index_classification_visits = 0, fixed_index_entries_written = 0;
#endif
    void bind_fixed_seed_storage();
    std::array<size_t, 2> classify_serial_shot(const std::vector<uint64_t>& events);
    void scatter_serial_shot(const std::vector<uint64_t>& events, bool build_index);
    void bind_fixed_seed_index();
    bool validate_group_shot(const std::vector<uint64_t>& events, bool advance_epoch = true);
    bool classify_fixed_group_shot(const std::vector<uint64_t>& events);
    void scatter_fixed_group(std::span<const uint64_t> events, size_t component, uint32_t mask,
                             std::array<size_t, 9>& counts) noexcept;
    void scatter_indexed_fixed_group(size_t component, uint32_t mask,
                                    std::array<size_t, 9>& counts) noexcept;
    void bind_partition_buckets(size_t partitions, size_t conflict_width,
                                TimePartitionCuts::Geometry geometry = TimePartitionCuts::Geometry::symmetric_width);
    void stage_bound_partitioned_shot(const std::vector<uint64_t>& events,
                                     std::span<BoundPartitionSeedLayout> layouts);
    void stage_group_partitioned_shot(const std::vector<uint64_t>& events,
                                     std::span<BoundPartitionSeedLayout> layouts);
    void scatter_bound_partitioned_shot(std::span<const uint64_t> events,
        std::span<const BoundPartitionSeedLayout> layouts, uint32_t bucket_mask) noexcept;
    void scatter_bound_partitioned_shot(std::span<const uint64_t> events,
        std::span<const BoundPartitionSeedLayout> layouts,
        std::span<const uint32_t> component_bucket_masks) noexcept;
    // Whole-component owners preserve serial event order without consulting
    // spatial membership. Inactive spans are published only after both jobs join.
    void stage_component_shot(const std::vector<uint64_t>& events);
    void scatter_component_shot(std::span<const uint64_t> events, size_t component) noexcept;
    void publish_staged_shot() noexcept;
    template <bool PublishCells> void publish_staged_shot_impl() noexcept;
    // Component chains publish each cell lifetime before its own solve.
    // The coordinator completes only shared span/backing bookkeeping after join.
    void publish_chained_partitioned_shot() noexcept;
    std::vector<DetectorAddress> detector_addresses;
    std::vector<Component> component_decoders;
    std::vector<uint32_t> partition_positions;
    // Only one of these handles owns seed storage. Retired objects retain
    // their lifetimes until a later resize, without reset-time element writes.
    std::vector<PredecodeSeed> retired_shot_seeds;
    enum class SeedStorageStage { retired, staged, legacy_partial, published };
    SeedStorageStage seed_storage_stage = SeedStorageStage::retired;
    size_t observable_count = 0;
    double weight_scale = 1;
    bool search_graph_included = false;
    bool predecode_enabled_ = PM_PREDECODE_ENABLED;
    bool has_negative_detection_events = false;
    uint32_t shot_epoch = 0;
    bool shot_clean = true;
    bool prefix_ready = false;
    void scatter_shot(const std::vector<uint64_t>& events, bool build_index);
    void scatter_dynamic_shot(const std::vector<uint64_t>& events, bool build_index);
    using ScatterEntry = void (ComponentDecoder::*)(const std::vector<uint64_t>&, bool);
    ScatterEntry scatter_entry_ = &ComponentDecoder::scatter_dynamic_shot;
    void reconstruct_seed_order(std::span<PredecodeSeed> seeds, size_t split);
    void permute_seed_order(std::span<PredecodeSeed> seeds, size_t split);
    using SeedOrderEntry = void (ComponentDecoder::*)(std::span<PredecodeSeed>, size_t);
    SeedOrderEntry seed_order_entry_ = &ComponentDecoder::reconstruct_seed_order;
    void finish_shot_build(bool build_index);
    void clear_shot();
    // Called only after every component owner has retired its own state.
    void retire_shot_storage() noexcept;
    void acquire_seed_storage(SeedStorageStage stage);
    template <typename Count>
    bool count_validated_shot(const std::vector<uint64_t>& events, Count&& count);
    template <bool Cached, bool InputBounds = false, typename Layout>
    void stage_partitioned_shot_impl(const std::vector<uint64_t>& events,
                                    std::span<Layout> layouts, size_t partitions);
    template <bool Cached, typename Layout, typename MaskForComponent>
    void scatter_partitioned_shot_impl(std::span<const uint64_t> events,
                                      std::span<const Layout> layouts,
                                      const MaskForComponent& bucket_mask) noexcept;
    void append_global_edges(const Component& component, const std::vector<int64_t>& local, std::vector<int64_t>& out);

   public:
#if PM_STRUCTURED_PROFILE_TIMERS
    // Per call of the bit-packed decode/decode_partitioned APIs. Component time
    // is an inclusive parent window; merge is only the result accumulation.
    ComponentDecodeTimingStats profile;
#endif
    // Consecutive pairs of global detector IDs; -1 denotes the virtual boundary.
    std::vector<int64_t> match_edges;
    ComponentDecoder() = default;
    ComponentDecoder(
        BackendUserGraph& graph,
        const StaticGridSpec& spec,
        weight_int num_distinct_weights,
        bool ensure_search_graph_included,
        bool predecode_enabled = PM_PREDECODE_ENABLED);
    ComponentDecoder(ComponentDecoder&&) noexcept = default;
    ComponentDecoder& operator=(ComponentDecoder&&) noexcept = default;
    std::vector<Component>& components() {
        return component_decoders;
    }
    const std::vector<Component>& components() const {
        return component_decoders;
    }
    double normalising_constant() const {
        return weight_scale;
    }
    size_t num_nodes() const {
        return detector_addresses.size();
    }
    size_t num_observables() const {
        return observable_count;
    }
    bool has_search_graph() const {
        return search_graph_included;
    }
    bool predecode_enabled() const noexcept {
        return predecode_enabled_;
    }
    // A single dense owner; all component seed spans point into this allocation.
    std::vector<PredecodeSeed> shot_seeds;
    // Configure the original serial fixed-prefix policy before the first shot.
    // Shared spatial construction keeps its independently bound bucket layout.
    bool initialize_serial_seed_storage();
    void configure_seed_ordering(SeedOrdering ordering);
    size_t serial_seed_storage_capacity() const noexcept {
        return serial_seed_storage ? fixed_component_bases[component_decoders.size()] : 0;
    }
    void prepare_shot(const std::vector<uint64_t>& events, bool build_index = true);
    bool supports_owner_scatter() const noexcept { return !has_negative_detection_events; }
    // Staged spans contain retained records and are not eligible for cleanup
    // until every owner has filled its ranges and the coordinator has joined.
    void stage_partitioned_shot(const std::vector<uint64_t>& events,
                                std::span<PartitionSeedLayout> layouts, size_t partitions);
    void scatter_partitioned_shot(std::span<const uint64_t> events,
                                  std::span<const PartitionSeedLayout> layouts, uint32_t bucket_mask) noexcept;
    void scatter_partitioned_shot(std::span<const uint64_t> events,
                                  std::span<const PartitionSeedLayout> layouts,
                                  std::span<const uint32_t> component_bucket_masks) noexcept;
    void publish_partitioned_shot() noexcept;
    // Explicit three-part lifecycle. Both independent components have fully
    // published initial queues when prepare_for_mwpm returns. Snapshot any
    // diagnostics before reset_after_shot, which retires this shot's cells.
    // solve_prepared retires node ownership immediately; imported seeds retain
    // their other ephemeral fields until reset_after_shot or next preparation.
    void prepare_for_mwpm(const std::vector<uint64_t>& events);
    MatchingResult solve_prepared();
    void reset_after_shot();
    void reset();
    static void validate_correlations(bool enabled);
    PartitionedSyndrome partition_detection_events(const std::vector<uint64_t>& events) const;
    MatchingResult decode_partitioned(const PartitionedSyndrome& events);
    MatchingResult decode(const std::vector<uint64_t>& events);
    void decode(const std::vector<uint64_t>& events, uint8_t* observables, total_weight_int& weight);
    void decode_to_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges);
    void decode_to_match_edges(const std::vector<uint64_t>& events);
    void get_nodes_on_shortest_path_from_source(size_t src, size_t dst, std::vector<size_t>& out);
};

MatchingResult decode_detection_events_for_up_to_64_observables(
    ComponentDecoder& decoder, const std::vector<uint64_t>& events, bool correlations);
void decode_detection_events(
    ComponentDecoder& decoder,
    const std::vector<uint64_t>& events,
    uint8_t* observables,
    total_weight_int& weight,
    bool correlations);
void decode_detection_events_to_match_edges(ComponentDecoder& decoder, const std::vector<uint64_t>& events);
void decode_detection_events_to_edges(
    ComponentDecoder& decoder, const std::vector<uint64_t>& events, std::vector<int64_t>& edges);
void decode_detection_events_to_edges_with_edge_correlations(
    ComponentDecoder& decoder, const std::vector<uint64_t>& events, std::vector<int64_t>& edges);

}  // namespace pm::structured
#endif
