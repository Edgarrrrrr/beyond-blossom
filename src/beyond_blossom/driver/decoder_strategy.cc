// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/driver/decoder_strategy.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#if defined(__linux__)
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "beyond_blossom/driver/backend_user_graph.h"
#include "beyond_blossom/driver/mwpm_decoding.h"
#include "beyond_blossom/structured/driver/shared_component_decoder.h"

namespace beyond_blossom {
DecoderStrategy parse_decoder_strategy(std::string_view name) {
    if (name == "unstructured" || name == "upstream") return DecoderStrategy::Unstructured;
    if (name == "structured" || name == "gridsimd") return DecoderStrategy::Structured;
    if (name == "predecoded" || name == "predecode") return DecoderStrategy::Predecoded;
    if (name == "component-parallel" || name == "component") return DecoderStrategy::ComponentParallel;
    if (name == "pipelined" || name == "tailreset") return DecoderStrategy::Pipelined;
    if (name == "parallel-predecode" || name == "spatial") return DecoderStrategy::ParallelPredecode;
    throw std::invalid_argument("Unknown decoder strategy: " + std::string(name));
}
const char* decoder_strategy_name(DecoderStrategy strategy) {
    switch (strategy) {
        case DecoderStrategy::Unstructured: return "unstructured";
        case DecoderStrategy::Structured: return "structured";
        case DecoderStrategy::Predecoded: return "predecoded";
        case DecoderStrategy::ComponentParallel: return "component-parallel";
        case DecoderStrategy::Pipelined: return "pipelined";
        case DecoderStrategy::ParallelPredecode: return "parallel-predecode";
    }
    throw std::invalid_argument("Invalid decoder strategy");
}

struct StrategyDecoder::State {
    using Serial = structured::ComponentDecoder;
    using Shared = structured::SharedComponentDecoder;
    using Pipeline = structured::DoubleBufferComponentDecoder;
    DecoderMetadata info;
    bool active = false;
    std::unique_ptr<Mwpm> generic;
    std::unique_ptr<Serial> serial;
    std::unique_ptr<Shared> shared;
    std::unique_ptr<Pipeline> pipeline;
    MatchingResult (*packed_entry)(State&, const std::vector<uint64_t>&) = nullptr;
    void (*extended_entry)(State&, const std::vector<uint64_t>&, uint8_t*, total_weight_int&) = nullptr;
    void (*edges_entry)(State&, const std::vector<uint64_t>&, std::vector<int64_t>&) = nullptr;
    void (*matches_entry)(State&, const std::vector<uint64_t>&, std::vector<int64_t>&) = nullptr;
    void (*session_entry)(State&, const std::function<void()>&) = nullptr;
    std::vector<MatchingResult> (*batch_entry)(State&, std::span<const std::vector<uint64_t>>) = nullptr;
    void (*drain_entry)(State&) = nullptr;

    template <typename D> D& decoder() {
        if constexpr (std::is_same_v<D, Mwpm>) return *generic;
        else if constexpr (std::is_same_v<D, Serial>) return *serial;
        else if constexpr (std::is_same_v<D, Shared>) return *shared;
        else return *pipeline;
    }
    template <typename D, bool Correlations>
    static MatchingResult decode_one(D& decoder, const std::vector<uint64_t>& events) {
        if constexpr (std::is_same_v<D, Mwpm>)
            return beyond_blossom::decode_detection_events_for_up_to_64_observables(decoder, events, Correlations);
        else return decoder.decode(events);
    }
    template <typename D> void record_threads() {
        info.threads.clear();
        if constexpr (std::is_same_v<D, Shared> || std::is_same_v<D, Pipeline>) {
            auto& d = decoder<D>();
            info.actual_threads = d.team_size();
            const auto append = [&](const auto& snapshots, size_t buffer) {
                for (const auto& snapshot : snapshots)
                    info.threads.push_back({buffer, snapshot.component, snapshot.lane,
                        snapshot.requested_cpu, snapshot.thread_id, snapshot.affinity,
                        snapshot.qos_class, snapshot.qos_relative_priority});
            };
            if constexpr (std::is_same_v<D, Pipeline>) {
                append(d.thread_snapshots(0), 0);
                append(d.thread_snapshots(1), 1);
            } else append(d.thread_snapshots(), 0);
        } else {
            info.actual_threads = 1;
            DecoderThreadInfo thread;
#if defined(__linux__)
            thread.thread_id = static_cast<uint64_t>(syscall(SYS_gettid));
            cpu_set_t affinity;
            CPU_ZERO(&affinity);
            if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0)
                for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                    if (CPU_ISSET(cpu, &affinity)) thread.affinity.push_back(cpu);
#elif defined(__APPLE__)
            int error = pthread_threadid_np(nullptr, &thread.thread_id);
            if (error) throw std::system_error(error, std::generic_category(), "Reading serial thread ID");
            qos_class_t qos;
            error = pthread_get_qos_class_np(pthread_self(), &qos, &thread.qos_relative_priority);
            if (error) throw std::system_error(error, std::generic_category(), "Reading serial thread QoS");
            thread.qos_class = static_cast<int>(qos);
#endif
            info.threads.push_back(std::move(thread));
        }
    }
    template <typename D> static void run_session(State& s, const std::function<void()>& body) {
        if (s.active) throw std::logic_error("Decoder sessions cannot be nested.");
        const auto callback = [&] {
            s.active = true;
            struct Guard { bool& active; ~Guard() { active = false; } } guard{s.active};
            s.record_threads<D>();
            body();
            if constexpr (std::is_same_v<D, Pipeline>) s.decoder<D>().drain();
        };
        if constexpr (std::is_same_v<D, Shared> || std::is_same_v<D, Pipeline>)
            s.decoder<D>().run_with_team(callback);
        else callback();
    }
    template <typename D, bool Correlations = false> void bind() {
        packed_entry = [](State& s, const auto& events) { return decode_one<D, Correlations>(s.decoder<D>(), events); };
        extended_entry = [](State& s, const auto& events, uint8_t* observables, total_weight_int& weight) {
            auto& d = s.decoder<D>();
            total_weight_int shot_weight = 0;
            if constexpr (std::is_same_v<D, Mwpm>)
                beyond_blossom::decode_detection_events(d, events, observables, shot_weight, Correlations);
            else if constexpr (std::is_same_v<D, Serial>) d.decode(events, observables, shot_weight);
            else {
                const auto result = d.decode(events);
                fill_bit_vector_from_obs_mask(result.obs_mask, observables, s.info.num_observables);
                shot_weight = result.weight;
            }
            weight = shot_weight;
        };
        edges_entry = [](State& s, const auto& events, auto& edges) {
            if constexpr (std::is_same_v<D, Mwpm>) {
                if constexpr (Correlations)
                    beyond_blossom::decode_detection_events_to_edges_with_edge_correlations(s.decoder<D>(), events, edges);
                else beyond_blossom::decode_detection_events_to_edges(s.decoder<D>(), events, edges);
            } else if constexpr (std::is_same_v<D, Serial>) s.decoder<D>().decode_to_edges(events, edges);
            else throw std::invalid_argument("This parallel strategy has no physical-edge export.");
        };
        matches_entry = [](State& s, const auto& events, auto& edges) {
            auto& d = s.decoder<D>();
            if constexpr (std::is_same_v<D, Mwpm>) {
                beyond_blossom::decode_detection_events_to_match_edges(d, events);
                edges.clear();
                for (const auto& edge : d.flooder.match_edges) {
                    edges.push_back(edge.loc_from - d.flooder.graph.nodes.data());
                    edges.push_back(edge.loc_to ? edge.loc_to - d.flooder.graph.nodes.data() : -1);
                }
            } else if constexpr (std::is_same_v<D, Serial>) {
                d.decode_to_match_edges(events);
                edges = d.match_edges;
            } else if constexpr (std::is_same_v<D, Shared>) {
                const auto matches = d.decode_to_matches(events);
                edges.clear();
                for (const auto& match : matches) { edges.push_back(match.u); edges.push_back(match.v); }
            } else throw std::invalid_argument("This pipeline strategy has no compressed-match export.");
        };
        session_entry = &run_session<D>;
        batch_entry = [](State& s, std::span<const std::vector<uint64_t>> shots) {
            std::vector<MatchingResult> result(shots.size());
            run_session<D>(s, [&] {
                auto& d = s.decoder<D>();
                for (size_t k = 0; k < shots.size(); ++k) result[k] = decode_one<D, Correlations>(d, shots[k]);
            });
            return result;
        };
        drain_entry = [](State& s) {
            if constexpr (std::is_same_v<D, Pipeline>) s.decoder<D>().drain();
        };
    }
    explicit State(const DecoderOptions& options) {
        decoder_strategy_name(options.strategy);  // Validate even an explicitly cast enum.
        if (options.num_distinct_weights <= 0) throw std::invalid_argument("num_distinct_weights must be positive.");
        info.strategy = options.strategy;
        info.correlations_enabled = options.enable_correlations;
        info.supports_match_edges = !options.enable_correlations;
        info.search_graph_included = options.ensure_search_graph_included || options.enable_correlations;
        info.grid = options.grid;
        info.predecode_enabled = !options.enable_correlations && options.strategy != DecoderStrategy::Structured;
        if (options.strategy == DecoderStrategy::Unstructured) {
            if (options.grid) throw std::invalid_argument("Unstructured does not accept structured grid configuration.");
        } else {
            Serial::validate_correlations(options.enable_correlations);
            if (!options.grid) throw std::invalid_argument("A structured strategy requires explicit StaticGridSpec geometry.");
        }
        switch (options.strategy) {
            case DecoderStrategy::ComponentParallel: info.requested_threads = 2; break;
            case DecoderStrategy::Pipelined: info.requested_threads = 4; break;
            case DecoderStrategy::ParallelPredecode: info.requested_threads = 8; break;
            default: break;
        }
        const bool parallel_strategy = info.requested_threads > 1;
        if (options.same_algorithm_single_thread && !parallel_strategy)
            throw std::invalid_argument("The same-algorithm single-thread control requires ComponentParallel, Pipelined or ParallelPredecode.");
        info.same_algorithm_single_thread = options.same_algorithm_single_thread;
        if (info.same_algorithm_single_thread) info.requested_threads = 1;
        if (!parallel_strategy) {
            if (!options.cpus.empty()) throw std::invalid_argument("Serial strategy affinity is owned by the caller; leave DecoderOptions.cpus empty.");
        } else {
            if (options.cpus.size() != info.requested_threads)
                throw std::invalid_argument("Strategy " + std::string(decoder_strategy_name(options.strategy)) +
                    " requires exactly " + std::to_string(info.requested_threads) + " explicitly selected CPUs.");
            auto cpus = options.cpus;
            std::sort(cpus.begin(), cpus.end());
            if (cpus.front() < 0 || std::adjacent_find(cpus.begin(), cpus.end()) != cpus.end())
                throw std::invalid_argument("Parallel CPU IDs must be nonnegative and distinct.");
            info.supports_extended_observables = false;
            info.supports_edges = false;
            info.supports_match_edges = options.strategy == DecoderStrategy::ComponentParallel;
            info.execution = options.strategy == DecoderStrategy::ComponentParallel
                ? DecoderExecution::component_team : DecoderExecution::double_buffer;
            if (info.execution == DecoderExecution::double_buffer)
                info.pipeline_schedule = info.same_algorithm_single_thread ? "single_thread" : "overlap";
#if defined(__APPLE__) && defined(__aarch64__)
            // M4 candidate configuration: keep low-syndrome components on the
            // original serial entry before any partition preparation.
            static_assert(BB_M4_SPATIAL_COMPONENT_THRESHOLD > 0 &&
                          BB_M4_SPATIAL_COMPONENT_THRESHOLD <= UINT32_MAX);
            info.component_threshold = options.strategy == DecoderStrategy::ParallelPredecode
                ? BB_M4_SPATIAL_COMPONENT_THRESHOLD : 64;
#else
            info.component_threshold = options.strategy == DecoderStrategy::ParallelPredecode ? 0 : 64;
#endif
            info.shot_threshold = 32;
            info.conflict_width = options.strategy == DecoderStrategy::ParallelPredecode ? 0 : 3;
            info.partitions = options.strategy == DecoderStrategy::ParallelPredecode ? 2 : 1;
            info.parallel_import = options.strategy == DecoderStrategy::ParallelPredecode;
            info.prefix_schedule = options.strategy == DecoderStrategy::ParallelPredecode ? "component_group_owned" : "component_sequential";
            info.conflict_geometry = options.strategy == DecoderStrategy::ParallelPredecode ? "three_planes" : "symmetric_width";
            info.fixed_seed_construction = "fused";
            info.fixed_seed_input = options.strategy == DecoderStrategy::ParallelPredecode ? "indexed" : "scan";
            info.import_preparation = options.strategy == DecoderStrategy::ParallelPredecode ? "captained_serial" : "serial";
            info.import_reset_owner = options.strategy == DecoderStrategy::ParallelPredecode ? "captain" : "importers";
            info.seed_ordering = options.strategy == DecoderStrategy::ParallelPredecode ? "cycle_permutation" : "reconstruct";
        }
    }
    void finish_generic(Mwpm&& decoder, const DecoderOptions& options) {
        generic = std::make_unique<Mwpm>(std::move(decoder));
        info.num_nodes = generic->flooder.graph.nodes.size();
        info.num_observables = generic->flooder.graph.num_observables;
        info.search_graph_included = generic->search_flooder.graph.nodes.size() == info.num_nodes;
        info.normalising_constant = generic->flooder.graph.normalising_constant;
        if (options.enable_correlations) bind<Mwpm, true>();
        else bind<Mwpm, false>();
    }
    void finish_structured(BackendUserGraph& graph, const DecoderOptions& options) {
        auto first = Serial(graph, *options.grid, options.num_distinct_weights,
            info.search_graph_included, info.predecode_enabled);
        info.num_nodes = first.num_nodes();
        info.num_observables = first.num_observables();
        info.search_graph_included = first.has_search_graph();
        info.normalising_constant = first.normalising_constant();
        for (const auto& component : first.components())
            if (component.mwpm.flooder.graph.grid)
                for (const auto& grid : component.mwpm.flooder.graph.grid->components)
                    info.selected_simd.push_back(grid->selected_simd);
        if (info.execution == DecoderExecution::serial) {
            first.initialize_serial_seed_storage();
            serial = std::make_unique<Serial>(std::move(first));
            bind<Serial>();
            return;
        }
        if (info.num_observables > 64)
            throw std::invalid_argument("The selected parallel strategy supports at most 64 observables.");
        if (first.components().size() != 2)
            throw std::invalid_argument("Parallel strategies require two independent X/Z components.");
        const bool spatial = options.strategy == DecoderStrategy::ParallelPredecode;
        structured::SharedDecoderConfiguration config;
        if (spatial) {
            config.fixed_seed_construction = structured::FixedSeedConstruction::fused;
            config.fixed_seed_input = structured::FixedSeedInput::indexed;
            config.import_preparation = structured::ImportPreparation::captained_serial;
            config.import_reset_owner = structured::ImportResetOwner::captain;
            config.seed_ordering = structured::SeedOrdering::cycle_permutation;
        }
        const auto make_shared = [&](Serial&& decoder, std::vector<int> cpus) {
#if defined(__APPLE__)
            // Validated slot labels select team size/order only. Darwin does
            // not expose Linux-style binding to individual physical CPUs.
            std::fill(cpus.begin(), cpus.end(), -1);
#endif
            return Shared(std::move(decoder), std::move(cpus), info.partitions,
                info.component_threshold, info.shot_threshold, info.conflict_width, info.parallel_import,
                spatial ? Shared::PrefixSchedule::component_group_owned : Shared::PrefixSchedule::component_sequential,
                Shared::ContinuationPlacement::component_captain,
                spatial ? structured::TimePartitionCuts::Geometry::three_planes
                        : structured::TimePartitionCuts::Geometry::symmetric_width, config);
        };
        if (options.strategy == DecoderStrategy::ComponentParallel) {
            shared = std::make_unique<Shared>(make_shared(std::move(first), options.cpus));
            bind<Shared>();
        } else {
            const auto middle = options.cpus.begin() + options.cpus.size() / 2;
            auto second = Serial(graph, *options.grid, options.num_distinct_weights,
                info.search_graph_included, info.predecode_enabled);
            auto even = make_shared(std::move(first), info.same_algorithm_single_thread
                ? options.cpus : std::vector<int>(options.cpus.begin(), middle));
            auto odd = make_shared(std::move(second), info.same_algorithm_single_thread
                ? options.cpus : std::vector<int>(middle, options.cpus.end()));
            pipeline = std::make_unique<Pipeline>(std::move(even), std::move(odd),
                info.same_algorithm_single_thread ? Pipeline::Schedule::single_thread : Pipeline::Schedule::overlap);
            bind<Pipeline>();
        }
    }
};

StrategyDecoder::StrategyDecoder(const stim::DetectorErrorModel& dem, const DecoderOptions& options)
    : state(std::make_unique<State>(options)) {
    if (options.strategy == DecoderStrategy::Unstructured)
        state->finish_generic(detector_error_model_to_mwpm(dem, options.num_distinct_weights,
            state->info.search_graph_included, options.enable_correlations), options);
    else {
        auto graph = detector_error_model_to_backend_user_graph(dem, false, options.num_distinct_weights);
        state->finish_structured(graph, options);
    }
}
StrategyDecoder::StrategyDecoder(BackendUserGraph& graph, const DecoderOptions& options)
    : state(std::make_unique<State>(options)) {
    if (options.enable_correlations && graph.loaded_from_dem_without_correlations)
        throw std::invalid_argument("Reload this DEM with correlations before constructing a correlated decoder.");
    if (options.strategy == DecoderStrategy::Unstructured)
        state->finish_generic(graph.to_mwpm(options.num_distinct_weights, state->info.search_graph_included), options);
    else state->finish_structured(graph, options);
}
StrategyDecoder::~StrategyDecoder() = default;
StrategyDecoder::StrategyDecoder(StrategyDecoder&& other) { *this = std::move(other); }
StrategyDecoder& StrategyDecoder::operator=(StrategyDecoder&& other) {
    if ((state && state->active) || (other.state && other.state->active))
        throw std::logic_error("An active decoder session cannot be moved.");
    if (this != &other) state = std::move(other.state);
    return *this;
}
StrategyDecoder::State& StrategyDecoder::checked() {
    if (!state) throw std::logic_error("Strategy decoder is moved from.");
    return *state;
}
const StrategyDecoder::State& StrategyDecoder::checked() const {
    if (!state) throw std::logic_error("Strategy decoder is moved from.");
    return *state;
}
const DecoderMetadata& StrategyDecoder::metadata() const { return checked().info; }
MatchingResult StrategyDecoder::decode(const std::vector<uint64_t>& events) {
    auto& s = checked();
    if (s.info.num_observables > 64) throw std::invalid_argument("Packed results support at most 64 observables; use the observable-array overload.");
    return s.packed_entry(s, events);
}
void StrategyDecoder::decode(const std::vector<uint64_t>& events, uint8_t* observables, total_weight_int& weight) {
    auto& s = checked();
    if (!observables && s.info.num_observables) throw std::invalid_argument("Observable output must not be null.");
    s.extended_entry(s, events, observables, weight);
}
void StrategyDecoder::decode_to_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
    auto& s = checked();
    if (!s.info.supports_edges) throw std::invalid_argument("This parallel strategy has no physical-edge export.");
    if (!s.info.search_graph_included) throw std::invalid_argument("Physical-edge export requires ensure_search_graph_included at construction.");
    s.edges_entry(s, events, edges);
}
void StrategyDecoder::decode_to_match_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
    auto& s = checked();
    if (s.info.correlations_enabled)
        throw std::invalid_argument("Compressed matching-pair export does not support correlated decoding.");
    s.matches_entry(s, events, edges);
}
void StrategyDecoder::run_with_team(const std::function<void(StrategyDecoder&)>& callback) {
    auto& s = checked();
    s.session_entry(s, [&] { callback(*this); });
}
std::vector<MatchingResult> StrategyDecoder::decode_batch(std::span<const std::vector<uint64_t>> shots) {
    auto& s = checked();
    if (s.info.num_observables > 64) throw std::invalid_argument("Packed batch results support at most 64 observables.");
    return s.batch_entry(s, shots);
}
void StrategyDecoder::drain() { auto& s = checked(); s.drain_entry(s); }
}  // namespace beyond_blossom
