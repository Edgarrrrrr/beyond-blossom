// Separate untimed AE replay. All collection code is absent from timing builds.
#if BB_TAILRESET_SYNDROME_STATS
template <typename Decoder>
std::vector<beyond_blossom::MatchingResult> collect_tailreset_syndromes(const Options& o, const Dataset& data, Decoder& decoder) {
    using Counts = std::array<beyond_blossom::structured::TailResetSyndromeCounts, 2>;
    std::vector<Counts> records(data.hits.size());
    Measurement m;
    m.actual.resize(data.hits.size());
    decode_window(o, decoder, [&] {
        m.frequency_before_warmup = read_cpu_frequencies(o);
        const auto begin = Clock::now();
        const auto prefix = std::min(size_t{1024}, data.hits.size());
        do {
            for (size_t k = 0; k < prefix; ++k) {
                const auto value = decoder.decode(data.hits[k]);
                if (value.weight != data.expected[k].weight)
                    throw std::runtime_error("Statistics warmup optimum mismatch");
            }
            m.warmup_shots += prefix;
        } while (std::chrono::duration<double>(Clock::now() - begin).count() < o.warmup_seconds);
        decoder.drain();
        m.warmup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        m.frequency_after_warmup = read_cpu_frequencies(o);
        for (size_t k = 0; k < data.hits.size(); ++k) {
            m.actual[k] = decoder.decode(data.hits[k]);
            records[k] = decoder.syndrome_statistics();
        }
        decoder.drain();
        m.frequency_after_measurement = read_cpu_frequencies(o);
    });
    persist_warmup(o, m);
    std::ofstream csv(o.output / "syndromes.csv");
    csv << "shot_index,shot_execution_index,input_events,integer_weight,observable_mask";
    for (size_t c = 0; c < 2; ++c)
        csv << ",c" << c << "_normalized,c" << c << "_residual,c" << c << "_pairs,c" << c << "_boundaries";
    csv << '\n';
    std::array<beyond_blossom::structured::TailResetSyndromeCounts, 2> totals{};
    uint64_t completed = 0;
    for (size_t k = 0; k < data.hits.size(); ++k) {
        csv << data.shot_ids[k] << ',' << k << ',' << data.hits[k].size() << ','
            << m.actual[k].weight << ',' << m.actual[k].obs_mask;
        size_t remaining = 0;
        for (size_t c = 0; c < 2; ++c) {
            const auto& r = records[k][c];
            csv << ',' << r.normalized << ',' << r.residual << ',' << r.pairs << ',' << r.boundaries;
            totals[c].normalized += r.normalized; totals[c].residual += r.residual;
            totals[c].pairs += r.pairs; totals[c].boundaries += r.boundaries;
            remaining += r.residual;
        }
        completed += remaining == 0;
        csv << '\n';
    }
    if (!csv) throw std::runtime_error("Syndrome persistence failed");
    std::ofstream meta(o.output / "syndromes.json");
    meta << "{\"complete\":true,\"timed\":false,\"mode\":" << BB_TAILRESET_PREDECODE_MODE
         << ",\"shots\":" << data.hits.size() << ",\"completed_shots\":" << completed
         << ",\"scope\":\"actual double-buffer TailReset after joined prepare_for_mwpm, before solve_prepared; full decode verifies results; X/Z summed; statistics excluded from time results\",\"components\":[";
    for (size_t c = 0; c < 2; ++c) {
        const auto& r = totals[c];
        if (c) meta << ',';
        meta << "{\"normalized\":" << r.normalized << ",\"residual\":" << r.residual
             << ",\"pairs\":" << r.pairs << ",\"boundaries\":" << r.boundaries << '}';
    }
    meta << "]}\n";
    if (!meta) throw std::runtime_error("Syndrome metadata persistence failed");
    return std::move(m.actual);
}
#endif
