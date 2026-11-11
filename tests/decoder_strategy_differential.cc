// Correctness/lifecycle driver for the public construction-bound decoder API.
// This executable deliberately collects no performance measurements.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "beyond_blossom/driver/backend_user_graph.h"
#include "beyond_blossom/driver/decoder_strategy.h"
#include "stim.h"

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <typename T> T read(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    require(bool(input), "Truncated frozen input");
    return value;
}
uint64_t mix(uint64_t state, uint64_t value) {
    state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    state ^= state >> 30;
    state *= 0xbf58476d1ce4e5b9ULL;
    state ^= state >> 27;
    state *= 0x94d049bb133111ebULL;
    return state ^ (state >> 31);
}
bool equal(const beyond_blossom::MatchingResult& a, const beyond_blossom::MatchingResult& b) {
    return a.weight == b.weight && a.obs_mask == b.obs_mask;
}
struct Input {
    std::array<uint64_t, 12> header;
    std::vector<std::vector<uint64_t>> hits;
    std::vector<beyond_blossom::MatchingResult> expected;
};
Input load(const std::filesystem::path& path) {
    static_assert(std::endian::native == std::endian::little);
    std::ifstream input(path, std::ios::binary);
    Input result;
    auto& h = result.header;
    h = read<std::array<uint64_t, 12>>(input);
    require(h[0] == 0x5354454e43494c31ULL && (h[1] == 2 || h[1] == 3), "Unknown frozen input format");
    require(h[4] > 0 && h[4] <= 100000, "This regression driver requires a small saved prefix");
    result.hits.reserve(h[4]);
    result.expected.reserve(h[4]);
    uint64_t syndrome_hash = 0, result_hash = 0, events = 0;
    for (uint64_t k = 0; k < h[4]; ++k) {
        if (h[1] == 3) {
            const auto id = read<uint64_t>(input);
            require(id == k, "Regression input IDs must be consecutive");
            syndrome_hash = mix(syndrome_hash, id);
        }
        const auto count = read<uint64_t>(input);
        require(count <= h[5], "Invalid frozen detector count");
        std::vector<uint64_t> hits(count);
        syndrome_hash = mix(syndrome_hash, count);
        for (auto& event : hits) {
            event = read<uint64_t>(input);
            require(event < h[5], "Frozen detector ID out of range");
            syndrome_hash = mix(syndrome_hash, event);
        }
        require(std::is_sorted(hits.begin(), hits.end()) &&
            std::adjacent_find(hits.begin(), hits.end()) == hits.end(), "Frozen events must be sorted and unique");
        beyond_blossom::MatchingResult expected;
        expected.obs_mask = read<uint64_t>(input);
        expected.weight = read<int64_t>(input);
        result_hash = mix(mix(result_hash, expected.obs_mask), static_cast<uint64_t>(expected.weight));
        events += count;
        result.hits.push_back(std::move(hits));
        result.expected.push_back(expected);
    }
    require(events == h[6] && syndrome_hash == h[7] && result_hash == h[8], "Frozen prefix checksum differs");
    require(input.peek() == std::char_traits<char>::eof(), "Unexpected extra input records");
    return result;
}
std::vector<int> parse_cpus(const std::string& text) {
    std::vector<int> result;
    std::istringstream input(text);
    std::string item;
    while (std::getline(input, item, ',')) result.push_back(std::stoi(item));
    require(!result.empty(), "An explicit CPU map is required");
    require(std::set<int>(result.begin(), result.end()).size() == result.size(), "CPU IDs must be distinct");
    return result;
}
void metadata(std::ostream& out, const beyond_blossom::DecoderMetadata& m) {
    out << "{\"requested_threads\":" << m.requested_threads << ",\"actual_threads\":" << m.actual_threads
        << ",\"partitions\":" << m.partitions << ",\"predecode_enabled\":" << (m.predecode_enabled ? "true" : "false")
        << ",\"selected_simd\":[";
    for (size_t k = 0; k < m.selected_simd.size(); ++k) {
        if (k) out << ',';
        out << static_cast<int>(m.selected_simd[k]);
    }
    out << "],\"threads\":[";
    for (size_t k = 0; k < m.threads.size(); ++k) {
        if (k) out << ',';
        const auto& t = m.threads[k];
        out << "{\"tid\":" << t.thread_id << ",\"requested_cpu\":" << t.requested_cpu
            << ",\"buffer\":" << t.buffer << ",\"component\":" << t.component << ",\"lane\":" << t.lane
            << ",\"affinity\":[";
        for (size_t a = 0; a < t.affinity.size(); ++a) { if (a) out << ','; out << t.affinity[a]; }
        out << "]}";
    }
    out << "]}";
}
}  // namespace

int main(int argc, const char** argv) {
    try {
        std::string strategy, dataset, model, output, cpu_text;
        for (int k = 1; k < argc; k += 2) {
            require(k + 1 < argc, "Options require values");
            const std::string key = argv[k], value = argv[k + 1];
            if (key == "--strategy") strategy = value;
            else if (key == "--dataset") dataset = value;
            else if (key == "--model") model = value;
            else if (key == "--output") output = value;
            else if (key == "--cpus") cpu_text = value;
            else throw std::invalid_argument("Unknown argument " + key);
        }
        require(!strategy.empty() && !dataset.empty() && !model.empty() && !output.empty(), "Missing required argument");
        const auto cpus = parse_cpus(cpu_text);
        const auto data = load(dataset);
        std::unique_ptr<FILE, decltype(&fclose)> file(fopen(model.c_str(), "r"), &fclose);
        require(bool(file), "Cannot open frozen DEM");
        const auto dem = stim::DetectorErrorModel::from_file(file.get());
        require(dem.count_detectors() == data.header[5], "Frozen DEM detector count differs");
        beyond_blossom::DecoderOptions options;
        options.strategy = beyond_blossom::parse_decoder_strategy(strategy);
        if (options.strategy != beyond_blossom::DecoderStrategy::Unstructured)
            options.grid = beyond_blossom::static_grid_spec_from_dem(dem, data.header[2], data.header[2], true, beyond_blossom::GridSimd::avx512vl);
        if (options.strategy == beyond_blossom::DecoderStrategy::ComponentParallel || options.strategy == beyond_blossom::DecoderStrategy::Pipelined ||
            options.strategy == beyond_blossom::DecoderStrategy::ParallelPredecode) options.cpus = cpus;
        beyond_blossom::StrategyDecoder decoder(dem, options);
        require(std::bit_cast<uint64_t>(decoder.metadata().normalising_constant) == data.header[10], "Integer quantization scale differs");
        const auto batch = decoder.decode_batch(data.hits);
        require(batch.size() == data.hits.size(), "Public batch returned the wrong count");
        const auto batch_metadata = decoder.metadata();
        std::vector<beyond_blossom::MatchingResult> stream;
        stream.reserve(data.hits.size());
        decoder.run_with_team([&](beyond_blossom::StrategyDecoder& active) {
            for (const auto& hits : data.hits) stream.push_back(active.decode(hits));
            active.drain();
        });
        for (size_t k = 0; k < batch.size(); ++k) {
            require(batch[k].weight == data.expected[k].weight, "Public decoder differs from the frozen integer optimum");
            require(equal(batch[k], stream[k]), "Batch and explicit-drain stream differ");
        }
        require(decoder.decode_batch({}).empty(), "Empty public batch did not return empty");
        const auto tail = std::span<const std::vector<uint64_t>>(data.hits).last(std::min<size_t>(3, data.hits.size()));
        const auto reused = decoder.decode_batch(tail);
        for (size_t k = 0; k < reused.size(); ++k)
            require(equal(reused[k], batch[batch.size() - reused.size() + k]), "Reuse after final drain differs");
        require(batch_metadata.actual_threads == cpus.size() && batch_metadata.threads.size() == cpus.size(), "Actual public thread count differs");
        const std::set<int> expected_cpus(cpus.begin(), cpus.end());
        std::set<int> actual_cpus;
        for (const auto& thread : batch_metadata.threads) {
            require(thread.thread_id != 0 && thread.affinity.size() == 1, "Missing actual thread identity or singleton affinity");
            actual_cpus.insert(thread.affinity.front());
        }
        require(actual_cpus == expected_cpus, "Actual public CPU mapping differs");
        require(std::filesystem::create_directory(output), "Output directory already exists");
        std::ofstream rows(std::filesystem::path(output) / "shots.csv");
        rows << "shot_index,detection_events,integer_weight,observable_mask,stream_integer_weight,stream_observable_mask\n";
        for (size_t k = 0; k < batch.size(); ++k)
            rows << k << ',' << data.hits[k].size() << ',' << batch[k].weight << ',' << batch[k].obs_mask
                 << ',' << stream[k].weight << ',' << stream[k].obs_mask << '\n';
        rows.close();
        require(bool(rows), "Could not write raw results");
        std::ofstream receipt(std::filesystem::path(output) / "complete.json");
        receipt << "{\"complete\":true,\"strategy\":\"" << strategy << "\",\"shots\":" << batch.size()
                << ",\"weight_differences\":0,\"batch_stream_differences\":0,\"explicit_stream_drain_returned\":true,"
                   "\"empty_batch_returned\":true,\"post_drain_reuse_equal\":true,\"metadata\":";
        metadata(receipt, batch_metadata);
        receipt << ",\"scope\":\"Correctness, actual thread map, explicit drain and reuse; no performance measurement\"}\n";
        receipt.close();
        require(bool(receipt), "Could not write completion receipt");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
