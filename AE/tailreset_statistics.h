// Separate untimed AE replay. All collection code is absent from timing builds.
#if BB_TAILRESET_SYNDROME_STATS
using PredecodeSyndromeCounts = std::array<beyond_blossom::structured::TailResetSyndromeCounts, 2>;

template <typename Decoder>
beyond_blossom::MatchingResult decode_with_syndrome_counts(
    Decoder& decoder, const std::vector<uint64_t>& events, PredecodeSyndromeCounts& counts) {
    if constexpr (requires { decoder.syndrome_statistics(); }) {
        const auto result = decoder.decode(events);
        counts = decoder.syndrome_statistics();
        return result;
    } else {
        static_assert(std::is_same_v<std::decay_t<Decoder>, beyond_blossom::structured::ComponentDecoder>);
        decoder.prepare_for_mwpm(events);
        try {
            counts = {};
            for (size_t c = 0; c < decoder.components().size(); ++c) {
                const auto& view = decoder.components()[c].mwpm.predecoder.seed_view;
                auto& count = counts.at(c);
                count.normalized = view.active_count();
                uint64_t paired_endpoints = 0;
                for (const auto& range : view.active_ranges())
                    for (const auto& seed : view.storage.subspan(range.begin, range.count())) {
                        if (seed.mate == beyond_blossom::structured::PredecodeSeed::UNMATCHED) ++count.residual;
                        else if (seed.mate == beyond_blossom::structured::PredecodeSeed::BOUNDARY) ++count.boundaries;
                        else ++paired_endpoints;
                    }
                if (paired_endpoints % 2) throw std::runtime_error("Odd predecode paired endpoint count");
                count.pairs = paired_endpoints / 2;
                if (count.normalized != count.residual + 2 * count.pairs + count.boundaries)
                    throw std::runtime_error("Predecode syndrome count identity failed");
#if BB_TAILRESET_PREDECODE_MODE == 1
                if (count.residual != count.normalized || count.pairs || count.boundaries)
                    throw std::runtime_error("Skip policy unexpectedly matched a seed");
#endif
            }
            const auto result = decoder.solve_prepared();
            decoder.reset_after_shot();
            return result;
        } catch (...) {
            decoder.reset();
            throw;
        }
    }
}

template <typename Decoder>
std::vector<beyond_blossom::MatchingResult> collect_tailreset_syndromes(const Options& o, const Dataset& data, Decoder& decoder) {
    using Counts = PredecodeSyndromeCounts;
    std::vector<Counts> records(data.hits.size());
    Measurement m;
    m.actual.resize(data.hits.size());
    decode_window(o, decoder, [&] {
        m.frequency_before_warmup = read_cpu_frequencies(o);
        const auto begin = Clock::now();
        const auto prefix = std::min(size_t{1024}, data.hits.size());
        do {
            for (size_t k = 0; k < prefix; ++k) {
                Counts unused;
                const auto value = decode_with_syndrome_counts(decoder, data.hits[k], unused);
                if (value.weight != data.expected[k].weight)
                    throw std::runtime_error("Statistics warmup optimum mismatch");
            }
            m.warmup_shots += prefix;
        } while (std::chrono::duration<double>(Clock::now() - begin).count() < o.warmup_seconds);
        if constexpr (requires { decoder.drain(); }) decoder.drain();
        m.warmup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        m.frequency_after_warmup = read_cpu_frequencies(o);
        for (size_t k = 0; k < data.hits.size(); ++k) {
            m.actual[k] = decode_with_syndrome_counts(decoder, data.hits[k], records[k]);
        }
        if constexpr (requires { decoder.drain(); }) decoder.drain();
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
         << ",\"execution\":\"" << o.execution << "\",\"scope\":\""
         << (o.execution == "serial"
             ? "serial ComponentDecoder after prepare_for_mwpm, before solve_prepared; explicit reset_after_shot; full decode verifies results; X/Z summed; statistics excluded from time results"
             : "actual double-buffer TailReset after joined prepare_for_mwpm, before solve_prepared; full decode verifies results; X/Z summed; statistics excluded from time results")
         << "\",\"components\":[";
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
