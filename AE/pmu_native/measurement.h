// EXP-20261014-01: preallocated outer-clock acquisition, independent of internals.
struct ClockCalibration {
    uint64_t minimum_ns = UINT64_MAX, median_ns = 0, p99_ns = 0;
    long resolution_ns = 0;
};
ClockCalibration calibrate_clock() {
    std::vector<uint64_t> costs(10000);
    for (auto& cost : costs) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        cost = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    }
    std::sort(costs.begin(), costs.end());
    timespec resolution{};
    if (clock_getres(CLOCK_MONOTONIC, &resolution))
        throw std::runtime_error("clock_getres failed");
    return {costs.front(), costs[costs.size() / 2], costs[9900], resolution.tv_sec * 1000000000L + resolution.tv_nsec};
}
struct Measurement {
    struct BlockRecord { size_t offset = 0, count = 0; uint64_t elapsed_ns = 0; };
    std::vector<pm::MatchingResult> actual;
    std::vector<uint64_t> elapsed_ns;
    std::vector<BlockRecord> blocks;
    uint64_t elapsed_total_ns = 0, outer_wall_ns = 0, warmup_shots = 0, warmup_elapsed_ns = 0;
    uint64_t final_drain_ns = 0, warmup_alignment_shots = 0;
    bool drain_separately_measured = false, final_drain_called = false;
    std::vector<int64_t> frequency_before_warmup, frequency_after_warmup, frequency_after_measurement;
    rusage cpu_before{}, cpu_after{};
};
std::vector<int64_t> read_cpu_frequencies(const Options& o) {
    std::vector<int64_t> values;
    for (int cpu : o.cpus) {
        std::ifstream source("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq");
        int64_t khz = -1;
        source >> khz;
        values.push_back(khz);
    }
    return values;
}
struct NoMeasurementHook { void operator()(Measurement&) const {} void operator()() const {} };
template <typename Decode, typename Window, typename Before = NoMeasurementHook, typename Drain = NoMeasurementHook>
Measurement measure(const Options& o, const Dataset& data, Decode&& decode, Window&& window,
                    Before&& before_measured = {}, Drain&& drain = {}) {
    Measurement m;
    m.actual.resize(data.hits.size());
    if (o.mode == "latency") m.elapsed_ns.resize(data.hits.size());
    if (o.mode == "batch") m.blocks.resize((data.hits.size() + o.block - 1) / o.block);
    if (o.mode != "pmu" && o.mode != "batch" && o.mode != "latency")
        throw std::invalid_argument("Pure measurement requires batch or latency");
    std::unique_ptr<CacheReadCounters> pmu;
    if (o.mode == "pmu") pmu = std::make_unique<CacheReadCounters>();
    window([&] {
        m.frequency_before_warmup = read_cpu_frequencies(o);
        const auto warm_begin = Clock::now();
        const size_t prefix = std::min(size_t{1024}, data.hits.size());
        do {
            for (size_t k = 0; k < prefix; ++k) {
                const auto value = decode(k);
                if (value.weight != data.expected[k].weight || (o.verify == "exact" && value != data.expected[k]))
                    throw std::runtime_error("Warmup optimum/result mismatch at shot " + std::to_string(k));
            }
            m.warmup_shots += prefix;
        } while (std::chrono::duration<double>(Clock::now() - warm_begin).count() < o.warmup_seconds);
        before_measured(m);
        m.warmup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - warm_begin).count();
        m.frequency_after_warmup = read_cpu_frequencies(o);
        if (pmu) {
            pmu->start();
            for (size_t k = 0; k < data.hits.size(); ++k) m.actual[k] = decode(k);
            pmu->stop();
            m.frequency_after_measurement = read_cpu_frequencies(o);
            pmu->persist(o.output / "pmu.json", data.hits.size());
            return;
        }
        if (getrusage(RUSAGE_SELF, &m.cpu_before)) throw std::runtime_error("Could not start CPU snapshot");
        const auto outer_begin = Clock::now();
        if (o.mode == "latency") {
            for (size_t k = 0; k < data.hits.size(); ++k) {
                const auto begin = Clock::now();
                m.actual[k] = decode(k);
                const auto end = Clock::now();
                m.elapsed_ns[k] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
            }
            if constexpr (!std::is_same_v<std::decay_t<Drain>, NoMeasurementHook>) {
                const auto drain_begin = Clock::now();
                drain();
                m.final_drain_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - drain_begin).count();
                m.final_drain_called = m.drain_separately_measured = true;
            }
        } else {
            for (size_t offset = 0; offset < data.hits.size(); offset += o.block) {
                const auto end = std::min(offset + o.block, data.hits.size());
                const auto begin = Clock::now();
                for (size_t k = offset; k < end; ++k) m.actual[k] = decode(k);
                if (end == data.hits.size()) {
                    if constexpr (!std::is_same_v<std::decay_t<Drain>, NoMeasurementHook>) {
                        drain();
                        m.final_drain_called = true;
                    }
                }
                const auto stop = Clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - begin).count();
                m.blocks[offset / o.block] = {offset, end - offset, static_cast<uint64_t>(elapsed)};
                m.elapsed_total_ns += elapsed;
            }
        }
        const auto outer_end = Clock::now();
        m.outer_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(outer_end - outer_begin).count();
        if (getrusage(RUSAGE_SELF, &m.cpu_after)) throw std::runtime_error("Could not finish CPU snapshot");
        m.frequency_after_measurement = read_cpu_frequencies(o);
    });
    for (const auto value : m.elapsed_ns) m.elapsed_total_ns += value;
    return m;
}
void write_cpu(const Options& o, const rusage& a, const rusage& b, uint64_t outer_ns) {
    auto cpu_ns = [](timeval v) { return int64_t{v.tv_sec} * 1000000000 + int64_t{v.tv_usec} * 1000; };
    std::ofstream out(o.output / "process_usage.json");
    out << "{\"user_cpu_ns\":" << cpu_ns(b.ru_utime) - cpu_ns(a.ru_utime)
        << ",\"system_cpu_ns\":" << cpu_ns(b.ru_stime) - cpu_ns(a.ru_stime)
        << ",\"outer_wall_ns\":" << outer_ns
        << ",\"peak_rss_kib\":" << b.ru_maxrss
        << ",\"rss_scope\":\"Linux native process lifetime peak through measured-end snapshot; includes setup, graph, warmup and record arrays; excludes later checking and CSV; not a window-only peak\""

        << ",\"voluntary_context_switches\":" << b.ru_nvcsw - a.ru_nvcsw
        << ",\"involuntary_context_switches\":" << b.ru_nivcsw - a.ru_nivcsw
        << ",\"scope\":\"All process threads over contiguous measured loop; includes outer clocks, sample stores, diagnostic record copies when enabled and loop control; team entry/exit, warmup, construction, checking and CSV excluded. Spin CPU is not separately measured.\"}\n";
    if (!out) throw std::runtime_error("CPU record write failed");
}
void persist_warmup(const Options& o, const Measurement& m) {
    std::ofstream warmup(o.output / "warmup.json");
    warmup << "{\"requested_seconds\":" << o.warmup_seconds << ",\"actual_elapsed_ns\":" << m.warmup_elapsed_ns
           << ",\"shots\":" << m.warmup_shots
           << ",\"buffer_alignment_shots\":" << m.warmup_alignment_shots
           << ",\"scope\":\"continuous decode and output checks inside the same resident team before timing\","
           << "\"frequency_scope\":\"sysfs scaling_cur_freq snapshots outside timing; kHz; -1 unavailable; not an interval average\",\"cpus\":[";
    for (size_t i = 0; i < o.cpus.size(); ++i) {
        if (i) warmup << ',';
        warmup << "{\"cpu\":" << o.cpus[i] << ",\"before_warmup_khz\":" << m.frequency_before_warmup[i]
               << ",\"after_warmup_khz\":" << m.frequency_after_warmup[i]
               << ",\"after_measurement_khz\":" << m.frequency_after_measurement[i] << '}';
    }
    warmup << "]}\n";
    if (!warmup) throw std::runtime_error("Warmup record write failed");
}
void persist_clock(const Options& o, const ClockCalibration& c) {
    std::ofstream clock(o.output / "clock.json");
    clock << "{\"clock_id\":\"std::chrono::steady_clock/CLOCK_MONOTONIC\",\"resolution_ns\":" << c.resolution_ns
          << ",\"adjacent_read_min_ns\":" << c.minimum_ns << ",\"adjacent_read_median_ns\":" << c.median_ns
          << ",\"adjacent_read_p99_ns\":" << c.p99_ns
          << ",\"samples\":10000,\"subtraction_applied\":false,\"method\":\"Back-to-back reads before decoder warmup; pure batch control measures whole-protocol disturbance.\"}\n";
    if (!clock) throw std::runtime_error("Clock record write failed");
}
void persist_measurement(const Options& o, const Dataset& data, const Measurement& m, const ClockCalibration& c) {
    persist_warmup(o, m);
    if (o.mode == "pmu") {
        std::ofstream raw(o.output / "native_shots.csv");
        raw << "shot_index,input_events,elapsed_ns,observable_mask,integer_weight,status,shot_execution_index\n";
        for (size_t k = 0; k < data.hits.size(); ++k)
            raw << data.shot_ids[k] << ',' << data.hits[k].size() << ",," << m.actual[k].obs_mask
                << ',' << m.actual[k].weight << ",measured," << k << '\n';
        if (!raw) throw std::runtime_error("PMU result persistence failed");
        return;
    }
    std::ofstream raw(o.output / "native_shots.csv");
    raw << "shot_index,input_events,elapsed_ns,observable_mask,integer_weight,status,shot_execution_index,replay_iteration,sample_role\n";
    uint64_t checksum = 0;
    size_t obs_diff = 0, weight_diff = 0;
    for (size_t k = 0; k < data.hits.size(); ++k) {
        raw << data.shot_ids[k] << ',' << data.hits[k].size() << ',';
        if (!m.elapsed_ns.empty()) raw << m.elapsed_ns[k];
        const auto& value = m.actual[k];
        raw << ',' << value.obs_mask << ',' << value.weight << ",measured," << k << ',';
        if (!data.replay_iteration.empty()) raw << data.replay_iteration[k];
        raw << ',' << (data.replay_target.empty() || data.replay_target[k] ? "target" : "context") << '\n';
        obs_diff += value.obs_mask != data.expected[k].obs_mask;
        weight_diff += value.weight != data.expected[k].weight;
        checksum = mix(mix(checksum, value.obs_mask), static_cast<uint64_t>(value.weight));
    }
    std::ofstream out(o.output / "measurement.csv");
    out << "mode,shots,elapsed_ns,ns_per_shot,outer_wall_ns,warmup_shots,block_size,syndrome_hash,result_hash,reference_result_hash,observable_diff,weight_diff,total_threads,execution\n"
        << std::setprecision(17) << o.mode << ',' << data.hits.size() << ',' << m.elapsed_total_ns << ','
        << double(m.elapsed_total_ns) / data.hits.size() << ',' << m.outer_wall_ns << ',' << m.warmup_shots << ','
        << o.block << ',' << data.syndrome_hash << ',' << checksum << ',' << data.result_hash << ',' << obs_diff
        << ',' << weight_diff << ',' << o.cpus.size() << ',' << o.execution << '\n';
    persist_clock(o, c);
    if (!raw || !out) throw std::runtime_error("Measurement record write failed");
    if (!m.blocks.empty()) {
        std::ofstream blocks(o.output / "native_blocks.csv");
        blocks << "first_shot_index,count,elapsed_ns,first_shot_execution_index\n";
        for (const auto& b : m.blocks)
            blocks << data.shot_ids.at(b.offset) << ',' << b.count << ',' << b.elapsed_ns << ',' << b.offset << '\n';
        if (!blocks) throw std::runtime_error("Batch block record write failed");
    }
    std::ofstream drain(o.output / "completion.json");
    drain << "{\"final_drain_called\":" << (m.final_drain_called ? "true" : "false")
          << ",\"final_drain_ns\":";
    if (m.drain_separately_measured) drain << m.final_drain_ns; else drain << "null";
    drain << ",\"last_shot_result_return_ns\":";
    if (!m.elapsed_ns.empty()) drain << m.elapsed_ns.back(); else drain << "null";
    drain << ",\"sample_elapsed_total_ns\":" << m.elapsed_total_ns
          << ",\"complete_elapsed_ns\":" << m.elapsed_total_ns + m.final_drain_ns
          << ",\"complete_outer_wall_ns\":" << m.outer_wall_ns
          << ",\"scope\":\"latency final drain is separate; batch final drain inside last block; complete outer wall includes clocks, stores and loop control\"}\n";
    if (!drain) throw std::runtime_error("Completion record write failed");
    write_cpu(o, m.cpu_before, m.cpu_after, m.outer_wall_ns);
    std::ofstream scope(o.output / "measurement_scope.json");
    const bool prepared = o.mode == "full-latency" || o.mode == "full-batch" || o.mode == "prefix-batch";
    const bool pipeline = o.execution == "pipeline";
    const bool shot_clock = o.mode == "latency" || o.mode == "full-latency" || o.mode == "pipe-latency";
    scope << R"({"schema_version":1,"unit":"nanosecond","clock_id":"std::chrono::steady_clock/CLOCK_MONOTONIC","mode":")"
          << o.mode << R"(","decode_entry":")" << (pipeline ? "pipeline" : prepared ? "prepared" : "normal")
          << R"(","timed_scope":")"
          << (pipeline ? "caller submission through synchronous decode return and result store; ready, dispatch, reuse and result waits included; final batch block drains both buffers"
                       : prepared ? "prepare_for_mwpm, solve_prepared, reset_after_shot, return and result store"
                       : "normal decode call, return and result store; existing decoder cleanup remains in its normal API cycle")
          << R"(","clock_pairs_per_shot":)" << (shot_clock ? 1 : 0)
          << R"(,"clocks_around_blocks":)" << (shot_clock ? "false" : "true")
          << R"(,"record_arrays":"preallocated before team entry and warmup","per_shot_elapsed_store":"after end clock; included in process CPU and outer wall","team":"one resident team over cold check, warmup and measured inputs; entry and exit excluded","checking_and_csv":"after team exit; excluded","internal_instrumentation":false,"explicit_reset_in_timed_scope":)"
          << (prepared ? "true" : "false")
          << R"(,"final_drain":")" << (pipeline ? (shot_clock ? "separate completion.json field; included in complete outer total" : "inside final native block") : "normal or prepared lifecycle as declared") << "\"}\n";
    if (!scope) throw std::runtime_error("Measurement scope write failed");
}
