// Fixed-storage endpoints are copied after the outer stop clock and persisted
// after the resident team exits. Nested windows keep their explicit identity.
#if !defined(STENCIL_GENERIC_ONLY) && BB_STRUCTURED_PROFILE_TIMELINE
template <typename Records>
void write_phase_timeline(const Options& o, const Dataset& data, const Records& records, uint64_t warmup_shots) {
    std::ofstream windows(o.output / "diagnostic_windows.csv"), timeline(o.output / "phase_intervals.csv");
    windows << "shot_index,shot_execution_index,start_ns,end_ns,wall_ns,warmup_shots\n";
    timeline << "shot_index,shot_execution_index,component_id,phase,classification,start_ns,end_ns,present,interval_index,worker_index,repeated,composition_phase\n";
    constexpr std::array<const char*, 19> phase_names{
        "Prepare", "Handoff", "Mwpm", "Reset", "SeedBuild", "Normalize", "Degree", "Sort",
        "GrowMatch", "HandoffRegions", "HandoffMatches", "HandoffSchedule", "Initialization", "Extract",
        "Scratch", "CellIndex", "Complete", "ImportReserve", "ImportConstruct"};
    for (size_t k = 0; k < records.size(); ++k) {
        const auto& r = records[k];
        windows << data.shot_ids[k] << ',' << k << ',' << r.start_ns << ',' << r.end_ns << ',' << r.wall_ns << ',' << warmup_shots << '\n';
        size_t interval_index = 0;
        auto emit = [&](int component, int worker, const char* phase, const char* classification,
                        const char* composition_phase, const beyond_blossom::structured::PhaseInterval& interval) {
            if ((!interval.present && (interval.start_ns || interval.end_ns || interval.repeated)) ||
                (interval.present && (interval.start_ns < r.start_ns || interval.end_ns > r.end_ns || interval.end_ns < interval.start_ns)))
                throw std::runtime_error("Invalid timeline interval at shot " + std::to_string(data.shot_ids[k]) + " phase " + phase);
            // A repeated fine-grained scope provides an envelope; its gaps are
            // unknown and must never become continuous activity coverage.
            if (interval.repeated) {
                if (std::string_view(classification) == "activity" || std::string_view(classification) == "wait")
                    throw std::runtime_error("Repeated activity/wait interval cannot form an exact union");
                classification = "detail_envelope";
                composition_phase = "";
            }
            timeline << data.shot_ids[k] << ',' << k << ',' << component << ',' << phase << ',' << classification << ','
                     << interval.start_ns << ',' << interval.end_ns << ',' << unsigned(interval.present) << ','
                     << interval_index++ << ',' << worker << ',' << unsigned(interval.repeated) << ',' << composition_phase << '\n';
        };
        const auto& t = r.stats;
        const auto& seed_build = r.seed_build;
        if (o.mode == "prefix-timeline") {
            emit(-1, -1, "Prefix", "envelope", "", t.prefix_interval);
            emit(-1, -1, "SeedCount", "detail", "", seed_build.seed_count_interval);
            emit(-1, -1, "SeedLayout", "detail", "", seed_build.seed_layout_interval);
            emit(-1, -1, "SeedScatter", "detail", "", seed_build.seed_scatter_interval);
            emit(-1, -1, "SeedBind", "detail", "", seed_build.seed_bind_interval);
        }
        emit(-1, -1, "Partition", "activity", "Partition", t.partition_interval);
        if constexpr (requires { t.total_interval; }) {
            emit(-1, -1, "Decode", "envelope", "", t.total_interval);
            emit(-1, -1, "Dispatch", "activity", "Dispatch", t.dispatch_interval);
            emit(-1, -1, "Join", "wait", "", t.join_interval);
            emit(-1, -1, "Merge", "activity", "Merge", t.merge_interval);
        } else {
            emit(-1, -1, "Decode", "envelope", "", t.decode_interval);
        }
        if constexpr (requires { t.interior_interval; }) {
            emit(-1, -1, "InteriorBatch", "envelope", "", t.interior_interval);
            emit(-1, 0, "InteriorDispatch", "activity", "Dispatch", t.interior_dispatch_interval);
            emit(-1, 0, "InteriorJoin", "wait", "", t.interior_join_interval);
            emit(-1, 0, "TailDispatch", "activity", "Dispatch", t.tail_dispatch_interval);
            emit(-1, 0, "TailJoin", "wait", "", t.tail_join_interval);
        }
        if constexpr (requires { t.continuation_dispatch_interval; t.continuation_join_interval; }) {
            emit(-1, 0, "ContinuationDispatch", "activity", "Dispatch", t.continuation_dispatch_interval);
            emit(-1, 0, "ContinuationJoin", "wait", "", t.continuation_join_interval);
        }
        if constexpr (requires { t.whole_prefix_dispatch_interval; t.whole_prefix_join_interval; }) {
            emit(-1, 0, "WholePrefixDispatch", "activity", "Dispatch", t.whole_prefix_dispatch_interval);
            emit(-1, 0, "WholePrefixJoin", "wait", "", t.whole_prefix_join_interval);
        }
        if constexpr (requires { t.group_interval; t.group_dispatch_interval; t.group_join_interval; }) {
            emit(-1, -1, "Group", "envelope", "", t.group_interval);
            emit(-1, 0, "GroupDispatch", "activity", "Dispatch", t.group_dispatch_interval);
            emit(-1, 0, "GroupJoin", "wait", "", t.group_join_interval);
        }
        for (size_t c = 0; c < 2; ++c) {
            const auto& p = r.components[c].seed;
            int tail_worker = -1;
            int continuation_worker = -1;
            int prefix_worker = -1;
            bool group_prefix_active = false;
            bool import_plan_active = false;
            if constexpr (requires { t.component[c].total_interval; }) {
                const auto& shared = t.component[c];
                const int captain = o.component_parallelism == 2 && o.cpus.size() > 1 &&
                    static_cast<int>(r.components[c].route) != 3 ? int(c * (o.cpus.size() / 2)) : 0;
                tail_worker = continuation_worker = captain;
                if constexpr (requires { shared.import_plan_build_intervals; }) {
                    for (size_t lane = 0; lane < std::min(r.components[c].active_threads, shared.import_plan_build_intervals.size()); ++lane) {
                        const auto& builds = shared.import_plan_build_intervals[lane];
                        import_plan_active |= builds[0].present || builds[1].present;
                        emit(c, captain + lane, "ImportPlanBuild0", "activity", "ImportPlan", builds[0]);
                        emit(c, captain + lane, "ImportPlanBuild1", "activity", "ImportPlan", builds[1]);
                        emit(c, captain + lane, "ImportPlanInteriorWait", "wait", "", shared.import_plan_interior_wait_intervals[lane]);
                        emit(c, captain + lane, "ImportPlanJoin", "wait", "", shared.import_plan_join_intervals[lane]);
                    }
                }
                if constexpr (requires { shared.group_worker; shared.group_prefix_interval; shared.group_lane_intervals; shared.group_seed_scatter_intervals; }) {
                    group_prefix_active = shared.group_prefix_interval.present;
                    int group_worker = captain;
                    if (shared.group_worker != SIZE_MAX) {
                        if (shared.group_worker >= o.cpus.size()) throw std::runtime_error("Invalid Group worker");
                        group_worker = static_cast<int>(shared.group_worker);
                    }
                    emit(c, group_worker, "GroupPrefix", "envelope", "", shared.group_prefix_interval);
                    for (size_t lane = 0; lane < std::min(r.components[c].active_threads, shared.group_lane_intervals.size()); ++lane) {
                        emit(c, group_worker + lane, "GroupLane", "envelope", "", shared.group_lane_intervals[lane]);
                        emit(c, group_worker + lane, "GroupSeedScatter", "activity", "SeedScatter", shared.group_seed_scatter_intervals[lane]);
                    }
                }
                if constexpr (requires { shared.prefix_worker; shared.whole_prefix_interval; shared.whole_seed_scatter_interval; }) {
                    if (shared.prefix_worker != SIZE_MAX) {
                        if (shared.prefix_worker >= o.cpus.size()) throw std::runtime_error("Invalid whole prefix worker");
                        prefix_worker = static_cast<int>(shared.prefix_worker);
                    }
                    emit(c, prefix_worker, "WholePrefix", "envelope", "", shared.whole_prefix_interval);
                    emit(c, prefix_worker, "WholeSeedScatter", "activity", "SeedScatter", shared.whole_seed_scatter_interval);
                }
                if constexpr (requires { shared.continuation_worker; }) {
                    if (shared.continuation_worker != SIZE_MAX) {
                        if (shared.continuation_worker >= o.cpus.size()) throw std::runtime_error("Invalid continuation worker");
                        continuation_worker = static_cast<int>(shared.continuation_worker);
                    }
                }
                if constexpr (requires { shared.tail_worker; shared.tail_interval; }) {
                    if (shared.tail_worker != SIZE_MAX) {
                        if (shared.tail_worker >= o.cpus.size()) throw std::runtime_error("Invalid component tail worker");
                        tail_worker = static_cast<int>(shared.tail_worker);
                    }
                    emit(c, tail_worker, "ComponentTail", "detail", "", shared.tail_interval);
                }
                if constexpr (requires { shared.owner_conflict_wait_intervals; }) {
                    for (size_t lane = 0; lane < std::min(r.components[c].active_threads, shared.owner_conflict_wait_intervals.size()); ++lane)
                        emit(c, captain + lane, "ComponentOwnerConflictWait", "wait", "", shared.owner_conflict_wait_intervals[lane]);
                }
                emit(c, -1, "Component", "envelope", "", shared.total_interval);
                emit(c, captain, "SharedPrepare", "activity", "Prepare", shared.prepare_interval);
                const bool group_envelope = o.component_parallelism == 2 && static_cast<int>(r.components[c].route) == 2;
                emit(c, captain, "SharedPredecode", group_envelope ? "envelope" : "activity",
                     group_envelope ? "" : "Prepare", shared.predecode_interval);
                emit(c, tail_worker, "SharedConflict", "detail", "", shared.conflict_interval);
                emit(c, captain, "SharedJoin", "wait", "", shared.join_interval);
                emit(c, continuation_worker, "Continuation", "envelope", "", shared.continuation_interval);
                const bool import_envelope = import_plan_active || (o.parallel_import && r.components[c].active_threads > 1);
                emit(c, tail_worker, "SharedImport", import_envelope ? "envelope" : "activity",
                     import_envelope ? "" : "Handoff", shared.import_interval);
                emit(c, captain, "SharedImportJoin", "wait", "", shared.import_join_interval);
                emit(c, captain, "SharedClear", "activity", "Clear", shared.clear_interval);
                emit(c, captain, "BucketCount", "detail", "", shared.bucket_count_interval);
                emit(c, captain, "BucketReorder", "detail", "", shared.bucket_reorder_interval);
                emit(c, captain, "ComponentDispatch", "detail", "", shared.dispatch_interval);
                emit(c, tail_worker, "PartialMerge", "detail", "", shared.partial_merge_interval);
                emit(c, captain, "ImportDispatch", "detail", "", shared.import_dispatch_interval);
                for (size_t lane = 0; lane < r.components[c].active_threads; ++lane) {
                    emit(c, captain + lane, "ComponentWorkerPredecode", "detail", "", shared.worker_intervals[lane]);
                    emit(c, captain + lane, "ComponentWorkerImport", "detail", "", shared.import_worker_intervals[lane]);
                }
                for (size_t b = 0; b < std::min(size_t{3}, o.partitions + 1); ++b) {
                    const auto& bucket = r.buckets[c][b];
                    const size_t lane = b == o.partitions ? (tail_worker < 0 ? 0 : size_t(tail_worker))
                        : size_t(captain) + b % std::max(size_t{1}, r.components[c].active_threads);
                    emit(c, lane, "BucketScratch", "detail", "", bucket.timeline[14]);
                    emit(c, lane, "BucketCellIndex", "detail", "", bucket.timeline[15]);
                    emit(c, lane, "BucketDegree", "detail", "", bucket.timeline[6]);
                    emit(c, lane, "BucketSort", "detail", "", bucket.timeline[7]);
                    emit(c, lane, "BucketGrowMatch", "detail", "", bucket.timeline[8]);
                }
            } else {
                emit(c, -1, "Component", "envelope", "", t.component_intervals[c]);
                emit(c, -1, "Merge", "activity", "Merge", t.merge_intervals[c]);
            }
            for (size_t phase = 0; phase < phase_names.size(); ++phase) {
                const bool top_level = phase <= static_cast<size_t>(beyond_blossom::structured::PredecodePhase::Mwpm) ||
                    phase == static_cast<size_t>(beyond_blossom::structured::PredecodePhase::Initialization) ||
                    phase == static_cast<size_t>(beyond_blossom::structured::PredecodePhase::Extract);
                const bool tail_phase = phase == 1 || phase == 9 || phase == 10 || phase == 11 ||
                    phase == 16 || phase == 17 || phase == 18;
                const bool continuation_phase = phase == 2 || phase == 13;
                const int captain = o.execution == "shared" && o.component_parallelism == 2 && o.cpus.size() > 1 &&
                    static_cast<int>(r.components[c].route) != 3 ? int(c * (o.cpus.size() / 2)) : 0;
                const bool coordinator_phase = (phase == 3 && (prefix_worker >= 0 || o.component_parallelism == 2)) ||
                    (phase == 14 && group_prefix_active);
                const bool parallel_handoff = o.parallel_import &&
                    r.components[c].active_threads > 1 && (phase == 1 || phase == 9);
                // Planned import can wait for the producer inside these scopes.
                // Its four sequential finalization phases provide activity;
                // producer and join are emitted on their actual actors above.
                const bool plan_handoff = import_plan_active && (phase == 1 || phase == 9);
                const bool plan_activity = import_plan_active && (phase == 10 || phase == 11 || phase == 17 || phase == 18);
                emit(c, continuation_phase ? continuation_worker : coordinator_phase ? 0 : prefix_worker >= 0 ? prefix_worker :
                     tail_phase ? tail_worker : o.execution == "shared" ? captain : -1,
                     phase_names[phase], parallel_handoff || plan_handoff ? "envelope" : top_level || plan_activity ? "activity" : "detail",
                     (top_level || plan_activity) && !parallel_handoff && !plan_handoff ? phase_names[phase] : "", p.timeline[phase]);
            }
        }
        if constexpr (requires { t.worker_intervals; }) {
            for (size_t worker = 0; worker < o.cpus.size(); ++worker) {
                const int component = o.component_parallelism == 1 || o.cpus.size() == 1
                    ? -1 : worker / (o.cpus.size() / 2);
                emit(component, worker, "WorkerPredecode", "detail", "", t.worker_intervals[worker]);
                emit(component, worker, "WorkerImport", "detail", "", t.import_worker_intervals[worker]);
            }
        }
        // Each owner scans both components in one scope. Keep this global
        // window once per physical lane instead of duplicating it by component.
        if constexpr (requires { t.owner_scatter_intervals; }) {
            for (size_t lane = 0; lane < std::min(o.cpus.size(), t.owner_scatter_intervals.size()); ++lane)
                emit(-1, lane, "OwnerSeedScatter", "detail", "", t.owner_scatter_intervals[lane]);
        }
        if constexpr (requires { t.owner_conflict_wait_intervals; }) {
            for (size_t lane = 0; lane < std::min(o.cpus.size(), t.owner_conflict_wait_intervals.size()); ++lane)
                emit(-1, lane, "OwnerConflictWait", "wait", "", t.owner_conflict_wait_intervals[lane]);
        }
    }
    if (!windows || !timeline) throw std::runtime_error("Timeline persistence failed");
}
#endif
