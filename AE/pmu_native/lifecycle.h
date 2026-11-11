// Fixed-case endpoints. Reset follows solve and is outside both decode clocks.
#ifndef STENCIL_GENERIC_ONLY
#include "prepared_snapshot_v2.h"
inline uint64_t clock_ns(Clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
}
template <typename Decoder>
void lifecycle_prepare(Decoder& decoder, const std::vector<uint64_t>& hits) {
    if constexpr (requires { decoder.prepare_for_mwpm(hits); }) decoder.prepare_for_mwpm(hits);
    else throw std::runtime_error("Decoder lacks prepared-state API");
}
template <typename Decoder>
pm::MatchingResult lifecycle_solve(Decoder& decoder) {
    if constexpr (requires { decoder.solve_prepared(); }) return decoder.solve_prepared();
    else throw std::runtime_error("Decoder lacks prepared-state API");
}
template <typename Decoder>
void lifecycle_reset(Decoder& decoder) {
    if constexpr (requires { decoder.reset_after_shot(); }) decoder.reset_after_shot();
    else throw std::runtime_error("Decoder lacks prepared-state API");
}
inline const char* lifecycle_sample_scope(const Options& o) {
    return o.replay_kind == "none"
        ? "original inputs in declared execution order; each selected record executes once"
        : "replayed inputs; original shot IDs preserved; repetitions are not independent inputs";
}
template <typename Records>
void persist_lifecycle(const Options& o, const Dataset& data, const Records& records) {
    std::ofstream out(o.output / "lifecycle_shots.csv");
    out << "shot_index,shot_execution_index,replay_iteration,sample_role,input_events,start_ns,prefix_end_ns,decode_end_ns,reset_start_ns,reset_end_ns,prefix_wall_ns,solve_wall_ns,decode_wall_ns,reset_wall_ns,observable_mask,integer_weight\n";
    for (size_t k = 0; k < records.size(); ++k) {
        const auto& r = records[k];
        if (!(r.start_ns <= r.prefix_end_ns && r.prefix_end_ns <= r.end_ns &&
              r.end_ns <= r.reset_start_ns && r.reset_start_ns <= r.reset_end_ns))
            throw std::runtime_error("Lifecycle timestamps are not ordered");
        out << data.shot_ids[k] << ',' << k << ',';
        if (!data.replay_iteration.empty()) out << data.replay_iteration[k];
        out << ',' << (data.replay_target.empty() || data.replay_target[k] ? "target" : "context")
            << ',' << data.hits[k].size() << ',' << r.start_ns << ',' << r.prefix_end_ns << ','
            << r.end_ns << ',' << r.reset_start_ns << ',' << r.reset_end_ns << ','
            << r.prefix_end_ns - r.start_ns << ',' << r.end_ns - r.prefix_end_ns << ','
            << r.end_ns - r.start_ns << ',' << r.reset_end_ns - r.reset_start_ns << ','
            << r.value.obs_mask << ',' << r.value.weight << '\n';
    }
    if (!out) throw std::runtime_error("Lifecycle record persistence failed");
    std::ofstream scope(o.output / "lifecycle_scope.json");
    scope << R"({"schema_version":1,"unit":"nanosecond","prefix":"global syndrome entry through both component predecode states and initial queues ready, before any formal MWPM iteration","decode":"prepare prefix plus solve/extraction/result merge; reset excluded","reset":"post-shot seed/cell cleanup; copied diagnostic records excluded","snapshots":"after decode end, before reset begin; no endpoint introspection in timed inputs","cpu":"continuous all-thread process window includes decode, reset, clocks, record copies and loop control","team":"one resident team over untimed state check, warmup and measured inputs; entry and exit excluded","replay_kind":")"
          << o.replay_kind << R"(","samples":")" << lifecycle_sample_scope(o) << R"("})" << '\n';
    if (!scope) throw std::runtime_error("Lifecycle scope persistence failed");
}
// Optional native scratch addresses are inspected once, outside timed replays.
template <typename Predecoder>
void persist_scratch_layout(const Options& o, uint64_t shot, size_t component, const Predecoder& pre) {
    if constexpr (requires { pre.scratch_layout(); }) {
        std::ofstream out(o.output / "scratch_layout.csv", component == 0 ? std::ios::out : std::ios::app);
        if (component == 0)
            out << "shot_index,component_id,bucket_index,seed_begin,seed_end,seed_count,mask_address,degree_address,order_address,mask_bytes,degree_bytes,order_bytes,mask_mod64,degree_mod64,order_mod64\n";
        size_t bucket = 0;
        for (const auto& layout : pre.scratch_layout()) {
            if (layout.seed_end < layout.seed_begin) throw std::runtime_error("Invalid scratch seed range");
            const auto count = layout.seed_end - layout.seed_begin;
            out << shot << ',' << component << ',' << bucket++ << ',' << layout.seed_begin << ',' << layout.seed_end
                << ',' << count << ',' << layout.mask_address << ',' << layout.degree_address << ',' << layout.order_address
                << ',' << count * sizeof(uint16_t) << ',' << count * sizeof(uint8_t) << ',' << count * sizeof(uint32_t)
                << ',' << layout.mask_address % 64 << ',' << layout.degree_address % 64 << ',' << layout.order_address % 64 << '\n';
        }
        if (!out) throw std::runtime_error("Scratch layout persistence failed");
    }
}
// The state is inspected once before warmup, separate from every timed replay.
template <typename Decoder>
void verify_prepared_state(const Options& o, const Dataset& data, Decoder& decoder) {
    // Constructor-bound capacity is cold configuration metadata in both builds.
    std::ofstream binding(o.output / "import_plan_binding.csv");
    binding << "component_id,compiled_control,capacity_records\n";
    for (size_t c = 0; c < 2; ++c) {
        size_t capacity = 0;
        if constexpr (requires { decoder.import_plan_capacity(c); })
            capacity = decoder.import_plan_capacity(c);
        binding << c << ',' << PM_SHARED_IMPORT_PLAN << ',' << capacity << '\n';
    }
    if (!binding) throw std::runtime_error("Import-plan binding persistence failed");
    lifecycle_prepare(decoder, data.hits.front());
    auto& metadata = metadata_decoder(decoder);
    persist_prepared_snapshot_v2(o, data.shot_ids.front(), metadata, decoder);
    const auto result = lifecycle_solve(decoder);
    lifecycle_reset(decoder);
    if (result != data.expected.front()) throw std::runtime_error("Untimed lifecycle exact result check failed");
}
template <typename Decoder>
std::vector<pm::MatchingResult> measure_lifecycle_batch(const Options& o, const Dataset& data, Decoder& decoder,
                                                     const ClockCalibration& calibration) {
    // Reuse the ordinary block clocks with the exact split-lifecycle callback.
    // The measurement helper selects batch outside its per-shot loop.
    Options batch = o;
    batch.mode = "batch";
    auto m = measure(batch, data, [&](size_t k) {
        lifecycle_prepare(decoder, data.hits[k]);
        const auto value = lifecycle_solve(decoder);
        lifecycle_reset(decoder);
        return value;
    }, [&](auto&& body) {
        decode_window(o, decoder, [&] {
            verify_prepared_state(o, data, decoder);
            body();
        });
    });
    persist_measurement(o, data, m, calibration);
    std::ofstream scope(o.output / "lifecycle_scope.json");
    scope << R"({"schema_version":1,"unit":"nanosecond","mode":"prefix-batch","timed_scope":"prepare_for_mwpm, solve_prepared, reset_after_shot and result store for every input; clocks only around blocks","per_shot_clocks":false,"reset_in_batch":true,"block_size":)"
          << o.block
          << R"(,"cpu":"continuous all-thread process window includes full lifecycle, result stores, block clocks and loop control","team":"one resident team over untimed state check, warmup and measured inputs; entry and exit excluded","replay_kind":")"
          << o.replay_kind << R"(","samples":")" << lifecycle_sample_scope(o) << R"("})" << '\n';
    if (!scope) throw std::runtime_error("Batch lifecycle scope persistence failed");
    return std::move(m.actual);
}
template <typename Decoder>
std::vector<pm::MatchingResult> measure_lifecycle(const Options& o, const Dataset& data, Decoder& decoder,
                                               const ClockCalibration& calibration) {
    struct Record {
        pm::MatchingResult value;
        uint64_t start_ns = 0, prefix_end_ns = 0, end_ns = 0, reset_start_ns = 0, reset_end_ns = 0;
    };
    std::vector<Record> records(data.hits.size());
    Measurement m;
    decode_window(o, decoder, [&] {
        verify_prepared_state(o, data, decoder);
        m.frequency_before_warmup = read_cpu_frequencies(o);
        const auto warm_start = Clock::now();
        const auto count = std::min(size_t{1024}, data.hits.size());
        do {
            for (size_t k = 0; k < count; ++k) {
                lifecycle_prepare(decoder, data.hits[k]);
                const auto value = lifecycle_solve(decoder);
                lifecycle_reset(decoder);
                if (value != data.expected[k]) throw std::runtime_error("Lifecycle warmup exact result mismatch");
            }
            m.warmup_shots += count;
        } while (std::chrono::duration<double>(Clock::now() - warm_start).count() < o.warmup_seconds);
        m.warmup_elapsed_ns = clock_ns(Clock::now()) - clock_ns(warm_start);
        m.frequency_after_warmup = read_cpu_frequencies(o);
        if (getrusage(RUSAGE_SELF, &m.cpu_before)) throw std::runtime_error("CPU start failed");
        const auto loop_start = Clock::now();
        for (size_t k = 0; k < data.hits.size(); ++k) {
            const auto start = Clock::now();
            lifecycle_prepare(decoder, data.hits[k]);
            const auto prefix_end = Clock::now();
            records[k].value = lifecycle_solve(decoder);
            const auto decode_end = Clock::now();
            const auto reset_begin = Clock::now();
            lifecycle_reset(decoder);
            const auto reset_end = Clock::now();
            auto& r = records[k];
            r.start_ns = clock_ns(start); r.prefix_end_ns = clock_ns(prefix_end); r.end_ns = clock_ns(decode_end);
            r.reset_start_ns = clock_ns(reset_begin); r.reset_end_ns = clock_ns(reset_end);
        }
        m.outer_wall_ns = clock_ns(Clock::now()) - clock_ns(loop_start);
        if (getrusage(RUSAGE_SELF, &m.cpu_after)) throw std::runtime_error("CPU end failed");
        m.frequency_after_measurement = read_cpu_frequencies(o);
    });
    persist_lifecycle(o, data, records); persist_warmup(o, m); persist_clock(o, calibration);
    write_cpu(o, m.cpu_before, m.cpu_after, m.outer_wall_ns);
    std::vector<pm::MatchingResult> values(records.size());
    for (size_t k = 0; k < records.size(); ++k) values[k] = records[k].value;
    return values;
}
#endif
