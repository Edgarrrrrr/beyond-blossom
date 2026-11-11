// Separate duration diagnostics. X/Z and worker windows may overlap.
#if !defined(STENCIL_GENERIC_ONLY) && PM_STRUCTURED_PROFILE_TIMERS
template <typename Component>
const auto& component_seed_stats(const Component& part) {
    if constexpr (requires { part.predecoder.stats; }) return part.predecoder.stats;
    else return part.mwpm.predecoder.stats;
}
// Static address evidence is captured after the cold check and before warmup.
// It never reads timer values or runs between the measured endpoints.
template <typename Decoder>
void write_profile_layout(const Options& o, Decoder& decoder) {
    const auto& source = decoder.profile;
    std::ofstream out(o.output / "profile_layout.csv");
    out << "component_id,lane,worker_index,active_worker,layout,field,address,bytes,cacheline_first,cacheline_last,storage_kind,bucket_id\n";
    auto field = [&](int component, size_t lane, size_t worker, bool active,
                     const char* layout, const char* name, const auto& value, const char* kind = "field", int bucket_id = -1) {
        const auto address = reinterpret_cast<uintptr_t>(std::addressof(value));
        const size_t bytes = sizeof(value);
        out << component << ',' << lane << ',' << worker << ',' << unsigned(active) << ',' << layout << ','
            << name << ',' << address << ',' << bytes << ',' << address / 64 << ','
            << (address + bytes - 1) / 64 << ',' << kind << ',' << bucket_id << '\n';
    };
    auto scalar_fields = [&](int component, size_t worker, const char* layout, const auto& value) {
        if constexpr (requires { value.partition_ns; }) field(component, 0, worker, true, layout, "partition_ns", value.partition_ns, "coordinator_field");
        if constexpr (requires { value.dispatch_ns; }) field(component, 0, worker, true, layout, "dispatch_ns", value.dispatch_ns, "coordinator_field");
        if constexpr (requires { value.join_ns; }) field(component, 0, worker, true, layout, "join_ns", value.join_ns, "coordinator_field");
        if constexpr (requires { value.merge_ns; }) field(component, 0, worker, true, layout, "merge_ns", value.merge_ns, "coordinator_field");
        if constexpr (requires { value.total_ns; }) field(component, 0, worker, true, layout, "total_ns", value.total_ns, "coordinator_field");
        if constexpr (requires { value.prefix_ns; }) field(component, 0, worker, true, layout, "prefix_ns", value.prefix_ns, "coordinator_field");
        if constexpr (requires { value.interior_ns; }) field(component, 0, worker, true, layout, "interior_ns", value.interior_ns, "coordinator_field");
        if constexpr (requires { value.interior_dispatch_ns; }) field(component, 0, worker, true, layout, "interior_dispatch_ns", value.interior_dispatch_ns, "coordinator_field");
        if constexpr (requires { value.interior_join_ns; }) field(component, 0, worker, true, layout, "interior_join_ns", value.interior_join_ns, "coordinator_field");
        if constexpr (requires { value.tail_dispatch_ns; }) field(component, 0, worker, true, layout, "tail_dispatch_ns", value.tail_dispatch_ns, "coordinator_field");
        if constexpr (requires { value.tail_join_ns; }) field(component, 0, worker, true, layout, "tail_join_ns", value.tail_join_ns, "coordinator_field");
        if constexpr (requires { value.continuation_dispatch_ns; }) field(component, 0, worker, true, layout, "continuation_dispatch_ns", value.continuation_dispatch_ns, "coordinator_field");
        if constexpr (requires { value.continuation_join_ns; }) field(component, 0, worker, true, layout, "continuation_join_ns", value.continuation_join_ns, "coordinator_field");
        if constexpr (requires { value.whole_prefix_dispatch_ns; }) field(component, 0, worker, true, layout, "whole_prefix_dispatch_ns", value.whole_prefix_dispatch_ns, "coordinator_field");
        if constexpr (requires { value.whole_prefix_join_ns; }) field(component, 0, worker, true, layout, "whole_prefix_join_ns", value.whole_prefix_join_ns, "coordinator_field");
        if constexpr (requires { value.group_ns; }) field(component, 0, worker, true, layout, "group_ns", value.group_ns, "coordinator_field");
        if constexpr (requires { value.group_dispatch_ns; }) field(component, 0, worker, true, layout, "group_dispatch_ns", value.group_dispatch_ns, "coordinator_field");
        if constexpr (requires { value.group_join_ns; }) field(component, 0, worker, true, layout, "group_join_ns", value.group_join_ns, "coordinator_field");
        if constexpr (requires { value.prepare_ns; }) field(component, 0, worker, true, layout, "prepare_ns", value.prepare_ns, "coordinator_field");
        if constexpr (requires { value.predecode_ns; }) field(component, 0, worker, true, layout, "predecode_ns", value.predecode_ns, "coordinator_field");
        if constexpr (requires { value.conflict_ns; }) field(component, 0, worker, true, layout, "conflict_ns", value.conflict_ns, "coordinator_field");
        if constexpr (requires { value.continuation_ns; }) field(component, 0, worker, true, layout, "continuation_ns", value.continuation_ns, "coordinator_field");
        if constexpr (requires { value.clear_ns; }) field(component, 0, worker, true, layout, "clear_ns", value.clear_ns, "coordinator_field");
        if constexpr (requires { value.import_ns; }) field(component, 0, worker, true, layout, "import_ns", value.import_ns, "coordinator_field");
        if constexpr (requires { value.import_join_ns; }) field(component, 0, worker, true, layout, "import_join_ns", value.import_join_ns, "coordinator_field");
        if constexpr (requires { value.bucket_count_ns; }) field(component, 0, worker, true, layout, "bucket_count_ns", value.bucket_count_ns, "coordinator_field");
        if constexpr (requires { value.bucket_reorder_ns; }) field(component, 0, worker, true, layout, "bucket_reorder_ns", value.bucket_reorder_ns, "coordinator_field");
        if constexpr (requires { value.partial_merge_ns; }) field(component, 0, worker, true, layout, "partial_merge_ns", value.partial_merge_ns, "coordinator_field");
        if constexpr (requires { value.import_dispatch_ns; }) field(component, 0, worker, true, layout, "import_dispatch_ns", value.import_dispatch_ns, "coordinator_field");
        if constexpr (requires { value.tail_ns; }) field(component, 0, worker, true, layout, "tail_ns", value.tail_ns, "coordinator_field");
        if constexpr (requires { value.whole_prefix_ns; }) field(component, 0, worker, true, layout, "whole_prefix_ns", value.whole_prefix_ns, "coordinator_field");
        if constexpr (requires { value.whole_seed_scatter_ns; }) field(component, 0, worker, true, layout, "whole_seed_scatter_ns", value.whole_seed_scatter_ns, "coordinator_field");
        if constexpr (requires { value.group_prefix_ns; }) field(component, 0, worker, true, layout, "group_prefix_ns", value.group_prefix_ns, "coordinator_field");
#if PM_STRUCTURED_PROFILE_TIMELINE
        if constexpr (requires { value.partition_interval; }) field(component, 0, worker, true, layout, "partition_interval", value.partition_interval, "coordinator_field");
        if constexpr (requires { value.dispatch_interval; }) field(component, 0, worker, true, layout, "dispatch_interval", value.dispatch_interval, "coordinator_field");
        if constexpr (requires { value.join_interval; }) field(component, 0, worker, true, layout, "join_interval", value.join_interval, "coordinator_field");
        if constexpr (requires { value.merge_interval; }) field(component, 0, worker, true, layout, "merge_interval", value.merge_interval, "coordinator_field");
        if constexpr (requires { value.total_interval; }) field(component, 0, worker, true, layout, "total_interval", value.total_interval, "coordinator_field");
        if constexpr (requires { value.prefix_interval; }) field(component, 0, worker, true, layout, "prefix_interval", value.prefix_interval, "coordinator_field");
        if constexpr (requires { value.interior_interval; }) field(component, 0, worker, true, layout, "interior_interval", value.interior_interval, "coordinator_field");
        if constexpr (requires { value.interior_dispatch_interval; }) field(component, 0, worker, true, layout, "interior_dispatch_interval", value.interior_dispatch_interval, "coordinator_field");
        if constexpr (requires { value.interior_join_interval; }) field(component, 0, worker, true, layout, "interior_join_interval", value.interior_join_interval, "coordinator_field");
        if constexpr (requires { value.tail_dispatch_interval; }) field(component, 0, worker, true, layout, "tail_dispatch_interval", value.tail_dispatch_interval, "coordinator_field");
        if constexpr (requires { value.tail_join_interval; }) field(component, 0, worker, true, layout, "tail_join_interval", value.tail_join_interval, "coordinator_field");
        if constexpr (requires { value.continuation_dispatch_interval; }) field(component, 0, worker, true, layout, "continuation_dispatch_interval", value.continuation_dispatch_interval, "coordinator_field");
        if constexpr (requires { value.continuation_join_interval; }) field(component, 0, worker, true, layout, "continuation_join_interval", value.continuation_join_interval, "coordinator_field");
        if constexpr (requires { value.whole_prefix_dispatch_interval; }) field(component, 0, worker, true, layout, "whole_prefix_dispatch_interval", value.whole_prefix_dispatch_interval, "coordinator_field");
        if constexpr (requires { value.whole_prefix_join_interval; }) field(component, 0, worker, true, layout, "whole_prefix_join_interval", value.whole_prefix_join_interval, "coordinator_field");
        if constexpr (requires { value.group_interval; }) field(component, 0, worker, true, layout, "group_interval", value.group_interval, "coordinator_field");
        if constexpr (requires { value.group_dispatch_interval; }) field(component, 0, worker, true, layout, "group_dispatch_interval", value.group_dispatch_interval, "coordinator_field");
        if constexpr (requires { value.group_join_interval; }) field(component, 0, worker, true, layout, "group_join_interval", value.group_join_interval, "coordinator_field");
        if constexpr (requires { value.prepare_interval; }) field(component, 0, worker, true, layout, "prepare_interval", value.prepare_interval, "coordinator_field");
        if constexpr (requires { value.predecode_interval; }) field(component, 0, worker, true, layout, "predecode_interval", value.predecode_interval, "coordinator_field");
        if constexpr (requires { value.conflict_interval; }) field(component, 0, worker, true, layout, "conflict_interval", value.conflict_interval, "coordinator_field");
        if constexpr (requires { value.continuation_interval; }) field(component, 0, worker, true, layout, "continuation_interval", value.continuation_interval, "coordinator_field");
        if constexpr (requires { value.clear_interval; }) field(component, 0, worker, true, layout, "clear_interval", value.clear_interval, "coordinator_field");
        if constexpr (requires { value.import_interval; }) field(component, 0, worker, true, layout, "import_interval", value.import_interval, "coordinator_field");
        if constexpr (requires { value.import_join_interval; }) field(component, 0, worker, true, layout, "import_join_interval", value.import_join_interval, "coordinator_field");
        if constexpr (requires { value.bucket_count_interval; }) field(component, 0, worker, true, layout, "bucket_count_interval", value.bucket_count_interval, "coordinator_field");
        if constexpr (requires { value.bucket_reorder_interval; }) field(component, 0, worker, true, layout, "bucket_reorder_interval", value.bucket_reorder_interval, "coordinator_field");
        if constexpr (requires { value.partial_merge_interval; }) field(component, 0, worker, true, layout, "partial_merge_interval", value.partial_merge_interval, "coordinator_field");
        if constexpr (requires { value.import_dispatch_interval; }) field(component, 0, worker, true, layout, "import_dispatch_interval", value.import_dispatch_interval, "coordinator_field");
        if constexpr (requires { value.tail_interval; }) field(component, 0, worker, true, layout, "tail_interval", value.tail_interval, "coordinator_field");
        if constexpr (requires { value.whole_prefix_interval; }) field(component, 0, worker, true, layout, "whole_prefix_interval", value.whole_prefix_interval, "coordinator_field");
        if constexpr (requires { value.whole_seed_scatter_interval; }) field(component, 0, worker, true, layout, "whole_seed_scatter_interval", value.whole_seed_scatter_interval, "coordinator_field");
        if constexpr (requires { value.group_prefix_interval; }) field(component, 0, worker, true, layout, "group_prefix_interval", value.group_prefix_interval, "coordinator_field");
#endif
    };
    if constexpr (requires { source.component[0].predecode_ns; }) {
        scalar_fields(-1, 0, requires { source.lanes; } ? "AoS" : "dense", source);
        if constexpr (requires { source.lanes; }) {
            for (size_t lane = 0; lane < source.lanes.size(); ++lane) {
                const auto& v = source.lanes[lane];
                const bool active = lane < o.cpus.size();
                field(-1, lane, lane, active, "AoS", "lane_storage", v, "lane_storage");
                field(-1, lane, lane, active, "AoS", "worker_ns", v.worker_ns);
                field(-1, lane, lane, active, "AoS", "import_worker_ns", v.import_worker_ns);
                field(-1, lane, lane, active, "AoS", "owner_scatter_ns", v.owner_scatter_ns);
                field(-1, lane, lane, active, "AoS", "owner_conflict_wait_ns", v.owner_conflict_wait_ns);
#if PM_STRUCTURED_PROFILE_TIMELINE
                field(-1, lane, lane, active, "AoS", "worker_interval", v.worker_interval);
                field(-1, lane, lane, active, "AoS", "import_worker_interval", v.import_worker_interval);
                field(-1, lane, lane, active, "AoS", "owner_scatter_interval", v.owner_scatter_interval);
                field(-1, lane, lane, active, "AoS", "owner_conflict_wait_interval", v.owner_conflict_wait_interval);
#endif
            }
        } else {
            for (size_t lane = 0; lane < source.worker_ns.size(); ++lane) {
                const bool active = lane < o.cpus.size();
                field(-1, lane, lane, active, "dense", "worker_ns", source.worker_ns[lane]);
                field(-1, lane, lane, active, "dense", "import_worker_ns", source.import_worker_ns[lane]);
                if constexpr (requires { source.owner_scatter_ns; })
                    field(-1, lane, lane, active, "dense", "owner_scatter_ns", source.owner_scatter_ns[lane]);
                if constexpr (requires { source.owner_conflict_wait_ns; })
                    field(-1, lane, lane, active, "dense", "owner_conflict_wait_ns", source.owner_conflict_wait_ns[lane]);
#if PM_STRUCTURED_PROFILE_TIMELINE
                field(-1, lane, lane, active, "dense", "worker_interval", source.worker_intervals[lane]);
                field(-1, lane, lane, active, "dense", "import_worker_interval", source.import_worker_intervals[lane]);
                if constexpr (requires { source.owner_scatter_intervals; })
                    field(-1, lane, lane, active, "dense", "owner_scatter_interval", source.owner_scatter_intervals[lane]);
                if constexpr (requires { source.owner_conflict_wait_intervals; })
                    field(-1, lane, lane, active, "dense", "owner_conflict_wait_interval", source.owner_conflict_wait_intervals[lane]);
#endif
            }
        }
        const size_t group_threads = o.component_parallelism == 2 && o.cpus.size() > 1 ? o.cpus.size() / 2 : o.cpus.size();
        for (size_t c = 0; c < source.component.size(); ++c) {
            const auto& p = source.component[c];
            size_t active_threads = group_threads;
            if constexpr (requires { decoder.active_predecode_threads(c); }) active_threads = decoder.active_predecode_threads(c);
            const size_t captain = o.component_parallelism == 2 && o.cpus.size() > 1 ? c * group_threads : 0;
            scalar_fields(c, captain, requires { p.lanes; } ? "AoS" : "dense", p);
            if constexpr (requires { decoder.partition_timing(c, size_t{0}); decoder.component_route(c); }) {
                if (static_cast<int>(decoder.component_route(c)) == 2) {
                    for (size_t b = 0; b <= o.partitions; ++b) {
                        const auto& stats = decoder.partition_timing(c, b);
                        const size_t lane = b == o.partitions ? 0 : b % std::max(size_t{1}, active_threads);
                        const size_t worker = captain + lane;
                        field(c, lane, worker, true, "partial", "stats_storage", stats, "partial_storage", int(b));
                        field(c, lane, worker, true, "partial", "prepare_ns", stats.prepare_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "handoff_ns", stats.handoff_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "mwpm_ns", stats.mwpm_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "reset_ns", stats.reset_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "seed_build_ns", stats.seed_build_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "normalize_ns", stats.normalize_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "degree_ns", stats.degree_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "sort_ns", stats.sort_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "grow_match_ns", stats.grow_match_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "handoff_regions_ns", stats.handoff_regions_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "handoff_matches_ns", stats.handoff_matches_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "handoff_schedule_ns", stats.handoff_schedule_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "initialization_ns", stats.initialization_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "extract_ns", stats.extract_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "scratch_ns", stats.scratch_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "cell_index_ns", stats.cell_index_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "complete_ns", stats.complete_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "import_reserve_ns", stats.import_reserve_ns, "partial_field", int(b));
                        field(c, lane, worker, true, "partial", "import_construct_ns", stats.import_construct_ns, "partial_field", int(b));
#if PM_STRUCTURED_PROFILE_TIMELINE
                        for (size_t phase = 0; phase < stats.timeline.size(); ++phase) {
                            const auto name = "phase_" + std::to_string(phase);
                            field(c, lane, worker, true, "partial", name.c_str(), stats.timeline[phase], "partial_field", int(b));
                        }
#endif
                    }
                }
            }
            if constexpr (requires { p.lanes; }) {
                for (size_t lane = 0; lane < p.lanes.size(); ++lane) {
                    const auto& v = p.lanes[lane]; const size_t worker = captain + lane;
                    const bool active = lane < active_threads;
                    field(c, lane, worker, active, "AoS", "lane_storage", v, "lane_storage");
                    field(c, lane, worker, active, "AoS", "worker_ns", v.worker_ns);
                    field(c, lane, worker, active, "AoS", "import_worker_ns", v.import_worker_ns);
                    field(c, lane, worker, active, "AoS", "owner_conflict_wait_ns", v.owner_conflict_wait_ns);
                    field(c, lane, worker, active, "AoS", "group_lane_ns", v.group_lane_ns);
                    field(c, lane, worker, active, "AoS", "group_seed_scatter_ns", v.group_seed_scatter_ns);
                    if constexpr (requires { v.import_plan_build_ns; }) {
                        field(c, lane, worker, active, "AoS", "import_plan_build_ns", v.import_plan_build_ns);
                        field(c, lane, worker, active, "AoS", "import_plan_interior_wait_ns", v.import_plan_interior_wait_ns);
                        field(c, lane, worker, active, "AoS", "import_plan_join_ns", v.import_plan_join_ns);
                    }
#if PM_STRUCTURED_PROFILE_TIMELINE
                    field(c, lane, worker, active, "AoS", "worker_interval", v.worker_interval);
                    field(c, lane, worker, active, "AoS", "import_worker_interval", v.import_worker_interval);
                    field(c, lane, worker, active, "AoS", "owner_conflict_wait_interval", v.owner_conflict_wait_interval);
                    field(c, lane, worker, active, "AoS", "group_lane_interval", v.group_lane_interval);
                    field(c, lane, worker, active, "AoS", "group_seed_scatter_interval", v.group_seed_scatter_interval);
                    if constexpr (requires { v.import_plan_build_intervals; }) {
                        field(c, lane, worker, active, "AoS", "import_plan_build_intervals", v.import_plan_build_intervals);
                        field(c, lane, worker, active, "AoS", "import_plan_interior_wait_interval", v.import_plan_interior_wait_interval);
                        field(c, lane, worker, active, "AoS", "import_plan_join_interval", v.import_plan_join_interval);
                    }
#endif
                }
            } else {
                for (size_t lane = 0; lane < p.worker_ns.size(); ++lane) {
                    const size_t worker = captain + lane; const bool active = lane < active_threads;
                    field(c, lane, worker, active, "dense", "worker_ns", p.worker_ns[lane]);
                    field(c, lane, worker, active, "dense", "import_worker_ns", p.import_worker_ns[lane]);
                    if constexpr (requires { p.owner_conflict_wait_ns; }) field(c, lane, worker, active, "dense", "owner_conflict_wait_ns", p.owner_conflict_wait_ns[lane]);
                    if constexpr (requires { p.group_lane_ns; }) {
                        field(c, lane, worker, active, "dense", "group_lane_ns", p.group_lane_ns[lane]);
                        field(c, lane, worker, active, "dense", "group_seed_scatter_ns", p.group_seed_scatter_ns[lane]);
                    }
#if PM_STRUCTURED_PROFILE_TIMELINE
                    field(c, lane, worker, active, "dense", "worker_interval", p.worker_intervals[lane]);
                    field(c, lane, worker, active, "dense", "import_worker_interval", p.import_worker_intervals[lane]);
                    if constexpr (requires { p.owner_conflict_wait_intervals; }) field(c, lane, worker, active, "dense", "owner_conflict_wait_interval", p.owner_conflict_wait_intervals[lane]);
                    if constexpr (requires { p.group_lane_intervals; }) {
                        field(c, lane, worker, active, "dense", "group_lane_interval", p.group_lane_intervals[lane]);
                        field(c, lane, worker, active, "dense", "group_seed_scatter_interval", p.group_seed_scatter_intervals[lane]);
                    }
#endif
                }
            }
        }
    }
    if (!out) throw std::runtime_error("Profile layout persistence failed");
}

template <typename Decoder>
std::vector<pm::MatchingResult> profile_parallel(const Options& o, const Dataset& data, Decoder& decoder,
                                              const ClockCalibration& calibration) {
    using Stats = std::decay_t<decltype(snapshot_profile_timers(decoder.profile))>;
    using SeedBuildStats = std::decay_t<decltype(snapshot_profile_timers(metadata_decoder(decoder).profile))>;
    const bool explicit_lifecycle = o.mode == "prefix-timeline" || o.mode == "prefix-stages" || o.mode == "queue-stages";
    using SeedStats = std::decay_t<decltype(snapshot_seed_timers(
        component_seed_stats(metadata_decoder(decoder).components().front())))>;
    struct ComponentRecord {
        SeedStats seed;
#if PM_STRUCTURED_PROFILE_COUNTERS
        pm::structured::QueueProfileStats queue;
        uint64_t live_end = 0, live_reset = 0, queued_trackers_end = 0, queued_trackers_reset = 0;
        int64_t active_roots_end = -1, active_roots_reset = -1;
        pm::structured::QueueProfileStats queue_after_reset;
#endif
        int route = -1;
        size_t active_threads = 1;
    };
    struct Record {
        pm::MatchingResult value;
        uint64_t wall_ns = 0;
uint64_t start_ns = 0, prefix_end_ns = 0, end_ns = 0, reset_start_ns = 0, reset_end_ns = 0;
        Stats stats;
        SeedBuildStats seed_build;
        std::array<ComponentRecord, 2> components;
        std::array<std::array<SeedStats, 3>, 2> buckets{};
    };
    std::vector<Record> records(data.hits.size());
    rusage usage_begin{}, usage_end{};
    uint64_t outer_wall_ns = 0, warmup_shots = 0;
    Measurement warmup;
    decode_window(o, decoder, [&] {
        if (explicit_lifecycle) {
            verify_prepared_state(o, data, decoder);
            write_profile_layout(o, decoder);
        }
        warmup.frequency_before_warmup = read_cpu_frequencies(o);
        const auto begin = Clock::now();
        const auto prefix = std::min(size_t{1024}, data.hits.size());
        do {
            for (size_t k = 0; k < prefix; ++k) {
                const auto value = explicit_lifecycle
                    ? (lifecycle_prepare(decoder, data.hits[k]), lifecycle_solve(decoder))
                    : decoder.decode(data.hits[k]);
                if (explicit_lifecycle) lifecycle_reset(decoder);
                if (value.weight != data.expected[k].weight || (o.verify == "exact" && value != data.expected[k]))
                    throw std::runtime_error("Diagnostic warmup optimum/result mismatch");
            }
            warmup_shots += prefix;
        } while (std::chrono::duration<double>(Clock::now() - begin).count() < o.warmup_seconds);
        warmup.warmup_shots = warmup_shots;
        warmup.warmup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        warmup.frequency_after_warmup = read_cpu_frequencies(o);
        if (getrusage(RUSAGE_SELF, &usage_begin)) throw std::runtime_error("CPU snapshot start failed");
        const auto loop_start = Clock::now();
        for (size_t k = 0; k < data.hits.size(); k++) {
            const auto start = Clock::now();
            if (explicit_lifecycle) {
                lifecycle_prepare(decoder, data.hits[k]);
                records[k].prefix_end_ns = clock_ns(Clock::now());
                records[k].value = lifecycle_solve(decoder);
            } else records[k].value = decoder.decode(data.hits[k]);
            const auto stop = Clock::now();
            records[k].wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
#if PM_STRUCTURED_PROFILE_TIMELINE
            records[k].start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
            records[k].end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop.time_since_epoch()).count();
#else
            if (explicit_lifecycle) {
                records[k].start_ns = clock_ns(start);
                records[k].end_ns = clock_ns(stop);
            }
#endif
            records[k].stats = snapshot_profile_timers(decoder.profile);
            records[k].seed_build = snapshot_profile_timers(metadata_decoder(decoder).profile);
            for (size_t c = 0; c < 2; c++) {
                const auto& part = metadata_decoder(decoder).components()[c];
                records[k].components[c].seed = snapshot_seed_timers(component_seed_stats(part));
#if PM_STRUCTURED_PROFILE_COUNTERS
                records[k].components[c].queue = part.mwpm.flooder.queue.profile;
                records[k].components[c].live_end = part.mwpm.flooder.queue.size();
                if constexpr (requires { part.mwpm.active_tree_count(); })
                    records[k].components[c].active_roots_end = part.mwpm.active_tree_count();
                for (size_t id = 0; id < part.mwpm.flooder.graph.num_nodes; ++id)
                    records[k].components[c].queued_trackers_end += part.mwpm.flooder.graph.node(id).node_event_tracker.has_queued_time;

#endif
                if constexpr (requires { decoder.component_route(c); decoder.active_predecode_threads(c); }) {
                    records[k].components[c].route = static_cast<int>(decoder.component_route(c));
                    records[k].components[c].active_threads = decoder.active_predecode_threads(c);
                }
                if constexpr (requires { decoder.partition_timing(c, size_t{0}); }) {
                    if (records[k].components[c].route == 2)
                        for (size_t b = 0; b < std::min(size_t{3}, o.partitions + 1); ++b)
                            records[k].buckets[c][b] = snapshot_seed_timers(decoder.partition_timing(c, b));
                }
            }
            if (explicit_lifecycle) {
                records[k].reset_start_ns = clock_ns(Clock::now());
                lifecycle_reset(decoder);
                records[k].reset_end_ns = clock_ns(Clock::now());
#if PM_STRUCTURED_PROFILE_COUNTERS
                for (size_t c = 0; c < 2; ++c) {
                    const auto& part = metadata_decoder(decoder).components()[c];
                    auto& target = records[k].components[c];
                    target.queue_after_reset = part.mwpm.flooder.queue.profile;
                    target.live_reset = part.mwpm.flooder.queue.size();
                    if constexpr (requires { part.mwpm.active_tree_count(); }) target.active_roots_reset = part.mwpm.active_tree_count();
                    for (size_t id = 0; id < part.mwpm.flooder.graph.num_nodes; ++id)
                        target.queued_trackers_reset += part.mwpm.flooder.graph.node(id).node_event_tracker.has_queued_time;
                }
#endif

            }
        }
        outer_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - loop_start).count();
        if (getrusage(RUSAGE_SELF, &usage_end)) throw std::runtime_error("CPU snapshot end failed");
        warmup.frequency_after_measurement = read_cpu_frequencies(o);
    });
    if (explicit_lifecycle) persist_lifecycle(o, data, records);
    write_cpu(o, usage_begin, usage_end, outer_wall_ns);
    persist_warmup(o, warmup);
    persist_clock(o, calibration);
#if PM_STRUCTURED_PROFILE_COUNTERS
    std::ofstream queues(o.output / "queue_lifecycle.csv");
    queues << "shot_index,shot_execution_index,component_id,pushes,pops,valid,stale_superseded,stale_cancelled,deferred_requeues,cleared_before_reset,cleared_after_reset,queue_live_end,queue_live_after_reset,active_roots_end,active_roots_after_reset,queued_node_trackers_end,queued_node_trackers_after_reset\n";
    for (size_t k = 0; k < records.size(); ++k)
        for (size_t c = 0; c < 2; ++c) {
            const auto& v = records[k].components[c];
            const auto& q = v.queue;
            queues << data.shot_ids[k] << ',' << k << ',' << c << ',' << q.pushes << ',' << q.pops << ','
                   << q.valid << ',' << q.stale_superseded << ',' << q.stale_cancelled << ',' << q.deferred_requeues << ','
                   << q.cleared << ',';
            if (explicit_lifecycle) queues << v.queue_after_reset.cleared;
            queues << ',' << v.live_end << ',';
            if (explicit_lifecycle) queues << v.live_reset;
            queues << ',';
            if (v.active_roots_end >= 0) queues << v.active_roots_end;
            queues << ',';
            if (explicit_lifecycle && v.active_roots_reset >= 0) queues << v.active_roots_reset;
            queues << ',' << v.queued_trackers_end << ',';
            if (explicit_lifecycle) queues << v.queued_trackers_reset;
            queues << '\n';
        }
    if (!queues) throw std::runtime_error("Queue lifecycle record write failed");
#endif
    std::ofstream shots(o.output / "diagnostic_shots.csv"), components(o.output / "profile_components.csv"), workers(o.output / "profile_workers.csv");
    std::ofstream plans(o.output / "profile_plan_workers.csv");
    plans << "shot_index,shot_execution_index,component_id,lane,worker_index,build0_ns,build1_ns,interior_wait_ns,join_ns\n";
    shots << "shot_index,shot_execution_index,input_events,observable,weight,wall_ns,partition_ns,dispatch_ns,join_ns,merge_ns,total_threads,partitions,component_threshold,shot_threshold,conflict_width,warmup_shots\n";
    components << "shot_index,component_id,";
#if PM_STRUCTURED_PROFILE_COUNTERS
    components << "input_events,normalized_seeds,residual_seeds,matched_pairs,matched_boundaries,mwpm_entered,import_regions,import_roots,import_pairs,import_boundaries,import_scheduled,queue_pushes,queue_pops,queue_cleared,queue_peak_size,queue_valid,queue_stale_superseded,queue_stale_cancelled,queue_deferred_requeues,queue_live_end,";
#endif
    components << "prepare_ns,reset_ns,seed_build_ns,normalize_ns,degree_ns,sort_ns,grow_match_ns,handoff_ns,handoff_regions_ns,handoff_matches_ns,handoff_schedule_ns,initialization_ns,mwpm_ns,extract_ns,route,active_threads,shared_total_ns,shared_prepare_ns,shared_predecode_ns,shared_conflict_ns,shared_join_ns,shared_continuation_ns,";
#if PM_STRUCTURED_PROFILE_COUNTERS
    components << "shared_conflict_seeds,";
#endif
    components << "shared_partitions,shared_import_ns,shared_import_join_ns,shared_clear_ns,tail_worker,tail_ns,conflict_owner_lane,continuation_worker,prefix_worker,whole_prefix_ns,whole_seed_scatter_ns,group_captain,group_threads,group_worker,group_prefix_ns\n";
    workers << "shot_index,worker_index,service_window_ns,import_service_window_ns\n";
    for (size_t k = 0; k < records.size(); ++k) {
        const auto& r = records[k]; const auto& t = r.stats;
        if (r.value.weight != data.expected[k].weight) throw std::runtime_error("Diagnostic optimum mismatch");
        uint64_t dispatch = 0, join = 0;
        if constexpr (requires { t.dispatch_ns; }) dispatch = t.dispatch_ns;
        if constexpr (requires { t.join_ns; }) join = t.join_ns;
        else if constexpr (requires { t.join_wait_ns; }) join = t.join_wait_ns;
        shots << data.shot_ids[k] << ',' << k << ',' << data.hits[k].size() << ',' << r.value.obs_mask << ',' << r.value.weight << ',' << r.wall_ns << ',' << t.partition_ns << ',' << dispatch << ',' << join << ',' << t.merge_ns << ',' << o.cpus.size() << ',' << o.partitions << ',' << o.component_threshold << ',' << o.shot_threshold << ',' << o.conflict_width << ',' << warmup_shots << '\n';
        if constexpr (requires { t.worker_ns; })
            for (size_t i = 0; i < o.cpus.size(); ++i) {
                workers << data.shot_ids[k] << ',' << i << ',' << t.worker_ns[i] << ',';
                if constexpr (requires { t.import_worker_ns; }) workers << t.import_worker_ns[i];
                workers << '\n';
            }
        for (size_t c = 0; c < 2; ++c) {
            const auto& part = r.components[c]; const auto& p = part.seed;
#if PM_STRUCTURED_PROFILE_COUNTERS
            const auto& q = part.queue;
            if (p.normalized_seeds != 2 * p.matched_pairs + p.matched_boundaries + p.residual_seeds ||
                q.pushes != q.pops + q.cleared + part.live_end ||
                q.pops != q.valid + q.stale_superseded + q.stale_cancelled + q.deferred_requeues)
                throw std::runtime_error("Diagnostic seed/queue conservation failed");
#endif
            components << data.shot_ids[k] << ',' << c << ',';
#if PM_STRUCTURED_PROFILE_COUNTERS
            components << p.input_events << ',' << p.normalized_seeds << ',' << p.residual_seeds << ',' << p.matched_pairs << ',' << p.matched_boundaries << ',' << p.mwpm_entered << ',' << p.import_regions << ',' << p.import_roots << ',' << p.import_pairs << ',' << p.import_boundaries << ',' << p.import_scheduled << ',' << q.pushes << ',' << q.pops << ',' << q.cleared << ',' << q.peak_size << ',' << q.valid << ',' << q.stale_superseded << ',' << q.stale_cancelled << ',' << q.deferred_requeues << ',' << part.live_end << ',';
#endif
            components << p.prepare_ns << ',' << p.reset_ns << ',' << p.seed_build_ns << ',' << p.normalize_ns << ',' << p.degree_ns << ',' << p.sort_ns << ',' << p.grow_match_ns << ',' << p.handoff_ns << ',' << p.handoff_regions_ns << ',' << p.handoff_matches_ns << ',' << p.handoff_schedule_ns << ',' << p.initialization_ns << ',' << p.mwpm_ns << ',' << p.extract_ns << ',' << part.route << ',' << part.active_threads;
            if constexpr (requires { t.component[c].predecode_ns; }) {
                const auto& v = t.component[c];
                components << ',' << v.total_ns << ',' << v.prepare_ns << ',' << v.predecode_ns << ',' << v.conflict_ns << ',' << v.join_ns << ',' << v.continuation_ns;
#if PM_STRUCTURED_PROFILE_COUNTERS
                components << ',' << v.conflict_seeds;
#endif
                components << ',' << v.partitions;
            } else {
                components << ",,,,,,,";
#if PM_STRUCTURED_PROFILE_COUNTERS
                components << ',';
#endif
            }
            if constexpr (requires { t.component[c].import_ns; })
                components << ',' << t.component[c].import_ns << ',' << t.component[c].import_join_ns;
            else components << ",,";
            if constexpr (requires { t.component[c].clear_ns; })
                components << ',' << t.component[c].clear_ns;
            else components << ',';
            if constexpr (requires { t.component[c].tail_worker; t.component[c].tail_ns; }) {
                components << ',';
                if (t.component[c].tail_worker != SIZE_MAX) components << t.component[c].tail_worker;
                components << ',' << t.component[c].tail_ns;
            } else components << ",,";
            if constexpr (requires { t.component[c].conflict_owner_lane; })
                components << ',' << t.component[c].conflict_owner_lane;
            else components << ',';
            components << ',';
            if constexpr (requires { t.component[c].continuation_worker; })
                if (t.component[c].continuation_worker != SIZE_MAX) components << t.component[c].continuation_worker;
            components << ',';
            if constexpr (requires { t.component[c].prefix_worker; t.component[c].whole_prefix_ns; t.component[c].whole_seed_scatter_ns; }) {
                if (t.component[c].prefix_worker != SIZE_MAX) components << t.component[c].prefix_worker;
                components << ',' << t.component[c].whole_prefix_ns << ',' << t.component[c].whole_seed_scatter_ns;
            } else components << ",,";
            const size_t group_threads = o.execution == "shared"
                ? (o.component_parallelism == 2 && o.cpus.size() > 1 ? o.cpus.size() / 2 : o.cpus.size()) : 1;
            const size_t captain = o.execution == "shared" && o.component_parallelism == 2 &&
                o.cpus.size() > 1 && static_cast<int>(r.components[c].route) != 3 ? c * group_threads : 0;
            components << ',' << captain << ',' << group_threads;
            if constexpr (requires { t.component[c].import_plan_build_ns; }) {
                const auto& v = t.component[c];
                for (size_t lane = 0; lane < part.active_threads; ++lane)
                    plans << data.shot_ids[k] << ',' << k << ',' << c << ',' << lane << ',' << captain + lane << ','
                          << v.import_plan_build_ns[lane][0] << ',' << v.import_plan_build_ns[lane][1] << ','
                          << v.import_plan_interior_wait_ns[lane] << ',' << v.import_plan_join_ns[lane] << '\n';
            }
            components << ',';
            if constexpr (requires { t.component[c].group_worker; t.component[c].group_prefix_ns; }) {
                if (t.component[c].group_worker != SIZE_MAX) components << t.component[c].group_worker;
                components << ',' << t.component[c].group_prefix_ns;
            } else components << ',';
            components << '\n';
        }
    }
    if (!shots || !components || !workers || !plans) throw std::runtime_error("Diagnostic CSV persistence failed");
#if PM_STRUCTURED_PROFILE_TIMELINE
    write_phase_timeline(o, data, records, warmup_shots);
#endif
    std::ofstream scope(o.output / "profile_scope.json");
#if PM_STRUCTURED_PROFILE_COUNTERS
    scope << R"({"schema_version":1,"format":"duration_windows_v1","unit":"nanosecond","record_copy":"after per-shot end clock, before next shot; included in process CPU/outer wall","overlap":"component, worker and nested windows overlap; never sum as wall-clock fractions","routes":{"-1":"serial/xz baseline","0":"not_run","1":"serial","2":"partitioned","3":"whole_serial"},"actual_active_threads":"per component per shot; team size separately recorded","useful_and_spin_cpu":"not separately measured"})" << '\n';
#else
    if (o.mode == "prefix-stages") {
        scope << R"({"schema_version":1,"format":"duration_windows_time_only_v1","unit":"nanosecond","instrumentation":{"structured_profile_counters":0,"structured_profile_timers":1,"structured_profile_timeline":0,"profile":0,"nvtx":0,"predecode_diagnostics":0},"algorithm_counters":"not collected; counter columns omitted","phase_intervals":"not collected; exact interval unions cannot be reconstructed from durations","record_copy":"after per-shot decode end, before reset; included in process CPU/outer wall","timed_scope":"explicit prepare prefix and solve/extraction/result merge; reset separately recorded after duration snapshots","team_entry_exit":"excluded; one OpenMP team surrounds cold check, warmup and measured loop","overlap":"component, worker and nested durations overlap; never sum as wall-clock fractions","wait_rule":"explicit waits remain duration fields; inclusive parent windows are not useful CPU","routes":{"-1":"serial/xz baseline","0":"not_run","1":"serial","2":"partitioned","3":"whole_serial"},"actual_active_threads":"per component per shot; team size separately recorded","useful_and_spin_cpu":"not separately measured"})" << '\n';
    } else {
    scope << R"({"schema_version":1,"format":"duration_windows_time_only_v1","unit":"nanosecond","instrumentation":{"structured_profile_counters":0,"structured_profile_timers":1,"profile":0,"nvtx":0,"predecode_diagnostics":0,"structured_profile_timeline":)"
          << PM_STRUCTURED_PROFILE_TIMELINE
          << R"(},"clock_id":"std::chrono::steady_clock/CLOCK_MONOTONIC","endpoint_convention":"half_open","algorithm_counters":"not collected; counter columns omitted","conservation":"algorithm counts not collected; timeline composition checked separately","record_copy":"after per-shot end clock, before next shot; included in process CPU/outer wall","timed_scope":"complete decode and result store; excludes diagnostic record copy","team_entry_exit":"excluded; one OpenMP team surrounds warmup and measured loop","activity_rule":"union composition_phase across components and nested scopes; distinct simultaneous phases form Mixed; uncovered outer time is Other","wait_rule":"Join coverage is reported separately and excluded from additive composition","repeated_rule":"repeated detailed scopes are detail_envelope and excluded from continuous unions","overlap":"component, worker and nested windows overlap; never sum as wall-clock fractions","routes":{"-1":"serial/xz baseline","0":"not_run","1":"serial","2":"partitioned","3":"whole_serial"},"actual_active_threads":"per component per shot; team size separately recorded","useful_and_spin_cpu":"not separately measured"})" << '\n';
    }
#endif
    if (!scope) throw std::runtime_error("Diagnostic scope persistence failed");
    std::vector<pm::MatchingResult> values(records.size());
    for (size_t k = 0; k < records.size(); ++k) values[k] = records[k].value;
    return values;
}
#endif
