// Copyright 2026 Beyond Blossom Contributors
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/bindings/structured.pybind.h"

#include <algorithm>
#include <memory>
#include <string>
#include "pybind11/numpy.h"
#include "pybind11/stl.h"
#include "stim.h"
#include "beyond_blossom/core/observable_bits.h"
#include "beyond_blossom/structured/decoder.h"

namespace py = pybind11;
namespace beyond_blossom::bindings {
namespace {
using Decoder = structured::Decoder;
using ByteArray = py::array_t<uint8_t, py::array::c_style | py::array::forcecast>;

GridSimd parse_simd(const std::string& name) {
    if (name == "auto") return GridSimd::automatic;
    if (name == "scalar") return GridSimd::scalar;
    if (name == "avx2") return GridSimd::avx2;
    if (name == "avx512vl") return GridSimd::avx512vl;
    if (name == "avx512") return GridSimd::avx512;
    throw std::invalid_argument("Unknown SIMD mode: " + name);
}
const char* simd_name(GridSimd mode) {
    switch (mode) {
        case GridSimd::automatic: return "auto";
        case GridSimd::scalar: return "scalar";
        case GridSimd::avx2: return "avx2";
        case GridSimd::avx512vl: return "avx512vl";
        case GridSimd::avx512: return "avx512";
    }
    return "unknown";
}
py::dict metadata(const Decoder& decoder) {
    const auto& info = decoder.metadata();
    py::dict result;
    result["strategy"] = decoder_strategy_name(info.strategy);
    result["backend"] = "structured";
    result["num_detectors"] = info.num_nodes;
    result["num_observables"] = info.num_observables;
    result["predecode_enabled"] = info.predecode_enabled;
    result["requested_threads"] = info.requested_threads;
    result["actual_threads"] = info.actual_threads;
    result["partitions"] = info.partitions;
    result["same_algorithm_single_thread"] = info.same_algorithm_single_thread;
    result["pipeline_schedule"] = info.pipeline_schedule;
    result["supports_extended_observables"] = info.supports_extended_observables;
    result["supports_edges"] = info.supports_edges && info.search_graph_included;
    result["supports_match_edges"] = info.supports_match_edges;
    result["normalising_constant"] = info.normalising_constant;
    result["distance"] = info.grid->distance;
    result["rounds"] = info.grid->rounds;
    result["geometry"] = info.grid->rotated ? "rotated" : "unrotated";
    py::list modes;
    for (auto mode : info.selected_simd) modes.append(simd_name(mode));
    result["selected_simd"] = modes;
    return result;
}

py::tuple decode_batch(Decoder& decoder, const ByteArray& shots, bool packed_input, bool packed_output) {
    const auto& info = decoder.metadata();
    const size_t num_nodes = info.num_nodes, num_obs = info.num_observables;
    const size_t input_width = packed_input ? (num_nodes + 7) / 8 : num_nodes;
    const size_t output_width = packed_output ? (num_obs + 7) / 8 : num_obs;
    if (shots.ndim() != 2 || static_cast<size_t>(shots.shape(1)) != input_width)
        throw std::invalid_argument("Shots must be a two-dimensional array with the decoder's detector width.");
    const size_t num_shots = shots.shape(0);
    py::array_t<uint8_t> predictions(std::vector<py::ssize_t>{static_cast<py::ssize_t>(num_shots),
                                                           static_cast<py::ssize_t>(output_width)});
    py::array_t<double> weights(num_shots);
    auto* output = predictions.mutable_data();
    auto* weight_output = weights.mutable_data();
    const auto* input = shots.data();
    std::fill_n(output, num_shots * output_width, uint8_t{0});
    {
        py::gil_scoped_release release;
        std::vector<uint64_t> events;
        std::vector<uint8_t> unpacked(packed_output && num_obs > 64 ? num_obs : 0);
        const auto run = [&]<bool PackedResult>() {
            decoder.run_with_team([&](Decoder& active) {
                for (size_t shot = 0; shot < num_shots; ++shot) {
                    events.clear();
                    const auto* row = input + shot * input_width;
                    for (size_t detector = 0; detector < num_nodes; ++detector) {
                        const uint8_t hit = packed_input ? ((row[detector / 8] >> (detector % 8)) & 1) : row[detector];
                        if (hit > 1) throw std::invalid_argument("Syndrome entries must be 0 or 1.");
                        if (hit) events.push_back(detector);
                    }
                    if (packed_input && num_nodes % 8 && (row[input_width - 1] >> (num_nodes % 8)))
                        throw std::invalid_argument("Packed syndrome padding bits must be zero.");
                    total_weight_int weight = 0;
                    if constexpr (PackedResult) {
                        const auto result = active.decode(events);
                        weight = result.weight;
                        if (packed_output) {
                            for (size_t byte = 0; byte < output_width; ++byte)
                                output[shot * output_width + byte] = static_cast<uint8_t>(result.obs_mask >> (8 * byte));
                        } else {
                            for (size_t obs = 0; obs < num_obs; ++obs)
                                output[shot * output_width + obs] = (result.obs_mask >> obs) & obs_int{1};
                        }
                    } else {
                        auto* observable_output = packed_output ? unpacked.data() : output + shot * output_width;
                        if (packed_output) std::fill(unpacked.begin(), unpacked.end(), uint8_t{0});
                        active.decode(events, observable_output, weight);
                        if (packed_output)
                            for (size_t obs = 0; obs < num_obs; ++obs)
                                output[shot * output_width + obs / 8] |= unpacked[obs] << (obs % 8);
                    }
                    weight_output[shot] = static_cast<double>(weight) / info.normalising_constant;
                }
            });
        };
        // Select the result representation once for the complete shot stream.
        if (num_obs <= 64) run.template operator()<true>();
        else run.template operator()<false>();
    }
    return py::make_tuple(std::move(predictions), std::move(weights));
}

py::tuple decode_sparse(Decoder& decoder, const std::vector<uint64_t>& events) {
    const auto& info = decoder.metadata();
    py::array_t<uint8_t> predictions(info.num_observables);
    auto* output = predictions.mutable_data();
    std::fill_n(output, info.num_observables, uint8_t{0});
    total_weight_int weight = 0;
    {
        py::gil_scoped_release release;
        decoder.run_with_team([&](Decoder& active) {
            if (info.num_observables <= 64) {
                const auto result = active.decode(events);
                weight = result.weight;
                fill_bit_vector_from_obs_mask(result.obs_mask, output, info.num_observables);
            } else {
                active.decode(events, output, weight);
            }
        });
    }
    return py::make_tuple(std::move(predictions), static_cast<double>(weight) / info.normalising_constant);
}

py::array_t<int64_t> decode_edges(Decoder& decoder, const std::vector<uint64_t>& events, bool compressed) {
    std::vector<int64_t> edges;
    {
        py::gil_scoped_release release;
        decoder.run_with_team([&](Decoder& active) {
            if (compressed) active.decode_to_match_edges(events, edges);
            else active.decode_to_edges(events, edges);
        });
    }
    py::array_t<int64_t> result(std::vector<py::ssize_t>{static_cast<py::ssize_t>(edges.size() / 2), 2});
    std::copy(edges.begin(), edges.end(), result.mutable_data());
    return result;
}
}  // namespace

void bind_structured_decoder(py::module_& module) {
    py::class_<Decoder>(module, "_StructuredDecoder")
        .def(py::init([](const std::string& model, size_t distance, size_t rounds, const std::string& geometry,
                         const std::string& strategy, const std::string& simd, std::vector<int> cpus,
                         bool same_algorithm_single_thread, bool include_search_graph) {
            if (geometry != "rotated" && geometry != "unrotated")
                throw std::invalid_argument("Geometry must be 'rotated' or 'unrotated'.");
            structured::Options options;
            options.distance = distance;
            options.rounds = rounds;
            options.rotated = geometry == "rotated";
            options.strategy = parse_decoder_strategy(strategy);
            options.simd = parse_simd(simd);
            options.cpus = std::move(cpus);
            options.same_algorithm_single_thread = same_algorithm_single_thread;
            options.include_search_graph = include_search_graph;
            py::gil_scoped_release release;
            return std::make_unique<Decoder>(stim::DetectorErrorModel(model), options);
        }))
        .def_property_readonly("metadata", &metadata)
        .def("decode_batch", &decode_batch)
        .def("decode_sparse", &decode_sparse)
        .def("decode_edges", &decode_edges);
}
}  // namespace beyond_blossom::bindings
