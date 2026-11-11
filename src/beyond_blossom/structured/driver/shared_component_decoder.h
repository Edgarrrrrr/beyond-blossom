// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_SHARED_COMPONENT_DECODER_H
#define BB_SHARED_COMPONENT_DECODER_H
#ifndef BB_SHARED_COMPONENT_PARALLELISM
#define BB_SHARED_COMPONENT_PARALLELISM 2
#endif
#if BB_SHARED_COMPONENT_PARALLELISM != 1 && BB_SHARED_COMPONENT_PARALLELISM != 2
#error "Shared component parallelism must be 1 or 2"
#endif
#include <array>
#include <functional>
#include <memory>
#include "beyond_blossom/structured/driver/component_decoder.h"
#if BB_STRUCTURED_PROFILE_TIMERS
#include "beyond_blossom/structured/driver/import_pmu.h"
#endif

namespace beyond_blossom::structured {
struct SharedDecoderConfiguration {
    enum class FixedSeedConstruction { fused, conflict_first };
    enum class FixedSeedInput { scan, indexed };
    enum class ImportPreparation { serial, parallel, captained, captained_serial, captained_reverse, captained_overlap };
    enum class ImportResetOwner { importers, captain };
    using SeedOrdering = ComponentDecoder::SeedOrdering;
    FixedSeedConstruction fixed_seed_construction = FixedSeedConstruction::fused;
    FixedSeedInput fixed_seed_input = FixedSeedInput::scan;
    ImportPreparation import_preparation = ImportPreparation::serial;
    ImportResetOwner import_reset_owner = ImportResetOwner::importers;
    SeedOrdering seed_ordering = SeedOrdering::reconstruct;
    // Compatibility for the historical explicit experimental constructor.
    // Normal strategy construction passes a value and never reads the environment.
    static SharedDecoderConfiguration from_environment();
};
using FixedSeedConstruction = SharedDecoderConfiguration::FixedSeedConstruction;
using FixedSeedInput = SharedDecoderConfiguration::FixedSeedInput;
using ImportPreparation = SharedDecoderConfiguration::ImportPreparation;
using ImportResetOwner = SharedDecoderConfiguration::ImportResetOwner;
using SeedOrdering = SharedDecoderConfiguration::SeedOrdering;
#ifndef BB_COMPONENT_OWNER_AUDIT
#define BB_COMPONENT_OWNER_AUDIT 0
#endif
enum class SharedRoute { not_run, serial, partitioned, whole_serial };
struct SharedThreadSnapshot {
    // CP1 reuses each lane across components; T1 shares its sole captain too.
    static constexpr size_t SHARED_COMPONENT = SIZE_MAX;
    int requested_cpu = -1;
    std::vector<int> affinity;
    uint64_t thread_id = 0;
    // Darwin requested QoS read back at team initialization; no CPU pinning.
    int qos_class = 0, qos_relative_priority = 0;
    size_t component = 0, lane = 0;
};
struct SharedMatch { int64_t u, v; obs_int obs_mask; };
#if BB_STRUCTURED_PROFILE_TIMERS
// Each timed lane owns complete cache lines. Clock-free builds retain the
// original profile declaration below, so this diagnostic cannot change pure ABI.
struct alignas(64) SharedComponentProfile {
    struct alignas(64) ComponentLane {
        uint64_t worker_ns = 0, import_worker_ns = 0, owner_conflict_wait_ns = 0;
        uint64_t import_roots_ns = 0, import_events_ns = 0;
        uint64_t group_lane_ns = 0, group_seed_scatter_ns = 0;
        uint64_t fused_construct_ns = 0, interior_construct_ns = 0, conflict_construct_ns = 0;
        size_t bounded_input_visits = 0, fused_input_visits = 0, interior_input_visits = 0, conflict_input_visits = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
        PhaseInterval worker_interval, import_worker_interval, owner_conflict_wait_interval;
        PhaseInterval group_lane_interval, group_seed_scatter_interval;
#endif
    };
    struct alignas(64) GlobalLane {
        uint64_t worker_ns = 0, import_worker_ns = 0, owner_scatter_ns = 0, owner_conflict_wait_ns = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
        PhaseInterval worker_interval, import_worker_interval, owner_scatter_interval, owner_conflict_wait_interval;
#endif
    };
    struct alignas(64) Component {
        uint64_t total_ns = 0, prepare_ns = 0, predecode_ns = 0, conflict_ns = 0;
        uint64_t join_ns = 0, continuation_ns = 0, clear_ns = 0;
        // Import includes reservation, object ranges, join, and queue publication.
        // import_join_ns is nested inside import_ns, as join_ns is in predecode_ns.
        uint64_t import_ns = 0, import_join_ns = 0;
        uint64_t bucket_count_ns = 0, bucket_reorder_ns = 0, dispatch_ns = 0, partial_merge_ns = 0;
        uint64_t import_dispatch_ns = 0;
        uint64_t import_scratch_ns = 0, import_root_reserve_ns = 0;
        uint64_t import_prepare_dispatch_ns = 0, import_prepare_join_ns = 0, import_publish_ns = 0;
        bool import_preparation_parallel = false;
        bool import_preparation_captained = false;
        // Indexed by lane within this component, so CP1 never joins X/Z into
        // one envelope. Degree/sort/grow windows are available per bucket.
        std::array<ComponentLane, 8> lanes{};
        size_t seeds = 0, conflict_seeds = 0, partitions = 0, active_threads = 0;
        size_t tail_worker = SIZE_MAX, continuation_worker = SIZE_MAX;
        size_t conflict_owner_lane = 0;
        uint64_t tail_ns = 0;
        // ComponentTail publishes each conflict span on its tail owner.
        // Other lanes acquire that publication before this component's work.
        // WholePrefix uses one captured owner per complete component, with
        // seed fill/index inside its envelope and no spatial bucket activity.
        size_t prefix_worker = SIZE_MAX;
        uint64_t whole_prefix_ns = 0, whole_seed_scatter_ns = 0;
        size_t group_worker = SIZE_MAX;
        uint64_t group_prefix_ns = 0;
        // Fixed construction and predecode, including dispatch/join/count
        // publication, ending before import. This is a component wall window.
        uint64_t fixed_predecode_including_seed_ns = 0;
        bool fixed_spans = false;
        std::array<size_t, 9> fixed_bucket_seeds{};
        SharedRoute route = SharedRoute::not_run;
#if BB_STRUCTURED_PROFILE_TIMELINE
        PhaseInterval total_interval, prepare_interval, predecode_interval, conflict_interval;
        PhaseInterval join_interval, continuation_interval, clear_interval;
        PhaseInterval import_interval, import_join_interval;
        PhaseInterval bucket_count_interval, bucket_reorder_interval, dispatch_interval, partial_merge_interval;
        PhaseInterval import_dispatch_interval;
        PhaseInterval tail_interval;
        PhaseInterval whole_prefix_interval, whole_seed_scatter_interval;
        PhaseInterval group_prefix_interval;
        PhaseInterval fixed_predecode_interval;
#endif
    };
    std::array<Component, 2> component{};
    std::array<GlobalLane, 8> lanes{};
    // One whole-shot scan per active lane fills both components. These are
    // single activities, not duplicated into component or bucket timelines.
    // The legacy schedule waits for the captain to publish both conflict
    // spans. ComponentTail instead records a separate wait per component.
    uint64_t partition_ns = 0, dispatch_ns = 0, join_ns = 0, merge_ns = 0, total_ns = 0;
    uint64_t prefix_ns = 0;
    uint64_t interior_ns = 0, interior_dispatch_ns = 0, interior_join_ns = 0;
    uint64_t tail_dispatch_ns = 0, tail_join_ns = 0;
    // Formal continuation dispatch/join are outside the prepared prefix.
    uint64_t continuation_dispatch_ns = 0, continuation_join_ns = 0;
    uint64_t whole_prefix_dispatch_ns = 0, whole_prefix_join_ns = 0;
    uint64_t group_ns = 0, group_dispatch_ns = 0, group_join_ns = 0;
    // Whole-shot preparation, recorded once before any component is dispatched.
    bool index_input_used = false;
    size_t index_classification_visits = 0, index_entries_written = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    PhaseInterval partition_interval, dispatch_interval, join_interval, merge_interval, total_interval;
    PhaseInterval prefix_interval;
    PhaseInterval interior_interval, interior_dispatch_interval, interior_join_interval;
    PhaseInterval tail_dispatch_interval, tail_join_interval;
    PhaseInterval continuation_dispatch_interval, continuation_join_interval;
    PhaseInterval whole_prefix_dispatch_interval, whole_prefix_join_interval;
    PhaseInterval group_interval, group_dispatch_interval, group_join_interval;
#endif
};

#else
struct SharedComponentProfile {
    struct Component {
        uint64_t total_ns = 0, prepare_ns = 0, predecode_ns = 0, conflict_ns = 0;
        uint64_t join_ns = 0, continuation_ns = 0, clear_ns = 0;
        // Import includes reservation, object ranges, join, and queue publication.
        // import_join_ns is nested inside import_ns, as join_ns is in predecode_ns.
        uint64_t import_ns = 0, import_join_ns = 0;
        uint64_t bucket_count_ns = 0, bucket_reorder_ns = 0, dispatch_ns = 0, partial_merge_ns = 0;
        uint64_t import_dispatch_ns = 0;
        // Indexed by lane within this component, so CP1 never joins X/Z into
        // one envelope. Degree/sort/grow windows are available per bucket.
        std::array<uint64_t, 8> worker_ns{}, import_worker_ns{};
        size_t seeds = 0, conflict_seeds = 0, partitions = 0, active_threads = 0;
        size_t tail_worker = SIZE_MAX, continuation_worker = SIZE_MAX;
        size_t conflict_owner_lane = 0;
        uint64_t tail_ns = 0;
        // ComponentTail publishes each conflict span on its tail owner.
        // Other lanes acquire that publication before this component's work.
        std::array<uint64_t, 8> owner_conflict_wait_ns{};
        // WholePrefix uses one captured owner per complete component, with
        // seed fill/index inside its envelope and no spatial bucket activity.
        size_t prefix_worker = SIZE_MAX;
        uint64_t whole_prefix_ns = 0, whole_seed_scatter_ns = 0;
        size_t group_worker = SIZE_MAX;
        uint64_t group_prefix_ns = 0;
        std::array<uint64_t, 8> group_lane_ns{}, group_seed_scatter_ns{};
        SharedRoute route = SharedRoute::not_run;
#if BB_STRUCTURED_PROFILE_TIMELINE
        PhaseInterval total_interval, prepare_interval, predecode_interval, conflict_interval;
        PhaseInterval join_interval, continuation_interval, clear_interval;
        PhaseInterval import_interval, import_join_interval;
        PhaseInterval bucket_count_interval, bucket_reorder_interval, dispatch_interval, partial_merge_interval;
        PhaseInterval import_dispatch_interval;
        std::array<PhaseInterval, 8> worker_intervals{}, import_worker_intervals{};
        PhaseInterval tail_interval;
        std::array<PhaseInterval, 8> owner_conflict_wait_intervals{};
        PhaseInterval whole_prefix_interval, whole_seed_scatter_interval;
        PhaseInterval group_prefix_interval;
        std::array<PhaseInterval, 8> group_lane_intervals{}, group_seed_scatter_intervals{};
#endif
    };
    std::array<Component, 2> component{};
    std::array<uint64_t, 8> worker_ns{};
    std::array<uint64_t, 8> import_worker_ns{};
    // One whole-shot scan per active lane fills both components. These are
    // single activities, not duplicated into component or bucket timelines.
    std::array<uint64_t, 8> owner_scatter_ns{};
    // The legacy schedule waits for the captain to publish both conflict
    // spans. ComponentTail instead records a separate wait per component.
    std::array<uint64_t, 8> owner_conflict_wait_ns{};
    uint64_t partition_ns = 0, dispatch_ns = 0, join_ns = 0, merge_ns = 0, total_ns = 0;
    uint64_t prefix_ns = 0;
    uint64_t interior_ns = 0, interior_dispatch_ns = 0, interior_join_ns = 0;
    uint64_t tail_dispatch_ns = 0, tail_join_ns = 0;
    // Formal continuation dispatch/join are outside the prepared prefix.
    uint64_t continuation_dispatch_ns = 0, continuation_join_ns = 0;
    uint64_t whole_prefix_dispatch_ns = 0, whole_prefix_join_ns = 0;
    uint64_t group_ns = 0, group_dispatch_ns = 0, group_join_ns = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    PhaseInterval partition_interval, dispatch_interval, join_interval, merge_interval, total_interval;
    PhaseInterval prefix_interval;
    std::array<PhaseInterval, 8> worker_intervals{}, import_worker_intervals{};
    std::array<PhaseInterval, 8> owner_scatter_intervals{};
    std::array<PhaseInterval, 8> owner_conflict_wait_intervals{};
    PhaseInterval interior_interval, interior_dispatch_interval, interior_join_interval;
    PhaseInterval tail_dispatch_interval, tail_join_interval;
    PhaseInterval continuation_dispatch_interval, continuation_join_interval;
    PhaseInterval whole_prefix_dispatch_interval, whole_prefix_join_interval;
    PhaseInterval group_interval, group_dispatch_interval, group_join_interval;
#endif
};

#endif

// One team surrounds a batch. CP2 uses disjoint X/Z groups; CP1 reuses one group
// sequentially across the independent components by default. An explicit prefix
// schedule may overlap their tails while retaining serial formal MWPM.
// The fixed spatial partition count is independent of the physical thread count,
// so T1 can execute the same algorithm sequentially. All seed and dense cell
// state belongs to ComponentDecoder. Workers own only scalar results/scratch views.
class SharedComponentDecoder {
    friend class DoubleBufferComponentDecoder;
    struct State;
    std::unique_ptr<State> state;
    State& checked();
    const State& checked() const;
    // Only DoubleBuffer uses the chained deferred-cleanup execution.
    MatchingResult decode_component_chains(const std::vector<uint64_t>& events);
   public:
    enum class PrefixSchedule : uint8_t {
        component_sequential, component_owned_tail, component_owned_whole, component_group_owned
    };
    enum class ContinuationPlacement : uint8_t { component_captain, prefix_tail_owner };
    static constexpr size_t component_parallelism = BB_SHARED_COMPONENT_PARALLELISM;
#if BB_STRUCTURED_PROFILE_TIMERS
    SharedComponentProfile profile;
#endif
    SharedComponentDecoder() noexcept;
    SharedComponentDecoder(ComponentDecoder&& decoder, std::vector<int> cpus,
                           size_t partitions = 1, size_t component_threshold = 64,
                           size_t shot_threshold = 32, size_t conflict_width = 2, bool parallel_import = false,
                           PrefixSchedule prefix_schedule = PrefixSchedule::component_sequential,
                           ContinuationPlacement continuation = ContinuationPlacement::component_captain,
                           TimePartitionCuts::Geometry geometry = TimePartitionCuts::Geometry::symmetric_width);
    SharedComponentDecoder(ComponentDecoder&& decoder, std::vector<int> cpus,
                           size_t partitions, size_t component_threshold,
                           size_t shot_threshold, size_t conflict_width, bool parallel_import,
                           PrefixSchedule prefix_schedule, ContinuationPlacement continuation,
                           TimePartitionCuts::Geometry geometry, SharedDecoderConfiguration configuration);
    ~SharedComponentDecoder();
    SharedComponentDecoder(SharedComponentDecoder&&);
    SharedComponentDecoder& operator=(SharedComponentDecoder&&);
    SharedComponentDecoder(const SharedComponentDecoder&) = delete;
    SharedComponentDecoder& operator=(const SharedComponentDecoder&) = delete;
    void run_with_team(const std::function<void()>& callback);
    MatchingResult decode(const std::vector<uint64_t>& events);
    void prepare_for_mwpm(const std::vector<uint64_t>& events);
    // Ownership is retired before returning. Imported seed fields finish
    // cleanup on the frozen import-reset owner policy in reset_after_shot; ordinary decode
    // retains its immediate-cleanup contract. Tail-owner placement changes only
    // this split continuation: C0 completes before C1 starts on its tail owner.
    MatchingResult solve_prepared();
    void reset_after_shot();
#if BB_STRUCTURED_PROFILE_TIMERS
    const PredecodeTimingStats& partition_timing(size_t component, size_t bucket) const;
    void reset_import_pmu_profiles();
    const import_pmu::ThreadCounters& import_worker_counters(size_t worker) const;
    const std::array<import_pmu::Aggregate, 3>& import_worker_aggregates(size_t worker) const;
#endif
    std::vector<SharedMatch> decode_to_matches(const std::vector<uint64_t>& events);
    void reset();
    ComponentDecoder& serial_decoder();
    const ComponentDecoder& serial_decoder() const;
    size_t team_size() const;
    const std::vector<SharedThreadSnapshot>& thread_snapshots() const;
    SharedRoute component_route(size_t component) const;
    size_t active_predecode_threads(size_t component) const;
#if BB_COMPONENT_OWNER_AUDIT
    // Audit only: prefix/import, MWPM/extract, joined component reset owners.
    std::array<uint64_t, 3> lifecycle_thread_ids(size_t component) const;
#endif
    // Frozen at construction; eligibility is independent of per-shot fallback.
    const char* fixed_seed_construction_name() const;
    bool fixed_seed_construction_enabled() const;
    const char* fixed_seed_input_name() const;
    const char* import_preparation_name() const;
    const char* import_reset_owner_name() const;
    bool import_preparation_enabled() const;
    size_t fixed_seed_index_entries() const;
};

#if BB_STRUCTURED_PROFILE_TIMERS
struct DoubleBufferResetProfile {
    size_t shot = SIZE_MAX, buffer = 0;
    uint64_t reset_ns = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    PhaseInterval reset_interval;
#endif
};
struct DoubleBufferShotProfile {
    size_t shot = 0, buffer = 0;
    uint64_t ready_wait_ns = 0, decode_ns = 0, prior_reset_ns = 0, result_wait_ns = 0;
    SharedComponentProfile shared;
    ComponentDecodeTimingStats component_decode;
    std::array<PredecodeTimingStats, 2> component;
    std::array<std::array<PredecodeTimingStats, 9>, 2> partition;
    size_t component_count = 0, partition_count = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    PhaseInterval ready_wait_interval, decode_interval, prior_reset_interval, result_wait_interval;
#endif
};
#endif

#if BB_TAILRESET_SYNDROME_STATS
// Each component is captured at its own prefix/solve boundary. The joined
// chains publish these snapshots before asynchronous reset can retire seeds.
struct TailResetSyndromeCounts {
    uint64_t normalized = 0, residual = 0, pairs = 0, boundaries = 0;
};
#endif
// Experimental synchronous stream: each decode call submits one input and
// returns its owned result in order. Two disjoint groups share ONE OpenMP team.
// Group zero's captain also runs the caller callback. On odd shots it publishes
// group one's decode, resets buffer zero, then waits for the result. Its reset
// can delay result delivery and is deliberately inside the caller's latency.
// Group one's reset overlaps the next even decode. run_with_team drains both
// buffers before stopping the team, including empty/odd/exceptional batches.
class DoubleBufferComponentDecoder {
    struct State;
    std::unique_ptr<State> state;
    State& checked();
    const State& checked() const;
   public:
    enum class Schedule { overlap, serial_control, single_thread };
    DoubleBufferComponentDecoder() noexcept;
    DoubleBufferComponentDecoder(SharedComponentDecoder&& even, SharedComponentDecoder&& odd,
                                 Schedule schedule = Schedule::overlap);
    ~DoubleBufferComponentDecoder();
    DoubleBufferComponentDecoder(DoubleBufferComponentDecoder&&);
    DoubleBufferComponentDecoder& operator=(DoubleBufferComponentDecoder&&);
    DoubleBufferComponentDecoder(const DoubleBufferComponentDecoder&) = delete;
    DoubleBufferComponentDecoder& operator=(const DoubleBufferComponentDecoder&) = delete;
    void run_with_team(const std::function<void()>& callback);
    MatchingResult decode(const std::vector<uint64_t>& events);
    // Inside the active callback, wait for all resets. Does not reset parity.
    void drain();
    size_t team_size() const;
    size_t group_size() const;
    size_t next_buffer() const;
#if BB_TAILRESET_SYNDROME_STATS
    std::array<TailResetSyndromeCounts, 2> syndrome_statistics() const;
#endif
    const std::vector<SharedThreadSnapshot>& thread_snapshots(size_t buffer) const;
    const char* fixed_seed_construction_name(size_t buffer) const;
    bool fixed_seed_construction_enabled(size_t buffer) const;
    const char* fixed_seed_input_name(size_t buffer) const;
    const char* import_preparation_name(size_t buffer) const;
    const char* import_reset_owner_name(size_t buffer) const;
    bool import_preparation_enabled(size_t buffer) const;
    size_t fixed_seed_index_entries(size_t buffer) const;
    // Setup metadata only; the team must be inactive. Mutable buffer access
    // would invalidate ownership of the asynchronous reset and is not exposed.
    const ComponentDecoder& serial_decoder(size_t buffer = 0) const;
#if BB_COMPONENT_OWNER_AUDIT
    std::array<uint64_t, 3> lifecycle_thread_ids(size_t buffer, size_t component) const;
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
    // Caller-owned snapshots: decode does not expose worker-owned storage.
    const DoubleBufferShotProfile& shot_profile() const;
    // Acquired when this buffer is reused or drain completes. The shot ID
    // allows the collector to attach delayed resets to their original shots.
    const DoubleBufferResetProfile& completed_reset_profile(size_t buffer) const;
    void reset_import_pmu_profiles();
    const import_pmu::ThreadCounters& import_worker_counters(size_t buffer, size_t worker) const;
    const std::array<import_pmu::Aggregate, 3>& import_worker_aggregates(size_t buffer, size_t worker) const;
#endif
};
}  // namespace beyond_blossom::structured
#endif
