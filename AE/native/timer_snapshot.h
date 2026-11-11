// Duration collection stores only durations, fixed endpoints and the partition
// configuration. Algorithm counter fields never belong to a time-only record.
#if !defined(STENCIL_GENERIC_ONLY) && BB_STRUCTURED_PROFILE_TIMERS
#if !BB_STRUCTURED_PROFILE_COUNTERS
struct PredecodeTimerSnapshot {
    uint64_t prepare_ns = 0, handoff_ns = 0, mwpm_ns = 0;
    uint64_t reset_ns = 0, seed_build_ns = 0, normalize_ns = 0;
    uint64_t degree_ns = 0, sort_ns = 0, grow_match_ns = 0;
    uint64_t handoff_regions_ns = 0, handoff_matches_ns = 0, handoff_schedule_ns = 0;
    uint64_t initialization_ns = 0, extract_ns = 0;
    uint64_t scratch_ns = 0, cell_index_ns = 0, complete_ns = 0, import_reserve_ns = 0, import_construct_ns = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    std::array<beyond_blossom::structured::PhaseInterval, 19> timeline{};
#endif
};

inline PredecodeTimerSnapshot snapshot_seed_timers(const beyond_blossom::structured::PredecodeTimingStats& source) {
    PredecodeTimerSnapshot out;
    out.prepare_ns = source.prepare_ns;
    out.handoff_ns = source.handoff_ns;
    out.mwpm_ns = source.mwpm_ns;
    out.reset_ns = source.reset_ns;
    out.seed_build_ns = source.seed_build_ns;
    out.normalize_ns = source.normalize_ns;
    out.degree_ns = source.degree_ns;
    out.sort_ns = source.sort_ns;
    out.grow_match_ns = source.grow_match_ns;
    out.handoff_regions_ns = source.handoff_regions_ns;
    out.handoff_matches_ns = source.handoff_matches_ns;
    out.handoff_schedule_ns = source.handoff_schedule_ns;
    out.initialization_ns = source.initialization_ns;
    out.extract_ns = source.extract_ns;
    out.scratch_ns = source.scratch_ns; out.cell_index_ns = source.cell_index_ns;
    out.complete_ns = source.complete_ns; out.import_reserve_ns = source.import_reserve_ns;
    out.import_construct_ns = source.import_construct_ns;
#if BB_STRUCTURED_PROFILE_TIMELINE
    out.timeline = source.timeline;
#endif
    return out;
}

struct SharedTimerSnapshot {
    struct Component {
        uint64_t total_ns = 0, prepare_ns = 0, predecode_ns = 0, conflict_ns = 0;
        uint64_t join_ns = 0, continuation_ns = 0, clear_ns = 0;
        uint64_t import_ns = 0, import_join_ns = 0;
        uint64_t bucket_count_ns = 0, bucket_reorder_ns = 0, dispatch_ns = 0, partial_merge_ns = 0, import_dispatch_ns = 0;
        std::array<uint64_t, 8> worker_ns{}, import_worker_ns{};
        size_t partitions = 0;
        size_t tail_worker = SIZE_MAX;
        uint64_t tail_ns = 0;
        size_t conflict_owner_lane = 0;
        size_t continuation_worker = SIZE_MAX;
        size_t prefix_worker = SIZE_MAX;
        uint64_t whole_prefix_ns = 0, whole_seed_scatter_ns = 0;
        size_t group_worker = SIZE_MAX;
        uint64_t group_prefix_ns = 0;
        std::array<uint64_t, 8> group_lane_ns{}, group_seed_scatter_ns{};
        std::array<uint64_t, 8> owner_conflict_wait_ns{};
        std::array<std::array<uint64_t, 2>, 8> import_plan_build_ns{};
        std::array<uint64_t, 8> import_plan_interior_wait_ns{}, import_plan_join_ns{};
#if BB_STRUCTURED_PROFILE_TIMELINE
        beyond_blossom::structured::PhaseInterval total_interval, prepare_interval, predecode_interval, conflict_interval;
        beyond_blossom::structured::PhaseInterval join_interval, continuation_interval, clear_interval;
        beyond_blossom::structured::PhaseInterval import_interval, import_join_interval;
        beyond_blossom::structured::PhaseInterval bucket_count_interval, bucket_reorder_interval, dispatch_interval, partial_merge_interval, import_dispatch_interval;
        std::array<beyond_blossom::structured::PhaseInterval, 8> worker_intervals{}, import_worker_intervals{};
        beyond_blossom::structured::PhaseInterval tail_interval;
        beyond_blossom::structured::PhaseInterval whole_prefix_interval, whole_seed_scatter_interval;
        beyond_blossom::structured::PhaseInterval group_prefix_interval;
        std::array<beyond_blossom::structured::PhaseInterval, 8> group_lane_intervals{}, group_seed_scatter_intervals{};
        std::array<beyond_blossom::structured::PhaseInterval, 8> owner_conflict_wait_intervals{};
        std::array<std::array<beyond_blossom::structured::PhaseInterval, 2>, 8> import_plan_build_intervals{};
        std::array<beyond_blossom::structured::PhaseInterval, 8> import_plan_interior_wait_intervals{}, import_plan_join_intervals{};
#endif
    };
    std::array<Component, 2> component{};
    std::array<uint64_t, 8> worker_ns{}, import_worker_ns{};
    std::array<uint64_t, 8> owner_scatter_ns{};
    std::array<uint64_t, 8> owner_conflict_wait_ns{};
    uint64_t partition_ns = 0, dispatch_ns = 0, join_ns = 0, merge_ns = 0, total_ns = 0, prefix_ns = 0;
    uint64_t interior_ns = 0, interior_dispatch_ns = 0, interior_join_ns = 0;
    uint64_t tail_dispatch_ns = 0, tail_join_ns = 0;
    uint64_t continuation_dispatch_ns = 0, continuation_join_ns = 0;
    uint64_t whole_prefix_dispatch_ns = 0, whole_prefix_join_ns = 0;
    uint64_t group_ns = 0, group_dispatch_ns = 0, group_join_ns = 0;
#if BB_STRUCTURED_PROFILE_TIMELINE
    beyond_blossom::structured::PhaseInterval partition_interval, dispatch_interval, join_interval, merge_interval, total_interval, prefix_interval;
    std::array<beyond_blossom::structured::PhaseInterval, 8> worker_intervals{}, import_worker_intervals{};
    std::array<beyond_blossom::structured::PhaseInterval, 8> owner_scatter_intervals{};
    std::array<beyond_blossom::structured::PhaseInterval, 8> owner_conflict_wait_intervals{};
    beyond_blossom::structured::PhaseInterval interior_interval, interior_dispatch_interval, interior_join_interval;
    beyond_blossom::structured::PhaseInterval tail_dispatch_interval, tail_join_interval;
    beyond_blossom::structured::PhaseInterval continuation_dispatch_interval, continuation_join_interval;
    beyond_blossom::structured::PhaseInterval whole_prefix_dispatch_interval, whole_prefix_join_interval;
    beyond_blossom::structured::PhaseInterval group_interval, group_dispatch_interval, group_join_interval;
#endif
};

template <typename Profile>
auto snapshot_profile_timers(const Profile& source) {
    if constexpr (requires { source.component[0].predecode_ns; }) {
        SharedTimerSnapshot out;
        out.partition_ns = source.partition_ns;
        out.dispatch_ns = source.dispatch_ns;
        out.join_ns = source.join_ns;
        out.merge_ns = source.merge_ns;
        out.total_ns = source.total_ns; out.prefix_ns = source.prefix_ns;
        if constexpr (requires { source.lanes; }) {
            for (size_t lane = 0; lane < out.worker_ns.size(); ++lane) {
                out.worker_ns[lane] = source.lanes[lane].worker_ns;
                out.import_worker_ns[lane] = source.lanes[lane].import_worker_ns;
                out.owner_scatter_ns[lane] = source.lanes[lane].owner_scatter_ns;
                out.owner_conflict_wait_ns[lane] = source.lanes[lane].owner_conflict_wait_ns;
            }
        } else {
            out.worker_ns = source.worker_ns;
            out.import_worker_ns = source.import_worker_ns;
        }
        if constexpr (requires { source.owner_scatter_ns; })
            out.owner_scatter_ns = source.owner_scatter_ns;
        if constexpr (requires { source.owner_conflict_wait_ns; })
            out.owner_conflict_wait_ns = source.owner_conflict_wait_ns;
        if constexpr (requires { source.continuation_dispatch_ns; source.continuation_join_ns; }) {
            out.continuation_dispatch_ns = source.continuation_dispatch_ns;
            out.continuation_join_ns = source.continuation_join_ns;
        }
        if constexpr (requires { source.whole_prefix_dispatch_ns; source.whole_prefix_join_ns; }) {
            out.whole_prefix_dispatch_ns = source.whole_prefix_dispatch_ns;
            out.whole_prefix_join_ns = source.whole_prefix_join_ns;
        }
        if constexpr (requires { source.group_ns; source.group_dispatch_ns; source.group_join_ns; }) {
            out.group_ns = source.group_ns;
            out.group_dispatch_ns = source.group_dispatch_ns;
            out.group_join_ns = source.group_join_ns;
        }
        if constexpr (requires { source.interior_ns; }) {
            out.interior_ns = source.interior_ns;
            out.interior_dispatch_ns = source.interior_dispatch_ns;
            out.interior_join_ns = source.interior_join_ns;
            out.tail_dispatch_ns = source.tail_dispatch_ns;
            out.tail_join_ns = source.tail_join_ns;
        }
#if BB_STRUCTURED_PROFILE_TIMELINE
        out.partition_interval = source.partition_interval;
        out.dispatch_interval = source.dispatch_interval;
        out.join_interval = source.join_interval;
        out.merge_interval = source.merge_interval;
        out.total_interval = source.total_interval; out.prefix_interval = source.prefix_interval;
        if constexpr (requires { source.lanes; }) {
            for (size_t lane = 0; lane < out.worker_intervals.size(); ++lane) {
                out.worker_intervals[lane] = source.lanes[lane].worker_interval;
                out.import_worker_intervals[lane] = source.lanes[lane].import_worker_interval;
                out.owner_scatter_intervals[lane] = source.lanes[lane].owner_scatter_interval;
                out.owner_conflict_wait_intervals[lane] = source.lanes[lane].owner_conflict_wait_interval;
            }
        } else {
            out.worker_intervals = source.worker_intervals;
            out.import_worker_intervals = source.import_worker_intervals;
        }
        if constexpr (requires { source.owner_scatter_intervals; })
            out.owner_scatter_intervals = source.owner_scatter_intervals;
        if constexpr (requires { source.owner_conflict_wait_intervals; })
            out.owner_conflict_wait_intervals = source.owner_conflict_wait_intervals;
        if constexpr (requires { source.continuation_dispatch_interval; source.continuation_join_interval; }) {
            out.continuation_dispatch_interval = source.continuation_dispatch_interval;
            out.continuation_join_interval = source.continuation_join_interval;
        }
        if constexpr (requires { source.whole_prefix_dispatch_interval; source.whole_prefix_join_interval; }) {
            out.whole_prefix_dispatch_interval = source.whole_prefix_dispatch_interval;
            out.whole_prefix_join_interval = source.whole_prefix_join_interval;
        }
        if constexpr (requires { source.group_interval; source.group_dispatch_interval; source.group_join_interval; }) {
            out.group_interval = source.group_interval;
            out.group_dispatch_interval = source.group_dispatch_interval;
            out.group_join_interval = source.group_join_interval;
        }
        if constexpr (requires { source.interior_interval; }) {
            out.interior_interval = source.interior_interval;
            out.interior_dispatch_interval = source.interior_dispatch_interval;
            out.interior_join_interval = source.interior_join_interval;
            out.tail_dispatch_interval = source.tail_dispatch_interval;
            out.tail_join_interval = source.tail_join_interval;
        }
#endif
        for (size_t c = 0; c < out.component.size(); ++c) {
            auto& target = out.component[c];
            const auto& part = source.component[c];
            target.total_ns = part.total_ns;
            target.prepare_ns = part.prepare_ns;
            target.predecode_ns = part.predecode_ns;
            target.conflict_ns = part.conflict_ns;
            target.join_ns = part.join_ns;
            target.continuation_ns = part.continuation_ns;
            target.clear_ns = part.clear_ns;
            target.import_ns = part.import_ns;
            target.import_join_ns = part.import_join_ns;
            target.partitions = part.partitions;
            target.bucket_count_ns = part.bucket_count_ns; target.bucket_reorder_ns = part.bucket_reorder_ns;
            target.dispatch_ns = part.dispatch_ns; target.partial_merge_ns = part.partial_merge_ns;
            target.import_dispatch_ns = part.import_dispatch_ns;
            if constexpr (requires { part.lanes; }) {
                for (size_t lane = 0; lane < target.worker_ns.size(); ++lane) {
                    target.worker_ns[lane] = part.lanes[lane].worker_ns;
                    target.import_worker_ns[lane] = part.lanes[lane].import_worker_ns;
                    target.owner_conflict_wait_ns[lane] = part.lanes[lane].owner_conflict_wait_ns;
                    target.group_lane_ns[lane] = part.lanes[lane].group_lane_ns;
                    target.group_seed_scatter_ns[lane] = part.lanes[lane].group_seed_scatter_ns;
                    if constexpr (requires { part.lanes[lane].import_plan_build_ns; }) {
                        target.import_plan_build_ns[lane] = part.lanes[lane].import_plan_build_ns;
                        target.import_plan_interior_wait_ns[lane] = part.lanes[lane].import_plan_interior_wait_ns;
                        target.import_plan_join_ns[lane] = part.lanes[lane].import_plan_join_ns;
                    }
                }
            } else {
                target.worker_ns = part.worker_ns; target.import_worker_ns = part.import_worker_ns;
            }
            if constexpr (requires { part.tail_worker; }) {
                target.tail_worker = part.tail_worker;
                target.tail_ns = part.tail_ns;
            }
            if constexpr (requires { part.conflict_owner_lane; }) target.conflict_owner_lane = part.conflict_owner_lane;
            if constexpr (requires { part.continuation_worker; }) target.continuation_worker = part.continuation_worker;
            if constexpr (requires { part.prefix_worker; part.whole_prefix_ns; part.whole_seed_scatter_ns; }) {
                target.prefix_worker = part.prefix_worker;
                target.whole_prefix_ns = part.whole_prefix_ns;
                target.whole_seed_scatter_ns = part.whole_seed_scatter_ns;
            }
            if constexpr (requires { part.group_worker; part.group_prefix_ns; }) {
                target.group_worker = part.group_worker;
                target.group_prefix_ns = part.group_prefix_ns;
            }
            if constexpr (requires { part.group_lane_ns; part.group_seed_scatter_ns; }) {
                target.group_lane_ns = part.group_lane_ns;
                target.group_seed_scatter_ns = part.group_seed_scatter_ns;
            }
            if constexpr (requires { part.owner_conflict_wait_ns; }) target.owner_conflict_wait_ns = part.owner_conflict_wait_ns;
#if BB_STRUCTURED_PROFILE_TIMELINE
            target.total_interval = part.total_interval;
            target.prepare_interval = part.prepare_interval;
            target.predecode_interval = part.predecode_interval;
            target.conflict_interval = part.conflict_interval;
            target.join_interval = part.join_interval;
            target.continuation_interval = part.continuation_interval;
            target.clear_interval = part.clear_interval;
            target.import_interval = part.import_interval;
            target.import_join_interval = part.import_join_interval;
            target.bucket_count_interval = part.bucket_count_interval; target.bucket_reorder_interval = part.bucket_reorder_interval;
            target.dispatch_interval = part.dispatch_interval; target.partial_merge_interval = part.partial_merge_interval;
            target.import_dispatch_interval = part.import_dispatch_interval;
            if constexpr (requires { part.lanes; }) {
                for (size_t lane = 0; lane < target.worker_intervals.size(); ++lane) {
                    target.worker_intervals[lane] = part.lanes[lane].worker_interval;
                    target.import_worker_intervals[lane] = part.lanes[lane].import_worker_interval;
                    target.owner_conflict_wait_intervals[lane] = part.lanes[lane].owner_conflict_wait_interval;
                    target.group_lane_intervals[lane] = part.lanes[lane].group_lane_interval;
                    target.group_seed_scatter_intervals[lane] = part.lanes[lane].group_seed_scatter_interval;
                    if constexpr (requires { part.lanes[lane].import_plan_build_intervals; }) {
                        target.import_plan_build_intervals[lane] = part.lanes[lane].import_plan_build_intervals;
                        target.import_plan_interior_wait_intervals[lane] = part.lanes[lane].import_plan_interior_wait_interval;
                        target.import_plan_join_intervals[lane] = part.lanes[lane].import_plan_join_interval;
                    }
                }
            } else {
                target.worker_intervals = part.worker_intervals; target.import_worker_intervals = part.import_worker_intervals;
            }
            if constexpr (requires { part.tail_interval; }) target.tail_interval = part.tail_interval;
            if constexpr (requires { part.whole_prefix_interval; part.whole_seed_scatter_interval; }) {
                target.whole_prefix_interval = part.whole_prefix_interval;
                target.whole_seed_scatter_interval = part.whole_seed_scatter_interval;
            }
            if constexpr (requires { part.group_prefix_interval; }) target.group_prefix_interval = part.group_prefix_interval;
            if constexpr (requires { part.group_lane_intervals; part.group_seed_scatter_intervals; }) {
                target.group_lane_intervals = part.group_lane_intervals;
                target.group_seed_scatter_intervals = part.group_seed_scatter_intervals;
            }
            if constexpr (requires { part.owner_conflict_wait_intervals; })
                target.owner_conflict_wait_intervals = part.owner_conflict_wait_intervals;
#endif
        }
        return out;
    } else {
        // ComponentDecodeTimingStats contains only durations and endpoints.
        return source;
    }
}
#else
inline const auto& snapshot_seed_timers(const beyond_blossom::structured::PredecodeTimingStats& source) {
    return source;
}
template <typename Profile>
const Profile& snapshot_profile_timers(const Profile& source) {
    return source;
}
#endif
#endif
