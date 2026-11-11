// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_DECODER_STRATEGY_H
#define BB_DECODER_STRATEGY_H

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "beyond_blossom/core/matching_result.h"
#include "beyond_blossom/core/grid/grid_spec.h"

// CMake freezes this construction-only Apple ARM tuning parameter. Direct
// library consumers keep the same default unless their build supplies it.
#ifndef BB_M4_SPATIAL_COMPONENT_THRESHOLD
#define BB_M4_SPATIAL_COMPONENT_THRESHOLD 64
#endif

namespace stim { struct DetectorErrorModel; }

namespace beyond_blossom {
class BackendUserGraph;

enum class DecoderStrategy { Unstructured, Structured, Predecoded, ComponentParallel, Pipelined, ParallelPredecode };
enum class DecoderExecution { serial, component_team, double_buffer };
DecoderStrategy parse_decoder_strategy(std::string_view name);
const char* decoder_strategy_name(DecoderStrategy strategy);

struct DecoderOptions {
    DecoderStrategy strategy = DecoderStrategy::Unstructured;
    // Required for structured methods; explicit geometry and DEM coordinates.
    std::optional<StaticGridSpec> grid;
    // Parallel methods require an explicit map of size 2, 4, or 8. On Linux
    // these are OS CPU IDs; on Darwin they are distinct logical slots
    // and the team requests USER_INITIATED QoS without physical CPU pinning.
    // Serial callers retain control of their own thread affinity and QoS.
    std::vector<int> cpus;
    // Explicit scaling control for ComponentParallel, Pipelined and ParallelPredecode only.
    // Requires one CPU/slot and retains the same algorithm, partitions and
    // buffers; pipeline work and reset execute sequentially on that thread.
    bool same_algorithm_single_thread = false;
    bool enable_correlations = false;
    bool ensure_search_graph_included = false;
    weight_int num_distinct_weights = NUM_DISTINCT_WEIGHTS;
};

struct DecoderThreadInfo {
    size_t buffer = 0, component = 0, lane = 0;
    int requested_cpu = -1;
    uint64_t thread_id = 0;
    std::vector<int> affinity;
    // Requested QoS read back at session initialization on Darwin.
    int qos_class = 0, qos_relative_priority = 0;
};

struct DecoderMetadata {
    DecoderStrategy strategy = DecoderStrategy::Unstructured;
    DecoderExecution execution = DecoderExecution::serial;
    bool predecode_enabled = false, correlations_enabled = false, search_graph_included = false;
    bool supports_extended_observables = true, supports_edges = true, supports_match_edges = true;
    size_t num_nodes = 0, num_observables = 0;
    double normalising_constant = 1;
    size_t requested_threads = 1, actual_threads = 0, partitions = 1;
    bool same_algorithm_single_thread = false;
    const char* pipeline_schedule = "none";
    size_t component_threshold = 0, shot_threshold = 0, conflict_width = 0;
    bool parallel_import = false;
    const char* prefix_schedule = "serial";
    const char* conflict_geometry = "none";
    const char* fixed_seed_construction = "none";
    const char* fixed_seed_input = "none";
    const char* import_preparation = "none";
    const char* import_reset_owner = "none";
    const char* seed_ordering = "reconstruct";
    std::optional<StaticGridSpec> grid;
    std::vector<GridSimd> selected_simd;
    // Logical buffer memberships. A single-thread pipeline has two entries
    // with the same actual TID; actual_threads counts actual OS threads.
    std::vector<DecoderThreadInfo> threads;
};

// A construction-bound API over the existing generic and structured decoders.
// Unstructured uses the arbitrary-graph decoder and its certified predecoder;
// Structured uses the shared structured core with predecode disabled.
// This mutable decoder is not reentrant or safe for concurrent callers.
class StrategyDecoder {
    struct State;
    std::unique_ptr<State> state;
    State& checked();
    const State& checked() const;

   public:
    StrategyDecoder(const stim::DetectorErrorModel& dem, const DecoderOptions& options = {});
    StrategyDecoder(BackendUserGraph& graph, const DecoderOptions& options);
    ~StrategyDecoder();
    StrategyDecoder(StrategyDecoder&&);
    StrategyDecoder& operator=(StrategyDecoder&&);
    StrategyDecoder(const StrategyDecoder&) = delete;
    StrategyDecoder& operator=(const StrategyDecoder&) = delete;

    const DecoderMetadata& metadata() const;
    // Packed result, at most 64 observables. Parallel strategies require this
    // call to be inside run_with_team; no per-shot team is created implicitly.
    MatchingResult decode(const std::vector<uint64_t>& events);
    // XORs observables and overwrites weight with this shot's integer solution
    // weight. The initial weight is ignored. Serial methods support arbitrary
    // observable counts; the legacy lower-level interfaces remain unchanged.
    void decode(const std::vector<uint64_t>& events, uint8_t* observables, total_weight_int& weight);
    // Search graph must have been requested in DecoderOptions or included
    // automatically for more than 64 observables. Parallel
    // strategies retain their original lack of physical-edge export.
    void decode_to_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges);
    // Compressed matching pairs; supported by generic, serial, and ComponentParallel.
    // Correlated decoding is unsupported by the ordinary matching-pair API;
    // this method explicitly rejects a decoder configured with correlations.
    // Pipelined/ParallelPredecode retain the original pipeline capability boundary.
    void decode_to_match_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges);
    // Establishes one team over the complete callback, including empty and
    // exceptional streams, and drains asynchronous resets before returning.
    // Serial methods use the same interface without creating worker threads.
    void run_with_team(const std::function<void(StrategyDecoder&)>& callback);
    // Batch dispatch occurs once, outside the typed per-shot loop. This method
    // owns its team and includes its final pipeline drain before returning.
    std::vector<MatchingResult> decode_batch(std::span<const std::vector<uint64_t>> shots);
    // Waits for delayed pipeline resets inside an active session; serial and
    // ComponentParallel decoders already return with their ordinary reset complete.
    void drain();
};
}  // namespace beyond_blossom
#endif
