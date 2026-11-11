// Caller-side synchronous measurements for the independent double buffers.
#if BB_EXPERIMENT_PIPELINE && !defined(STENCIL_GENERIC_ONLY)
template <typename Decoder>
void align_pipeline_warmup(const Dataset& data, Decoder& decoder, Measurement& measurement) {
    if (decoder.next_buffer() != 0) {
        const auto value = decoder.decode(data.hits.front());
        if (value != data.expected.front()) throw std::runtime_error("Pipeline alignment result mismatch");
        ++measurement.warmup_shots;
        ++measurement.warmup_alignment_shots;
    }
    decoder.drain();
    if (decoder.next_buffer() != 0) throw std::runtime_error("Pipeline warmup parity did not align");
}

template <typename Decoder>
std::vector<beyond_blossom::MatchingResult> measure_pipeline(const Options& o, const Dataset& data, Decoder& decoder,
                                               const ClockCalibration& calibration) {
#if BB_TAILRESET_SYNDROME_STATS
    if (o.mode == "pipe-statistics") return collect_tailreset_syndromes(o, data, decoder);
#endif
    if (o.mode != "pipe-latency" && o.mode != "pipe-batch")
        throw std::invalid_argument("Pure pipeline mode required");
    Options outer = o;
    outer.mode = o.mode == "pipe-latency" ? "latency" : "batch";
    auto measurement = measure(outer, data, [&](size_t k) { return decoder.decode(data.hits[k]); },
        [&](auto&& body) { decode_window(o, decoder, body); },
        [&](Measurement& m) { align_pipeline_warmup(data, decoder, m); },
        [&] { decoder.drain(); });
    persist_measurement(o, data, measurement, calibration);
    return std::move(measurement.actual);
}

#if BB_STRUCTURED_PROFILE_TIMERS
template <typename Decoder>
std::vector<beyond_blossom::MatchingResult> profile_pipeline(const Options& o, const Dataset& data, Decoder& decoder,
                                               const ClockCalibration& calibration) {
    if (o.mode != "pipe-stages" && o.mode != "pipe-timeline") throw std::invalid_argument("Separate pipeline stages mode required");
    using Profile = std::decay_t<decltype(decoder.shot_profile())>;
    using Reset = std::decay_t<decltype(decoder.completed_reset_profile(0))>;
    struct Record { Profile profile{}; Reset reset{}; uint64_t elapsed_ns = 0, caller_start_ns = 0, caller_end_ns = 0; bool reset_seen = false; };
    std::vector<Record> records(data.hits.size());
    Measurement measurement;
    measurement.actual.resize(data.hits.size());
    uint64_t first_native_shot = 0;
    decode_window(o, decoder, [&] {
        measurement.frequency_before_warmup = read_cpu_frequencies(o);
        const auto warm_begin = Clock::now();
        const size_t prefix = std::min(size_t{1024}, data.hits.size());
        do {
            for (size_t k = 0; k < prefix; ++k)
                if (decoder.decode(data.hits[k]) != data.expected[k])
                    throw std::runtime_error("Pipeline diagnostic warmup result mismatch");
            measurement.warmup_shots += prefix;
        } while (std::chrono::duration<double>(Clock::now() - warm_begin).count() < o.warmup_seconds);
        align_pipeline_warmup(data, decoder, measurement);
        measurement.warmup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - warm_begin).count();
        measurement.frequency_after_warmup = read_cpu_frequencies(o);
        auto acquire_resets = [&] {
            for (size_t buffer = 0; buffer != 2; ++buffer) {
                const auto reset = decoder.completed_reset_profile(buffer);
                if (reset.shot == SIZE_MAX || reset.shot < first_native_shot ||
                    reset.shot - first_native_shot >= records.size()) continue;
                auto& record = records[reset.shot - first_native_shot];
                if (record.profile.shot != reset.shot || record.profile.buffer != reset.buffer)
                    throw std::runtime_error("Pipeline reset snapshot belongs to a different shot");
                record.reset = reset;
                record.reset_seen = true;
            }
        };
        if (getrusage(RUSAGE_SELF, &measurement.cpu_before)) throw std::runtime_error("Could not start CPU snapshot");
        const auto outer_begin = Clock::now();
        for (size_t k = 0; k < data.hits.size(); ++k) {
            const auto begin = Clock::now();
            measurement.actual[k] = decoder.decode(data.hits[k]);
            const auto end = Clock::now();
            #if BB_STRUCTURED_PROFILE_TIMELINE
            records[k].caller_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count();
            records[k].caller_end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
#endif
            records[k].elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
            records[k].profile = decoder.shot_profile();
            if (k == 0) first_native_shot = records[k].profile.shot;
            if (records[k].profile.shot != first_native_shot + k || records[k].profile.buffer != k % 2)
                throw std::runtime_error("Pipeline diagnostic shot ordering differs");
            acquire_resets();
        }
        const auto drain_begin = Clock::now();
        decoder.drain();
        measurement.final_drain_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - drain_begin).count();
        measurement.final_drain_called = measurement.drain_separately_measured = true;
        acquire_resets();
        measurement.outer_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - outer_begin).count();
        if (getrusage(RUSAGE_SELF, &measurement.cpu_after)) throw std::runtime_error("Could not finish CPU snapshot");
        measurement.frequency_after_measurement = read_cpu_frequencies(o);
    });
    persist_warmup(o, measurement);
    persist_clock(o, calibration);
    write_cpu(o, measurement.cpu_before, measurement.cpu_after, measurement.outer_wall_ns);
    std::ofstream raw(o.output / "pipeline_shots.csv"), components(o.output / "pipeline_components.csv");
    raw << "shot_index,shot_execution_index,native_shot,buffer_id,caller_elapsed_ns,ready_wait_ns,decode_ns,prior_reset_ns,result_wait_ns,own_reset_ns,shared_dispatch_ns,shared_join_ns,shared_merge_ns,observable_mask,integer_weight\n";
    components << "shot_index,shot_execution_index,buffer_id,component_id,prepare_ns,predecode_ns,import_ns,mwpm_ns,extract_ns,shared_continuation_ns,active_threads,route\n";
    for (size_t k = 0; k < records.size(); ++k) {
        const auto& record = records[k];
        if (!record.reset_seen) throw std::runtime_error("Final drain failed to acquire every pipeline reset snapshot");
        const auto& p = record.profile;
        raw << data.shot_ids[k] << ',' << k << ',' << p.shot << ',' << p.buffer << ',' << record.elapsed_ns << ','
            << p.ready_wait_ns << ',' << p.decode_ns << ',' << p.prior_reset_ns << ',' << p.result_wait_ns << ','
            << record.reset.reset_ns << ',' << p.shared.dispatch_ns << ',' << p.shared.join_ns << ',' << p.shared.merge_ns << ','
            << measurement.actual[k].obs_mask << ',' << measurement.actual[k].weight << '\n';
        for (size_t c = 0; c < p.component_count; ++c) {
            const auto& component = p.component[c];
            const auto& shared = p.shared.component[c];
            components << data.shot_ids[k] << ',' << k << ',' << p.buffer << ',' << c << ','
                << component.prepare_ns << ',' << shared.predecode_ns << ',' << shared.import_ns << ','
                << component.mwpm_ns << ',' << component.extract_ns << ','
                << shared.continuation_ns << ',' << shared.active_threads << ',' << static_cast<int>(shared.route) << '\n';
        }
    }
#if BB_STRUCTURED_PROFILE_TIMELINE
    std::ofstream intervals(o.output / "pipeline_timeline.csv");
    intervals << "shot_index,shot_execution_index,buffer_id,component_id,lane,phase,start_ns,end_ns,scope,present,repeated\n";
    auto emit = [&](size_t k, size_t component, size_t lane, const char* phase, const auto& interval, const char* kind) {
        if (!interval.start_ns && !interval.end_ns) return;
        if (!interval.start_ns || interval.end_ns < interval.start_ns) throw std::runtime_error("Invalid native pipeline phase interval");
        intervals << data.shot_ids[k] << ',' << k << ',' << records[k].profile.buffer << ',';
        if (component != SIZE_MAX) intervals << component;
        intervals << ',';
        if (lane != SIZE_MAX) intervals << lane;
        intervals << ',' << phase << ',' << interval.start_ns << ',' << interval.end_ns << ',' << kind << ',' << interval.present << ',' << interval.repeated << '\n';
    };
    const std::array<const char*, 19> seed_phases{"prepare", "handoff", "mwpm", "reset", "seed_build", "normalize", "degree", "sort", "grow_match", "handoff_regions", "handoff_matches", "handoff_schedule", "initialization", "extract", "scratch", "cell_index", "complete", "import_reserve", "import_construct"};
    for (size_t k = 0; k < records.size(); ++k) {
        const auto& p = records[k].profile;
        beyond_blossom::structured::PhaseInterval caller;
        caller.start_ns = records[k].caller_start_ns; caller.end_ns = records[k].caller_end_ns; caller.present = true;
        emit(k, SIZE_MAX, SIZE_MAX, "caller", caller, "caller_envelope");
        emit(k, SIZE_MAX, SIZE_MAX, "ready_wait", p.ready_wait_interval, "wait");
        emit(k, SIZE_MAX, SIZE_MAX, "decode", p.decode_interval, "decode_envelope");
        emit(k, SIZE_MAX, SIZE_MAX, "prior_reset", p.prior_reset_interval, "other_buffer_reset");
        emit(k, SIZE_MAX, SIZE_MAX, "result_wait", p.result_wait_interval, "wait");
        emit(k, SIZE_MAX, SIZE_MAX, "own_reset", records[k].reset.reset_interval, "reset");
        emit(k, SIZE_MAX, SIZE_MAX, "merge", p.shared.merge_interval, "merge");
        for (size_t c = 0; c < p.component_count; ++c) {
            const auto& shared = p.shared.component[c];
            emit(k, c, SIZE_MAX, "predecode", shared.predecode_interval, "predecode_envelope");
            emit(k, c, SIZE_MAX, "import", shared.import_interval, "import_envelope");
            emit(k, c, SIZE_MAX, "continuation", shared.continuation_interval, "continuation_envelope");
            for (size_t phase = 0; phase < seed_phases.size(); ++phase)
                if (phase != 3) emit(k, c, SIZE_MAX, seed_phases[phase], p.component[c].timeline[phase], "component_phase");
            for (size_t partition = 0; partition < p.partition_count; ++partition)
                for (size_t phase = 0; phase < seed_phases.size(); ++phase)
                    if (phase != 3) emit(k, c, partition, seed_phases[phase], p.partition[c][partition].timeline[phase], "partition_detail");
        }
    }
    if (!intervals) throw std::runtime_error("Pipeline timeline write failed");
#endif
    std::ofstream scope(o.output / "diagnostic_scope.json");
    scope << R"({"schema_version":1,"format":"pipeline_duration_windows_v1","instrumentation":{"structured_profile_counters":0,"structured_profile_timers":1,"structured_profile_timeline":)" << BB_STRUCTURED_PROFILE_TIMELINE << R"(,"profile":0,"nvtx":0,"predecode_diagnostics":0},"caller_latency":"caller outer clocks around synchronous decode and result store","reset":"delayed completed reset copied by native shot identity after reuse or final drain","final_drain_ns":)"
          << measurement.final_drain_ns
          << R"(,"snapshot_copy":"after caller end clock; included in process CPU and complete outer wall","overlap":"buffer, component and nested windows overlap; duration sums are not wall fractions","team_entry_exit":"excluded; one team over warmup and measured stream"})" << '\n';
    if (!raw || !components || !scope) throw std::runtime_error("Pipeline diagnostic record write failed");
    return std::move(measurement.actual);
}
#endif
#endif
