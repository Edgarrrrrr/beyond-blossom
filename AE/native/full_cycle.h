// One full prepared cycle; no stage clocks, counters or snapshots in this call.
#ifndef STENCIL_GENERIC_ONLY
template <typename Decoder>
beyond_blossom::MatchingResult full_cycle(Decoder& decoder, const std::vector<uint64_t>& hits) {
    lifecycle_prepare(decoder, hits);
    const auto value = lifecycle_solve(decoder);
    lifecycle_reset(decoder);
    return value;
}
template <typename Decoder>
std::vector<beyond_blossom::MatchingResult> measure_full_cycle(const Options& o, const Dataset& data, Decoder& decoder,
                                                const ClockCalibration& calibration) {
    // The ordinary helper allocates result/latency arrays before team entry.
    // Both modes use exactly the same prepare, solve, reset callback.
    Options outer = o;
    outer.mode = o.mode == "full-latency" ? "latency" : "batch";
    auto m = measure(outer, data, [&](size_t k) {
        return full_cycle(decoder, data.hits[k]);
    }, [&](auto&& body) {
        decode_window(o, decoder, [&] {
            verify_prepared_state(o, data, decoder);
            body();
        });
    });
    persist_measurement(o, data, m, calibration);
    return std::move(m.actual);
}
#endif
