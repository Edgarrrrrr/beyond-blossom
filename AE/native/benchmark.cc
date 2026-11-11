// Copyright 2026 PyMatching Contributors
// SPDX-License-Identifier: Apache-2.0
// EXP-20261014-01: normal and explicit full-cycle outer-clock acquisition.
// Backend selection and all verification are outside the identical timed blocks.
#include <cerrno>
#include <cstring>
#define BB_AE_PORTABLE_PLATFORM 1
#if defined(__linux__)
#include <linux/perf_event.h>
#else
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include <sys/ioctl.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <queue>
#include <random>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <vector>

#include "beyond_blossom/driver/mwpm_decoding.h"
#include "beyond_blossom/driver/user_graph.h"
#ifndef STENCIL_GENERIC_ONLY
#include "beyond_blossom/driver/backend_user_graph.h"
#include "beyond_blossom/structured/driver/component_decoder.h"
#endif
#if BB_EXPERIMENT_OPENMP
#include "beyond_blossom/structured/driver/openmp_component_decoder.h"
#endif
#if BB_EXPERIMENT_SHARED
#include "beyond_blossom/structured/driver/shared_component_decoder.h"
#if __has_include("beyond_blossom/structured/driver/time_partition_cuts.h")
#include "beyond_blossom/structured/driver/time_partition_cuts.h"
#define BB_EXPERIMENT_TIME_CUTS 1
#else
#define BB_EXPERIMENT_TIME_CUTS 0
#endif
#endif
#if BB_UNIFIED_CONTROL
#include "beyond_blossom/driver/decoder_strategy.h"
#endif
#include "stim.h"
#ifndef BB_SHARED_COMPONENT_PARALLELISM
#define BB_SHARED_COMPONENT_PARALLELISM 2
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t MAGIC = 0x5354454e43494c31;
// Native little-endian uint64 fields: magic, version, d, p bits, shots, detectors,
// hit count, syndrome hash, result hash, mixed seed, scale bits, stream ID.
// v2 sampled records: size/hits/obs/weight. v3 imported records prefix each with original shot ID.
using Header = std::array<uint64_t, 12>;
struct Options {
    size_t distance = 9, shots = 100000, limit = 0, block = 1024, repeats = 10;
    uint64_t root_seed = 2026101401, stream_id = 0;
    double p = 0.001;
    std::string mode = "batch", backend = "generic", verify = "exact", execution = "serial";
    int main_cpu = -1, worker_cpu = -1;
    std::vector<int> cpus;
    std::string model_source = "frozen";
    size_t partitions = 2, component_threshold = 0, shot_threshold = 0, conflict_width = 3;
    size_t component_parallelism = BB_SHARED_COMPONENT_PARALLELISM;
    bool parallel_import = false;
    std::string prefix_schedule = "component_sequential", pipeline_schedule = "overlap";
    std::string conflict_geometry = "symmetric_width";
    std::string continuation_placement = "component_captain";
    bool witness_all = false;
    bool predecode = true;
    std::vector<size_t> selected_shots;
    std::string replay_kind = "none";
    size_t replay_count = 31, context_count = 64;
    std::filesystem::path parent_reference, import_hits, import_dem, import_circuit;
    double warmup_seconds = 2;
    std::filesystem::path dataset, output;
};
uint64_t parse_unsigned(const std::string& value) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
        throw std::invalid_argument("Unsigned 64-bit decimal seed or stream required");
    return std::stoull(value);
}
Options parse(int argc, char** argv) {
    Options o;
    for (int k = 1; k < argc; k += 2) {
        if (k + 1 == argc)
            throw std::invalid_argument("Missing argument value");
        std::string key = argv[k], value = argv[k + 1];
        if (key == "--predecode")
            o.predecode = std::stoi(value) != 0;
        else if (key == "--distance")
            o.distance = std::stoull(value);
        else if (key == "--shots")
            o.shots = std::stoull(value);
        else if (key == "--limit")
            o.limit = std::stoull(value);
        else if (key == "--root-seed")
            o.root_seed = parse_unsigned(value);
        else if (key == "--stream-id")
            o.stream_id = parse_unsigned(value);
        else if (key == "--error-rate")
            o.p = std::stod(value);
        else if (key == "--block-size")
            o.block = std::stoull(value);
        else if (key == "--repeats")
            o.repeats = std::stoull(value);
        else if (key == "--mode")
            o.mode = value;
        else if (key == "--backend")
            o.backend = value;
        else if (key == "--execution")
            o.execution = value;
        else if (key == "--pipeline-schedule")
            o.pipeline_schedule = value;
        else if (key == "--parallel-import")
            o.parallel_import = std::stoi(value) != 0;
        else if (key == "--prefix-schedule")
            o.prefix_schedule = value;
        else if (key == "--conflict-geometry")
            o.conflict_geometry = value;
        else if (key == "--continuation-placement")
            o.continuation_placement = value;
        else if (key == "--partitions")
            o.partitions = std::stoull(value);
        else if (key == "--component-parallelism")
            o.component_parallelism = std::stoull(value);
        else if (key == "--component-threshold")
            o.component_threshold = std::stoull(value);
        else if (key == "--shot-threshold")
            o.shot_threshold = std::stoull(value);
        else if (key == "--conflict-width")
            o.conflict_width = std::stoull(value);
        else if (key == "--cpus") {
            std::stringstream list(value);
            std::string token;
            while (std::getline(list, token, ','))
                o.cpus.push_back(std::stoi(token));
        } else if (key == "--main-cpu")
            o.main_cpu = std::stoi(value);
        else if (key == "--worker-cpu")
            o.worker_cpu = std::stoi(value);
        else if (key == "--model-source")
            o.model_source = value;
        else if (key == "--witness-all")
            o.witness_all = std::stoi(value) != 0;
        else if (key == "--shot-ids") {
            std::stringstream list(value);
            std::string token;
            while (std::getline(list, token, ',')) o.selected_shots.push_back(std::stoull(token));
        }
        else if (key == "--replay-kind")
            o.replay_kind = value;
        else if (key == "--replay-count")
            o.replay_count = std::stoull(value);
        else if (key == "--context-count")
            o.context_count = std::stoull(value);
        else if (key == "--import-hits")
            o.import_hits = value;
        else if (key == "--import-dem")
            o.import_dem = value;
        else if (key == "--import-circuit")
            o.import_circuit = value;
        else if (key == "--parent-reference")
            o.parent_reference = value;
        else if (key == "--verify")
            o.verify = value;
        else if (key == "--warmup-seconds")
            o.warmup_seconds = std::stod(value);
        else if (key == "--dataset")
            o.dataset = value;
        else if (key == "--output-dir")
            o.output = value;
        else
            throw std::invalid_argument("Unknown argument " + key);
    }
    if (!o.shots || !o.block || !o.repeats || !o.stream_id || o.dataset.empty() || o.output.empty())
        throw std::invalid_argument("Positive counts, nonzero stream ID, dataset and output required");
    if (!std::isfinite(o.p) || o.p <= 0 || o.p >= 0.5)
        throw std::invalid_argument("Invalid error rate");
    if (
#if BB_TAILRESET_SYNDROME_STATS
        o.mode != "pipe-statistics" &&
#endif
        o.mode != "pmu" && o.mode != "prepare" && o.mode != "import" && o.mode != "batch" && o.mode != "latency" &&
        o.mode != "diagnostic" && o.mode != "diag-time-only" && o.mode != "prefix" && o.mode != "prefix-timeline" &&
        o.mode != "prefix-batch" && o.mode != "prefix-stages" &&
        o.mode != "full-batch" && o.mode != "full-latency" && o.mode != "input-features" &&
        o.mode != "pipe-batch" && o.mode != "pipe-latency" && o.mode != "pipe-stages" && o.mode != "pipe-timeline" && o.mode != "queue-stages")
        throw std::invalid_argument("Unknown measurement mode");
    if (o.verify != "exact" && o.verify != "weight")
        throw std::invalid_argument("Invalid verification policy");
    if (!std::isfinite(o.warmup_seconds) || o.warmup_seconds < 2)
        throw std::invalid_argument("Invalid warmup duration");
    if (o.execution != "serial" && o.execution != "openmp2" && o.execution != "shared" && o.execution != "pipeline")
        throw std::invalid_argument("Unknown execution");
    if (o.model_source != "generate" && o.model_source != "frozen")
        throw std::invalid_argument("Unknown model source");
    if (o.cpus.empty()) {
        o.cpus.push_back(o.main_cpu);
        if (o.execution == "openmp2")
            o.cpus.push_back(o.worker_cpu);
    }
    if (o.main_cpu < 0)
        o.main_cpu = o.cpus.front();
#if defined(__linux__)
    if (o.cpus.front() != o.main_cpu || std::set<int>(o.cpus.begin(), o.cpus.end()).size() != o.cpus.size())
        throw std::invalid_argument("Unique CPU list starting with main required");
    for (int cpu : o.cpus)
        if (cpu < 0 || cpu >= CPU_SETSIZE)
            throw std::invalid_argument("CPU list outside cpu_set_t");
#else
    if (std::any_of(o.cpus.begin(), o.cpus.end(), [](int cpu) { return cpu != -1; }))
        throw std::invalid_argument("Darwin uses QoS and requires -1 CPU placeholders");
#endif
    if ((o.execution == "serial" && o.cpus.size() != 1) || (o.execution == "openmp2" && o.cpus.size() != 2))
        throw std::invalid_argument("Execution/thread count mismatch");
    if ((o.execution == "shared" || o.execution == "pipeline") && (o.partitions == 0 ||
            (o.cpus.size() != 1 && o.cpus.size() != 2 && o.cpus.size() != 4 && o.cpus.size() != 8)))
        throw std::invalid_argument("Shared decoding needs positive partitions and T1/T2/T4/T8");
    if ((o.execution == "shared" || o.execution == "pipeline") && o.component_parallelism != BB_SHARED_COMPONENT_PARALLELISM)
        throw std::invalid_argument("Requested component parallelism differs from compiled decoder");
    if (o.prefix_schedule != "component_sequential" && o.prefix_schedule != "component_owned_tail" &&
        o.prefix_schedule != "component_owned_whole" && o.prefix_schedule != "component_group_owned")
        throw std::invalid_argument("Unknown prefix schedule");
    const bool lifecycle_mode = o.mode == "prefix" || o.mode == "prefix-timeline" ||
        o.mode == "prefix-batch" || o.mode == "prefix-stages" ||
        o.mode == "full-batch" || o.mode == "full-latency" ||
        o.mode == "pipe-batch" || o.mode == "pipe-latency" || o.mode == "pipe-stages" || o.mode == "pipe-timeline" || o.mode == "queue-stages";
#if !BB_PREDECODE_ENABLED
    if (lifecycle_mode) throw std::invalid_argument("Prepared lifecycle requires enabled predecode build");
#endif
    if (o.prefix_schedule == "component_owned_tail" &&
        ((o.execution != "shared" && o.execution != "pipeline") || !lifecycle_mode || o.parallel_import))
        throw std::invalid_argument("Component-owned tail requires shared prefix mode and whole-component import");
    if (o.prefix_schedule == "component_owned_whole" &&
        ((o.execution != "shared" && o.execution != "pipeline") || !lifecycle_mode ||
         o.parallel_import || o.partitions != 1 || o.component_parallelism != 1))
        throw std::invalid_argument("Whole-component prefix requires shared CP1 prefix mode, P1 and serial owner import");
    if (o.prefix_schedule == "component_group_owned" &&
        ((o.execution != "shared" && o.execution != "pipeline") || !lifecycle_mode))
        throw std::invalid_argument("Group-owned prefix requires shared prefix mode");
    if (o.conflict_geometry != "symmetric_width" && o.conflict_geometry != "three_planes")
        throw std::invalid_argument("Unknown conflict geometry");
    if (o.conflict_geometry == "three_planes" &&
        ((o.execution != "shared" && o.execution != "pipeline") || o.conflict_width != 0 || o.partitions != 2))
        throw std::invalid_argument("Three-plane geometry requires shared P2 and width0");
    if (o.continuation_placement != "component_captain" && o.continuation_placement != "prefix_tail_owner")
        throw std::invalid_argument("Unknown continuation placement");
    if (o.continuation_placement == "prefix_tail_owner" &&
        ((o.prefix_schedule != "component_owned_tail" && o.prefix_schedule != "component_owned_whole") ||
         o.component_parallelism != 1))
        throw std::invalid_argument("Tail-owner continuation requires CP1 component-owned prefix");
    if (o.cpus.size() > 1)
        o.worker_cpu = o.cpus[1];
    if (o.replay_kind != "none" && o.replay_kind != "hot" && o.replay_kind != "context")
        throw std::invalid_argument("Unknown replay kind");
    if (o.replay_kind != "none" && (o.selected_shots.size() != 1 || !o.replay_count ||
        o.mode == "batch" || o.mode == "full-batch" || o.mode == "pipe-batch"))
        throw std::invalid_argument("Replay needs one original shot, positive repetitions and latency/diagnostic mode");
    if ((o.mode == "prefix-batch" || o.mode == "full-batch" || o.mode == "pipe-batch") &&
        (o.replay_kind != "none" || !o.selected_shots.empty()))
        throw std::invalid_argument("Prefix batch requires continuous inputs without shot selection or replay");
    if ((o.mode == "full-batch" || o.mode == "full-latency") && o.backend == "generic")
        throw std::invalid_argument("Full lifecycle mode requires structured prepared-state API");
    if (o.execution == "pipeline") {
        if (o.pipeline_schedule != "overlap" && o.pipeline_schedule != "serial_control" && o.pipeline_schedule != "single_thread")
            throw std::invalid_argument("Unknown pipeline schedule");
        if ((o.pipeline_schedule == "single_thread") != (o.cpus.size() == 1))
            throw std::invalid_argument("Pipeline single_thread requires one physical core; other schedules use 2G");
        if (
#if BB_TAILRESET_SYNDROME_STATS
            o.mode != "pipe-statistics" &&
#endif
            o.mode != "pipe-batch" && o.mode != "pipe-latency" && o.mode != "pipe-stages" && o.mode != "pipe-timeline")
            throw std::invalid_argument("Pipeline requires a caller-latency or full-lifecycle batch mode");
    }
    if (o.mode == "input-features" && (o.backend == "generic" || o.execution != "serial"))
        throw std::invalid_argument("Input features require a serial structured graph construction");
    return o;
}
uint64_t mix(uint64_t state, uint64_t value) {
    state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    state ^= state >> 30;
    state *= 0xbf58476d1ce4e5b9ULL;
    state ^= state >> 27;
    state *= 0x94d049bb133111ebULL;
    return state ^ (state >> 31);
}
uint64_t sample_seed(const Options& o) {
    return mix(mix(mix(mix(o.root_seed, o.stream_id), o.distance), std::bit_cast<uint64_t>(o.p)), o.distance);
}
double us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}
template <typename T>
void write(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
template <typename T>
T read(std::istream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!in)
        throw std::runtime_error("Truncated dataset");
    return value;
}
stim::Circuit circuit_for(const Options& o) {
    stim::CircuitGenParameters gen(o.distance, o.distance, "rotated_memory_x");
    gen.after_clifford_depolarization = o.p;
    gen.before_round_data_depolarization = o.p;
    gen.before_measure_flip_probability = o.p;
    gen.after_reset_flip_probability = o.p;
    return stim::generate_surface_code_circuit(gen).circuit;
}
void prepare(const Options& o) {
    auto start = Clock::now();
    auto circuit = circuit_for(o);
    auto dem = stim::ErrorAnalyzer::circuit_to_detector_error_model(circuit, true, true, false, 0, false, false);
    // Keep the exact generated model alongside the persistent binary input.
    std::ofstream circuit_file(o.output / "circuit.stim"), dem_file(o.output / "model.dem");
    circuit_file << circuit;
    dem_file << dem;
    circuit_file.close();
    dem_file.close();
    if (!circuit_file || !dem_file)
        throw std::runtime_error("Could not preserve circuit/DEM");
    auto decoder = beyond_blossom::detector_error_model_to_mwpm(dem, beyond_blossom::NUM_DISTINCT_WEIGHTS);
    const auto seed = sample_seed(o);
    Header h{
        MAGIC,
        2,
        o.distance,
        std::bit_cast<uint64_t>(o.p),
        o.shots,
        circuit.count_detectors(),
        0,
        0,
        0,
        seed,
        std::bit_cast<uint64_t>(decoder.flooder.graph.normalising_constant),
        o.stream_id};
    std::ofstream data(o.dataset, std::ios::binary);
    write(data, h);
    std::ofstream refs(o.output / "reference_shots.csv");
    refs << "shot_index,detection_events,observable_mask,integer_weight\n";
    for (size_t offset = 0; offset < o.shots; offset += 1024) {
        size_t count = std::min(size_t{1024}, o.shots - offset);
        std::mt19937_64 rng(mix(seed, offset));
        auto batch = stim::sample_batch_detection_events<stim::MAX_BITWORD_WIDTH>(circuit, count, rng);
        for (size_t k = 0; k < count; k++) {
            std::vector<uint64_t> hits;
            for (size_t detector = 0; detector < h[5]; detector++)
                if (batch.first[detector][k])
                    hits.push_back(detector);
            h[6] += hits.size();
            h[7] = mix(h[7], hits.size());
            for (auto detector : hits)
                h[7] = mix(h[7], detector);
            auto expected = beyond_blossom::decode_detection_events_for_up_to_64_observables(decoder, hits, false);
            h[8] = mix(mix(h[8], expected.obs_mask), static_cast<uint64_t>(expected.weight));
            write(data, static_cast<uint64_t>(hits.size()));
            data.write(reinterpret_cast<const char*>(hits.data()), hits.size() * sizeof(uint64_t));
            write(data, static_cast<uint64_t>(expected.obs_mask));
            write(data, static_cast<int64_t>(expected.weight));
            refs << offset + k << ',' << hits.size() << ',' << expected.obs_mask << ',' << expected.weight << '\n';
        }
    }
    data.seekp(0);
    write(data, h);
    data.close();
    refs.close();
    if (!data || !refs)
        throw std::runtime_error("Could not write input/reference dataset");
    std::ofstream metadata(o.output / "case.csv");
    metadata
        << "distance,error_rate,shots,num_detectors,detection_events,syndrome_hash,result_hash,seed,scale,setup_us\n"
        << std::setprecision(17) << o.distance << ',' << o.p << ',' << h[4] << ',' << h[5] << ',' << h[6] << ',' << h[7]
        << ',' << h[8] << ',' << seed << ',' << std::bit_cast<double>(h[10]) << ',' << us(start, Clock::now()) << '\n';
    if (!metadata)
        throw std::runtime_error("Could not write metadata");
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot read external input: " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    if (!in.eof() && in.fail()) throw std::runtime_error("External input read failed");
    return text.str();
}
void import_input(const Options& o) {
    if (o.import_hits.empty() || o.import_dem.empty() || o.import_circuit.empty() || o.backend != "generic" ||
        o.execution != "serial") throw std::invalid_argument("External import requires original hits, DEM, circuit and generic serial reference");
    const auto dem_text = read_text(o.import_dem);
    const auto circuit_text = read_text(o.import_circuit);
    stim::DetectorErrorModel dem(dem_text);
    stim::Circuit circuit(circuit_text);
    if (dem.count_detectors() != circuit.count_detectors()) throw std::runtime_error("External DEM/circuit detector count mismatch");
    auto decoder = beyond_blossom::detector_error_model_to_mwpm(dem, beyond_blossom::NUM_DISTINCT_WEIGHTS);
    Header h{MAGIC, 3, o.distance, std::bit_cast<uint64_t>(o.p), o.shots, dem.count_detectors(), 0, 0, 0,
             sample_seed(o), std::bit_cast<uint64_t>(decoder.flooder.graph.normalising_constant), o.stream_id};
    std::ofstream data(o.dataset, std::ios::binary), refs(o.output / "reference_shots.csv");
    write(data, h);
    refs << "shot_index,detection_events,observable_mask,integer_weight\n";
    std::ifstream input(o.import_hits);
    std::set<uint64_t> ids;
    for (size_t k = 0; k < o.shots; ++k) {
        uint64_t id, size;
        if (!(input >> id >> size) || size > h[5] || !ids.insert(id).second)
            throw std::runtime_error("Invalid/duplicate imported shot or count");
        std::vector<uint64_t> hits(size);
        for (auto& hit : hits)
            if (!(input >> hit) || hit >= h[5]) throw std::runtime_error("Invalid imported global detector ID");
        if (!std::is_sorted(hits.begin(), hits.end()) || std::adjacent_find(hits.begin(), hits.end()) != hits.end())
            throw std::runtime_error("Imported hits must be sorted and unique");
        const auto expected = beyond_blossom::decode_detection_events_for_up_to_64_observables(decoder, hits, false);
        h[6] += size;
        h[7] = mix(mix(h[7], id), size);
        for (auto hit : hits) h[7] = mix(h[7], hit);
        h[8] = mix(mix(h[8], expected.obs_mask), static_cast<uint64_t>(expected.weight));
        write(data, id);
        write(data, size);
        data.write(reinterpret_cast<const char*>(hits.data()), size * sizeof(uint64_t));
        write(data, static_cast<uint64_t>(expected.obs_mask));
        write(data, static_cast<int64_t>(expected.weight));
        refs << id << ',' << size << ',' << expected.obs_mask << ',' << expected.weight << '\n';
    }
    input >> std::ws;
    if (!input.eof()) throw std::runtime_error("Unexpected extra imported records");
    data.seekp(0); write(data, h); data.close(); refs.close();
    if (!data || !refs) throw std::runtime_error("External dataset write failed");
    std::ofstream dem_file(o.dataset.parent_path() / "model.dem"), circuit_file(o.dataset.parent_path() / "circuit.stim");
    dem_file << dem_text; circuit_file << circuit_text; dem_file.close(); circuit_file.close();
    if (!dem_file || !circuit_file) throw std::runtime_error("External model preservation failed");
    std::ofstream metadata(o.output / "case.csv");
    metadata << "distance,error_rate,shots,num_detectors,detection_events,syndrome_hash,result_hash,scale,format_version\n"
             << std::setprecision(17) << o.distance << ',' << o.p << ',' << h[4] << ',' << h[5] << ',' << h[6]
             << ',' << h[7] << ',' << h[8] << ',' << std::bit_cast<double>(h[10]) << ",3\n";
    if (!metadata) throw std::runtime_error("External metadata write failed");
}
struct Dataset {
    Header header;
    std::vector<std::vector<uint64_t>> hits;
    std::vector<beyond_blossom::MatchingResult> expected;
    uint64_t syndrome_hash = 0, result_hash = 0, detection_events = 0;
    std::vector<beyond_blossom::MatchingResult> parent;
    std::vector<size_t> shot_ids;
    std::vector<size_t> replay_iteration;
    std::vector<bool> replay_target;
};
Dataset load(const Options& o) {
    std::ifstream in(o.dataset, std::ios::binary);
    Dataset data;
    auto& h = data.header;
    h = read<Header>(in);
    if (h[0] != MAGIC || (h[1] != 2 && h[1] != 3) || h[2] != o.distance || h[3] != std::bit_cast<uint64_t>(o.p) || h[4] != o.shots ||
        h[9] != sample_seed(o) || h[11] != o.stream_id)
        throw std::runtime_error("Dataset identity/options mismatch");
    const size_t count = o.limit ? std::min(o.limit, o.shots) : o.shots;
    data.hits.reserve(count);
    data.expected.reserve(count);
    std::set<size_t> original_ids;
    for (size_t k = 0; k < count; k++) {
        const auto id = h[1] == 3 ? read<uint64_t>(in) : k;
        if (h[1] == 3 && !original_ids.insert(id).second) throw std::runtime_error("Duplicate original shot identity");
        if (h[1] == 3) data.syndrome_hash = mix(data.syndrome_hash, id);
        auto size = read<uint64_t>(in);
        if (size > h[5])
            throw std::runtime_error("Invalid hit count");
        std::vector<uint64_t> hits(size);
        in.read(reinterpret_cast<char*>(hits.data()), size * sizeof(uint64_t));
        data.syndrome_hash = mix(data.syndrome_hash, size);
        for (auto detector : hits) {
            if (detector >= h[5])
                throw std::runtime_error("Invalid detector id");
            data.syndrome_hash = mix(data.syndrome_hash, detector);
        }
        beyond_blossom::MatchingResult expected;
        expected.obs_mask = read<uint64_t>(in);
        expected.weight = read<int64_t>(in);
        data.result_hash = mix(mix(data.result_hash, expected.obs_mask), static_cast<uint64_t>(expected.weight));
        data.detection_events += size;
        data.hits.push_back(std::move(hits));
        data.expected.push_back(expected);
        data.shot_ids.push_back(id);
    }
    if (count == h[4] && (data.syndrome_hash != h[7] || data.result_hash != h[8] || data.detection_events != h[6] ||
                          in.peek() != std::char_traits<char>::eof()))
        throw std::runtime_error("Dataset checksum/length mismatch");
    if (!o.parent_reference.empty()) {
        std::ifstream parent(o.parent_reference, std::ios::binary);
        if (read<uint64_t>(parent) != 0x504152454e543031ULL || read<uint64_t>(parent) != count ||
            read<uint64_t>(parent) != data.syndrome_hash || read<uint64_t>(parent) != h[10])
            throw std::runtime_error("Parent reference identity mismatch");
        for (size_t k = 0; k < count; k++) {
            beyond_blossom::MatchingResult value;
            value.obs_mask = read<uint64_t>(parent);
            value.weight = read<int64_t>(parent);
            data.parent.push_back(value);
        }
        if (parent.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Parent reference length mismatch");
    }
    if (o.replay_kind != "none") {
        const auto found = std::find(data.shot_ids.begin(), data.shot_ids.end(), o.selected_shots.front());
        if (found == data.shot_ids.end()) throw std::invalid_argument("Replay target absent from input");
        const auto target = static_cast<size_t>(found - data.shot_ids.begin());
        const auto first = o.replay_kind == "context" ? target - std::min(target, o.context_count) : target;
        Dataset selected;
        selected.header = data.header;
        for (size_t repetition = 0; repetition < o.replay_count; ++repetition) {
            for (size_t index = first; index <= target; ++index) {
                selected.hits.push_back(data.hits[index]);
                selected.expected.push_back(data.expected[index]);
                if (!data.parent.empty()) selected.parent.push_back(data.parent[index]);
                selected.shot_ids.push_back(data.shot_ids[index]);
                selected.replay_iteration.push_back(repetition);
                selected.replay_target.push_back(index == target);
                selected.detection_events += data.hits[index].size();
                selected.syndrome_hash = mix(selected.syndrome_hash, data.hits[index].size());
                for (const auto hit : data.hits[index]) selected.syndrome_hash = mix(selected.syndrome_hash, hit);
                selected.result_hash = mix(mix(selected.result_hash, data.expected[index].obs_mask), static_cast<uint64_t>(data.expected[index].weight));
            }
        }
        return selected;
    }
    if (!o.selected_shots.empty()) {
        Dataset selected;
        selected.header = data.header;
        std::set<size_t> unique;
        for (const auto id : o.selected_shots) {
            const auto found = std::find(data.shot_ids.begin(), data.shot_ids.end(), id);
            if (found == data.shot_ids.end() || !unique.insert(id).second) throw std::invalid_argument("Invalid/duplicate selected original shot");
            const auto index = static_cast<size_t>(found - data.shot_ids.begin());
            selected.hits.push_back(std::move(data.hits[index]));
            selected.expected.push_back(data.expected[index]);
            if (!data.parent.empty()) selected.parent.push_back(data.parent[index]);
            selected.shot_ids.push_back(id);
            selected.detection_events += selected.hits.back().size();
            selected.syndrome_hash = mix(selected.syndrome_hash, selected.hits.back().size());
            for (const auto hit : selected.hits.back()) selected.syndrome_hash = mix(selected.syndrome_hash, hit);
            selected.result_hash = mix(mix(selected.result_hash, selected.expected.back().obs_mask), static_cast<uint64_t>(selected.expected.back().weight));
        }
        return selected;
    }
    return data;
}
void pin_main(int cpu) {
#if defined(__linux__)
    if (cpu < 0 || cpu >= CPU_SETSIZE)
        throw std::invalid_argument("CPU outside cpu_set_t");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask))
        throw std::runtime_error("Could not pin main thread");
#else
    if (cpu != -1) throw std::invalid_argument("Darwin does not support physical CPU pinning");
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0))
        throw std::runtime_error("Cannot set USER_INITIATED QoS");
#endif
}
struct ThreadIdentity {
    uint64_t tid;
    int cpu;
    int64_t component = -1, buffer = -1;
    size_t lane = 0, group_threads = 1, captain = 0;
    int socket = -1, physical_core = -1;
};
using ExecutionSnapshot = std::vector<ThreadIdentity>;
ThreadIdentity capture_thread(uint64_t tid, int expected) {
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (!tid || sched_getaffinity(static_cast<pid_t>(tid), sizeof(mask), &mask) || CPU_COUNT(&mask) != 1 ||
        !CPU_ISSET(expected, &mask))
        throw std::runtime_error("Actual thread identity/affinity mismatch");
    ThreadIdentity result{tid, expected};
    const auto root = "/sys/devices/system/cpu/cpu" + std::to_string(expected) + "/topology/";
    std::ifstream socket(root + "physical_package_id"), core(root + "core_id");
    if (!(socket >> result.socket) || !(core >> result.physical_core))
        throw std::runtime_error("Could not read actual CPU physical topology");
    return result;
#else
    if (!tid || expected != -1) throw std::runtime_error("Invalid Darwin thread identity");
    return ThreadIdentity{tid, -1};
#endif
}
ExecutionSnapshot capture_serial(const Options& o) {
#if defined(__linux__)
    return {capture_thread(static_cast<uint64_t>(syscall(SYS_gettid)), o.main_cpu)};
#else
    uint64_t tid = 0;
    if (pthread_threadid_np(nullptr, &tid)) throw std::runtime_error("Cannot query Darwin thread ID");
    return {capture_thread(tid, -1)};
#endif
}
void write_execution(const Options& o, const ExecutionSnapshot& begin, const ExecutionSnapshot& end) {
    if (begin.size() != o.cpus.size() || end.size() != begin.size())
        throw std::runtime_error("Team size mismatch");
    std::set<uint64_t> tids;
    std::set<std::pair<int, int>> physical;
    std::set<int> sockets;
    for (size_t i = 0; i < begin.size(); i++) {
        if (begin[i].tid != end[i].tid || begin[i].cpu != end[i].cpu || begin[i].cpu != o.cpus[i] ||
            !tids.insert(begin[i].tid).second)
            throw std::runtime_error("Thread identity changed/duplicated");
#if defined(__linux__)
        if (begin[i].socket != end[i].socket || begin[i].physical_core != end[i].physical_core ||
            !physical.insert({begin[i].socket, begin[i].physical_core}).second)
            throw std::runtime_error("Measured team must use distinct physical cores");
        sockets.insert(begin[i].socket);
#endif
    }
#if defined(__linux__)
    if (sockets.size() != 1) throw std::runtime_error("Measured team must use one socket");
#endif
    std::ofstream out(o.output / "execution.csv");
    out << "execution,total_threads,main_tid,main_cpu,team_entries,placement_checks,scope,component_parallelism\n"
        << o.execution << ',' << begin.size() << ',' << begin[0].tid << ',' << begin[0].cpu << ','
        << (o.execution != "serial") << ",2,before_warmup_and_after_all_shots,"
        << ((o.execution == "shared" || o.execution == "pipeline") ? o.component_parallelism : 1) << '\n';
    std::ofstream platform(o.output / "placement.json");
#if defined(__linux__)
    platform << "{\"physical_affinity_verified\":true,\"policy\":\"Linux CPU affinity\"}\n";
#else
    platform << "{\"physical_affinity_verified\":false,\"policy\":\"Darwin USER_INITIATED QoS\"}\n";
#endif
    std::ofstream threads(o.output / "execution_threads.csv");
    threads << "boundary,thread_index,tid,cpu,affinity_mask,component_id,lane,group_threads,captain,socket,physical_core,buffer_id\n";
    for (const auto& [label, snapshot] : {std::pair{"before", &begin}, std::pair{"after", &end}})
        for (size_t i = 0; i < snapshot->size(); i++)
            threads << label << ',' << i << ',' << (*snapshot)[i].tid << ',' << (*snapshot)[i].cpu << ','
                    << (*snapshot)[i].cpu << ',' << (*snapshot)[i].component << ',' << (*snapshot)[i].lane << ','
                    << (*snapshot)[i].group_threads << ',' << (*snapshot)[i].captain << ','
                    << (*snapshot)[i].socket << ',' << (*snapshot)[i].physical_core << ',' << (*snapshot)[i].buffer << '\n';
    if (!out || !threads)
        throw std::runtime_error("Execution receipt write failed");
}
template <typename Callback>
void serial_window(const Options& o, Callback&& callback) {
    const auto begin = capture_serial(o);
    callback();
    const auto end = capture_serial(o);
    write_execution(o, begin, end);
}
#ifndef STENCIL_GENERIC_ONLY
template <typename Decoder>
decltype(auto) metadata_decoder(Decoder& d) {
    if constexpr (requires { d.serial_decoder(); })
        return d.serial_decoder();
    else if constexpr (requires { d.topology(); })
        return d.topology();
    else
        return (d);
}
template <typename Decoder>
ExecutionSnapshot capture_team(const Options& o, Decoder& d) {
    if (d.team_size() != o.cpus.size())
        throw std::runtime_error("Actual team size differs from requested CPUs");
    ExecutionSnapshot result;
    auto append = [&](const auto& snapshots, size_t first, int64_t buffer, bool single_thread) {
        const size_t group = snapshots.size();
        for (size_t i = 0; i < group; ++i) {
            const auto& t = snapshots[i];
            const size_t global = single_thread ? 0 : first + i;
#if defined(__linux__)
            if (t.requested_cpu != o.cpus.at(global) || t.affinity != std::vector<int>{o.cpus.at(global)})
                throw std::runtime_error("Decoder CPU snapshot mismatch");
#else
            if (t.requested_cpu != -1 || !t.affinity.empty() || t.qos_class != QOS_CLASS_USER_INITIATED)
                throw std::runtime_error("Decoder Darwin QoS snapshot mismatch");
#endif
            if (single_thread && !result.empty()) {
                if (result.front().tid != t.thread_id || result.front().cpu != t.requested_cpu)
                    throw std::runtime_error("Logical single-thread buffers differ in actual thread identity");
                continue;
            }
            result.push_back(capture_thread(t.thread_id, o.cpus.at(global)));
            result.back().component = t.component == SIZE_MAX ? -1 : static_cast<int64_t>(t.component);
            result.back().lane = t.lane;
            const size_t component_group = o.component_parallelism == 1 || group == 1 ? group : group / 2;
            const size_t expected_component = o.component_parallelism == 1 || group == 1 ? SIZE_MAX : i / component_group;
            if (t.component != expected_component || t.lane != i % component_group)
                throw std::runtime_error("Shared component/lane snapshot differs from compiled grouping");
            result.back().group_threads = component_group;
            result.back().captain = global - t.lane;
            result.back().buffer = buffer;
        }
    };
    if constexpr (requires { d.thread_snapshots(size_t{}); d.group_size(); }) {
        const bool single = o.pipeline_schedule == "single_thread";
        if (d.thread_snapshots(0).size() != d.group_size() || d.thread_snapshots(1).size() != d.group_size())
            throw std::runtime_error("Pipeline group snapshot count differs");
        append(d.thread_snapshots(0), 0, 0, single);
        append(d.thread_snapshots(1), d.group_size(), 1, single);
    } else if constexpr (requires { d.thread_snapshots(); }) {
        if (d.thread_snapshots().size() != o.cpus.size())
            throw std::runtime_error("Thread snapshot count mismatch");
        append(d.thread_snapshots(), 0, -1, false);
    } else {
        result = capture_serial(o);
        result.push_back(capture_thread(d.worker_thread_id(), o.cpus[1]));
    }
    return result;
}
template <typename Decoder, typename Callback>
void decode_window(const Options& o, Decoder& decoder, Callback&& callback) {
    if constexpr (requires { decoder.run_with_team(callback); }) {
        ExecutionSnapshot begin, end;
        decoder.run_with_team([&] {
            begin = capture_team(o, decoder);
            callback();
            end = capture_team(o, decoder);
        });
        write_execution(o, begin, end);
    } else
        serial_window(o, std::forward<Callback>(callback));
}
#endif

#include "setup.h"
#include "cache_counters.h"
#include "measurement.h"
#include "structured_adapter.h"
#include "timer_snapshot.h"
#include "timeline.h"
#include "lifecycle.h"
#include "full_cycle.h"
#if BB_TAILRESET_SYNDROME_STATS
#include "../tailreset_statistics.h"
#endif
#include "pipeline_measurement.h"
#include "diagnostic.h"

void run(const Options& o) {
#if BB_TAILRESET_SYNDROME_STATS
    if (o.mode != "pipe-statistics")
        throw std::invalid_argument("Syndrome-statistics binary cannot collect decoding times");
#endif
    std::filesystem::create_directories(o.output);
    // The runner opens its process log first; all native artifacts remain new.
    for (const auto& entry : std::filesystem::directory_iterator(o.output))
        if (entry.path().filename() != "process.log")
            throw std::invalid_argument("Refusing existing native output artifacts");
    pin_main(o.main_cpu);
    if (o.mode == "prepare") {
        if (o.backend != "generic" || o.execution != "serial")
            throw std::invalid_argument("Input preparation requires normal official generic oracle");
        if (std::filesystem::exists(o.dataset)) throw std::invalid_argument("Refusing existing dataset");
        prepare(o); return;
    }
    if (o.mode == "import") { import_input(o); return; }
#if BB_STRUCTURED_PROFILE_COUNTERS && BB_STRUCTURED_PROFILE_TIMERS
    if (o.mode != "diagnostic" && o.mode != "prefix-timeline" && o.mode != "queue-stages") throw std::invalid_argument("Instrumented binary cannot collect pure timing");
#elif BB_STRUCTURED_PROFILE_TIMERS
    if (o.mode != "diag-time-only" && o.mode != "prefix-timeline" && o.mode != "prefix-stages" && o.mode != "pipe-stages" && o.mode != "pipe-timeline")
        throw std::invalid_argument("Time-only binary requires a diagnostic mode");
#if BB_ENABLE_PROFILE || BB_ENABLE_NVTX || BB_PREDECODE_DIAGNOSTICS
    throw std::invalid_argument("Time-only binary requires all algorithm profile and legacy diagnostics disabled");
#endif
#elif BB_STRUCTURED_PROFILE_COUNTERS
    throw std::invalid_argument("Counter-only binary has no duration collection mode");
#else
    if (o.mode == "diagnostic" || o.mode == "diag-time-only" || o.mode == "prefix-timeline" || o.mode == "prefix-stages" || o.mode == "pipe-stages" || o.mode == "pipe-timeline" || o.mode == "queue-stages")
        throw std::invalid_argument("Diagnostic mode requires a separate build");
#endif
#if BB_STRUCTURED_PROFILE_TIMELINE
    if (o.mode == "prefix-stages" || o.mode == "pipe-stages") throw std::invalid_argument("Stage duration modes require timeline endpoints disabled");
#else
    if (o.mode == "prefix-timeline" || o.mode == "pipe-timeline") throw std::invalid_argument("Prefix timeline requires timeline endpoints enabled");
#endif
#if BB_STRUCTURED_PROFILE_TIMERS || BB_STRUCTURED_PROFILE_COUNTERS || BB_STRUCTURED_PROFILE_TIMELINE || \
    BB_ENABLE_PROFILE || BB_ENABLE_NVTX || BB_PREDECODE_DIAGNOSTICS
    if (o.mode == "prefix-batch" || o.mode == "full-batch" || o.mode == "full-latency" ||
        o.mode == "batch" || o.mode == "latency" || o.mode == "pipe-batch" || o.mode == "pipe-latency")
        throw std::invalid_argument("Pure modes require all internal instrumentation disabled");
#endif
    const auto setup_start = Clock::now();
    const auto data = load(o);
    const auto after_input = Clock::now();
    const bool frozen_model = data.header[1] == 3 || o.model_source == "frozen";
    const auto dem = frozen_model
        ? stim::DetectorErrorModel(read_text(o.dataset.parent_path() / "model.dem"))
        : stim::ErrorAnalyzer::circuit_to_detector_error_model(circuit_for(o), true, true, false, 0, false, false);
    if (dem.count_detectors() != data.header[5]) throw std::runtime_error("Dataset/model detector count mismatch");
    const auto after_model = Clock::now();
    const auto calibration = calibrate_clock();
    const auto before_graph = Clock::now();
    auto persist_setup = [&] {
        const auto after_graph = Clock::now();
        std::ofstream out(o.output / "setup.json");
        out << "{\"scope\":\"setup only, excluded from all decode timing\",\"model_source\":\""
            << (frozen_model ? "frozen" : "generate") << "\",\"detectors\":" << data.header[5]
            << ",\"input_us\":" << us(setup_start, after_input) << ",\"model_us\":" << us(after_input, after_model)
            << ",\"clock_calibration_us\":" << us(after_model, before_graph) << ",\"graph_us\":" << us(before_graph, after_graph)
            << ",\"quantization_scale_bits\":" << data.header[10] << "}\n";
        if (!out) throw std::runtime_error("Setup receipt write failed");
    };
    if (o.backend == "generic") {
        if (o.execution != "serial") throw std::invalid_argument("Official upstream remains serial");
#if BB_UNIFIED_CONTROL
        beyond_blossom::DecoderOptions options;
        options.strategy = beyond_blossom::DecoderStrategy::Upstream;
        // Serial affinity is owned by the caller, as in the direct API.
        beyond_blossom::StrategyDecoder decoder(dem, options);
        if (std::bit_cast<uint64_t>(decoder.metadata().normalising_constant) != data.header[10])
            throw std::runtime_error("Upstream quantization mismatch");
        persist_setup();
        persist_private_storage(o, decoder);
        auto m = measure(o, data, [&](size_t k) { return decoder.decode(data.hits[k]); },
            [&](auto&& body) { decoder.run_with_team([&](auto&) { serial_window(o, body); }); });
#else
        auto decoder = beyond_blossom::detector_error_model_to_mwpm(dem, beyond_blossom::NUM_DISTINCT_WEIGHTS);
        if (std::bit_cast<uint64_t>(decoder.flooder.graph.normalising_constant) != data.header[10])
            throw std::runtime_error("Upstream quantization mismatch");
        persist_setup();
        persist_private_storage(o, decoder);
        auto m = measure(o, data,
            [&](size_t k) { return beyond_blossom::decode_detection_events_for_up_to_64_observables(decoder, data.hits[k], false); },
            [&](auto&& body) { serial_window(o, body); });
#endif
        persist_measurement(o, data, m, calibration);
        for (size_t k = 0; k < m.actual.size(); ++k)
            if (m.actual[k] != data.expected[k]) throw std::runtime_error("Official upstream result mismatch");
        std::ofstream check(o.output / "correctness.json");
        check << "{\"status\":\"passed\",\"shots\":" << m.actual.size()
              << ",\"weight_differences\":0,\"observable_differences\":0,\"actual_candidate_witnesses\":0,\"policy\":\"exact upstream reference\"}\n";
        return;
    }
#ifndef STENCIL_GENERIC_ONLY
    std::vector<beyond_blossom::MatchingResult> values;
    with_structured_decoder(o, dem, false, [&](auto& decoder) {
        auto& metadata = metadata_decoder(decoder);
        if (std::bit_cast<uint64_t>(metadata.normalising_constant()) != data.header[10])
            throw std::runtime_error("Structured quantization mismatch");
        persist_setup();
        persist_private_storage(o, decoder);
        persist_kernel_selection(o, decoder);
        std::vector<size_t> component_of(data.header[5], SIZE_MAX);
        for (size_t c = 0; c < metadata.components().size(); ++c)
            for (const auto detector : metadata.components()[c].global_detectors) component_of.at(detector) = c;
        std::ofstream features(o.output / "input_features.csv");
        features << "shot_index,shot_execution_index,input_events,component0_events,component1_events\n";
        for (size_t k = 0; k < data.hits.size(); ++k) {
            std::array<size_t, 2> counts{};
            for (const auto detector : data.hits[k]) ++counts.at(component_of.at(detector));
            features << data.shot_ids[k] << ',' << k << ',' << data.hits[k].size() << ',' << counts[0] << ',' << counts[1] << '\n';
        }
        features.close();
        if (!features) throw std::runtime_error("Input feature persistence failed");
        if (o.mode == "input-features") {
            std::ofstream scope(o.output / "feature_scope.json");
            scope << R"({"schema_version":1,"mode":"input-features","scope":"input and frozen graph construction only; no decode, warmup, team or decoder timing","component_mapping":"global detector IDs mapped through the constructed independent X/Z components","feature_extraction":"outside all measurement windows"})" << '\n';
            if (!scope) throw std::runtime_error("Feature scope persistence failed");
            return;
        }
        if constexpr (requires { decoder.next_buffer(); decoder.drain(); }) {
#if BB_EXPERIMENT_PIPELINE
#if BB_STRUCTURED_PROFILE_TIMERS
            values = profile_pipeline(o, data, decoder, calibration);
#else
            values = measure_pipeline(o, data, decoder, calibration);
#endif
#endif
            return;
        } else {
        if (o.mode == "full-batch" || o.mode == "full-latency") {
            if constexpr (requires { decoder.prepare_for_mwpm(data.hits.front()); decoder.solve_prepared(); decoder.reset_after_shot(); }) {
                values = measure_full_cycle(o, data, decoder, calibration);
            } else throw std::runtime_error("Full cycle requires explicit prepared-state lifecycle API");
            return;
        }
        if (o.mode == "prefix" || o.mode == "prefix-timeline" || o.mode == "prefix-batch" || o.mode == "prefix-stages" || o.mode == "queue-stages") {
            if constexpr (requires { decoder.prepare_for_mwpm(data.hits.front()); decoder.solve_prepared(); decoder.reset_after_shot(); }) {
#if BB_STRUCTURED_PROFILE_TIMERS
                values = profile_parallel(o, data, decoder, calibration);
#else
                if (o.mode == "prefix-batch") values = measure_lifecycle_batch(o, data, decoder, calibration);
                else values = measure_lifecycle(o, data, decoder, calibration);
#endif
            } else throw std::runtime_error("Prefix mode requires explicit prepared-state lifecycle API");
            return;
        }
#if BB_STRUCTURED_PROFILE_TIMERS
        values = profile_parallel(o, data, decoder, calibration);
#else
        auto m = measure(o, data, [&](size_t k) { return decoder.decode(data.hits[k]); },
                         [&](auto&& body) { decode_window(o, decoder, body); });
        persist_measurement(o, data, m, calibration);
        values = std::move(m.actual);
#endif
        }
    });
    if (o.mode == "input-features") return;
    verify_candidate(o, data, dem, values);
#else
    throw std::runtime_error("Generic-only executable cannot select structured");
#endif
}
}  // namespace
int main(int argc, char** argv) {
    try { run(parse(argc, argv)); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
