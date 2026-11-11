// Reproduce with the exact source snapshot or the candidate, compiled without LTO.
// Usage: benchmark dem|graph graph_path saved_shots_path baseline|on|off repeats workload_label
// DEM shots use b8. Graph shots use one "count detector..." record per line.
// Graph format: "num_nodes num_observables num_edges", then "u v signed_weight obs_mask".
// v=-1 denotes a boundary. All graph edge weights must be even, as in UserGraph.
#ifdef BEYOND_BENCHMARK_BASELINE
#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/driver/user_graph.h"
namespace decoder = pm;
#else
#include "beyond_blossom/driver/mwpm_decoding.h"
#include "beyond_blossom/driver/user_graph.h"
namespace decoder = beyond_blossom;
#endif
#include <chrono>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

int run_benchmark(int argc, char** argv) {
    if (argc < 7 || (argc - 7) % 2) {
        std::cerr << "Usage: benchmark dem|graph graph_path saved_shots_path baseline|on|off repeats workload_label "
                     "[--write-weights path] [--expected-weights path]\n";
        return 2;
    }
    std::string write_weights, expected_weights;
    for (int index = 7; index < argc; index += 2) {
        const std::string flag = argv[index];
        if (flag == "--write-weights") write_weights = argv[index + 1];
        else if (flag == "--expected-weights") expected_weights = argv[index + 1];
        else throw std::runtime_error("Unknown weight-validation option");
    }
    const std::string kind = argv[1], variant = argv[4];
    // argv[6] is an explicit workload label for machine-readable output.
    const size_t repeats = std::stoul(argv[5]);
    std::vector<std::vector<uint64_t>> shots;
    decoder::Mwpm mwpm;
    if (kind == "dem") {
        auto* file = std::fopen(argv[2], "r");
        if (!file) throw std::runtime_error("Cannot open DEM");
        auto dem = stim::DetectorErrorModel::from_file(file);
        std::fclose(file);
        mwpm = decoder::detector_error_model_to_mwpm(dem, decoder::NUM_DISTINCT_WEIGHTS);
        auto* shot_file = std::fopen(argv[3], "rb");
        if (!shot_file) throw std::runtime_error("Cannot open saved shots");
        auto reader = stim::MeasureRecordReader<stim::MAX_BITWORD_WIDTH>::make(
            shot_file, stim::SampleFormat::SAMPLE_FORMAT_B8, 0, dem.count_detectors(), dem.count_observables());
        stim::SparseShot shot;
        while (reader->start_and_read_entire_record(shot)) {
            shots.push_back(shot.hits);
            shot.clear();
        }
        reader.reset();
        std::fclose(shot_file);
    } else if (kind == "graph") {
        std::ifstream graph_file(argv[2]);
        size_t nodes, observables, count;
        if (!(graph_file >> nodes >> observables >> count)) throw std::runtime_error("Invalid graph header");
        decoder::MatchingGraph graph(nodes, observables);
        for (size_t index = 0; index < count; ++index) {
            size_t u;
            int64_t v;
            decoder::signed_weight_int weight;
            uint64_t mask;
            if (!(graph_file >> u >> v >> weight >> mask)) throw std::runtime_error("Invalid graph edge");
            std::vector<size_t> obs;
            for (size_t bit = 0; bit < observables; ++bit)
                if (mask & (uint64_t{1} << bit)) obs.push_back(bit);
            if (v < 0) graph.add_boundary_edge(u, weight, obs);
            else graph.add_edge(u, size_t(v), weight, obs);
        }
        mwpm = decoder::Mwpm(decoder::GraphFlooder(std::move(graph)));
        mwpm.flooder.sync_negative_weight_observables_and_detection_events();
        std::ifstream shots_file(argv[3]);
        size_t count_events;
        while (shots_file >> count_events) {
            auto& shot = shots.emplace_back(count_events);
            for (auto& event : shot)
                if (!(shots_file >> event)) throw std::runtime_error("Truncated saved shot");
        }
    } else throw std::runtime_error("Unknown input kind");
    if (shots.empty()) throw std::runtime_error("Empty shot corpus");
#ifndef BEYOND_BENCHMARK_BASELINE
    if (variant != "on" && variant != "off") throw std::runtime_error("Candidate variant must be on/off");
    mwpm.predecode_enabled = variant == "on";
#else
    if (variant != "baseline") throw std::runtime_error("Source snapshot variant must be baseline");
#endif
    // Warm every shot, then time decoding only. File parsing, graph construction,
    // and initial allocation never enter the steady-state timing interval.
    int64_t validation_weight = 0;
    uint64_t validation_weight_hash = 14695981039346656037ULL;
    std::vector<decoder::total_weight_int> validation_weights;
    validation_weights.reserve(shots.size());
    size_t complete = 0, skipped = 0;
    for (const auto& shot : shots) {
        const auto weight = decoder::decode_detection_events_for_up_to_64_observables(mwpm, shot, false).weight;
        validation_weight += weight;
        validation_weights.push_back(weight);
        validation_weight_hash = (validation_weight_hash ^ uint64_t(weight)) * 1099511628211ULL;
#ifndef BEYOND_BENCHMARK_BASELINE
        complete += mwpm.predecode_enabled && mwpm.predecoder.complete;
        skipped += mwpm.predecode_enabled && mwpm.predecoder.skipped;
#endif
    }
    if (!expected_weights.empty()) {
        std::ifstream expected(expected_weights, std::ios::binary);
        if (!expected) throw std::runtime_error("Cannot open expected per-shot weights");
        for (size_t index = 0; index < validation_weights.size(); ++index) {
            uint64_t bits = 0;
            for (size_t byte = 0; byte < 8; ++byte) {
                const int value = expected.get();
                if (value == EOF) throw std::runtime_error("Truncated expected per-shot weights");
                bits |= uint64_t(value) << (8 * byte);
            }
            if (bits != uint64_t(validation_weights[index]))
                throw std::runtime_error("Exact matching weight mismatch at shot " + std::to_string(index) +
                    ": expected " + std::to_string(std::bit_cast<int64_t>(bits)) +
                    ", observed " + std::to_string(validation_weights[index]));
        }
        if (expected.get() != EOF) throw std::runtime_error("Extra entries in expected per-shot weights");
    }
    if (!write_weights.empty()) {
        std::ofstream output(write_weights, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot write per-shot weights");
        for (auto weight : validation_weights)
            for (size_t byte = 0; byte < 8; ++byte)
                output.put(char((uint64_t(weight) >> (8 * byte)) & 255));
        if (!output) throw std::runtime_error("Failed to write per-shot weights");
    }
    std::cout << "workload,variant,repeat,shots,elapsed_ns,ns_per_shot,weight_checksum,weight_sequence_hash,observable_checksum,complete_shots,skipped_shots\n";
    for (size_t repeat = 0; repeat < repeats; ++repeat) {
        int64_t weight_checksum = 0;
        uint64_t observable_checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (const auto& shot : shots) {
            const auto result = decoder::decode_detection_events_for_up_to_64_observables(mwpm, shot, false);
            weight_checksum += result.weight;
            observable_checksum += result.obs_mask;
        }
        const auto end = std::chrono::steady_clock::now();
        if (weight_checksum != validation_weight) throw std::runtime_error("Nonrepeatable result weights");
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        std::cout << argv[6] << ',' << variant << ',' << repeat << ',' << shots.size() << ',' << elapsed << ','
                  << std::setprecision(12) << double(elapsed) / shots.size() << ',' << weight_checksum << ','
                  << validation_weight_hash << ','
                  << observable_checksum << ',' << complete << ',' << skipped << '\n';
    }
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run_benchmark(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
