// Copyright 2022 PyMatching Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pymatching/sparse_blossom/driver/namespaced_main.h"

#include <chrono>
#include <charconv>
#include <cstring>
#include <iostream>
#include <vector>

#include "pymatching/sparse_blossom/diagram/animation_main.h"
#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/driver/backend_user_graph.h"
#include "pymatching/sparse_blossom/driver/decoder_strategy.h"
#include "pymatching/sparse_blossom/driver/user_graph.h"
#include "stim.h"

namespace {
size_t positive_grid_argument(const char* name, int argc, const char** argv) {
    const char* argument = stim::find_argument(name, argc, argv);
    if (!argument) throw std::invalid_argument(std::string("Structured decoding requires ") + name + ".");
    size_t value = 0;
    const auto end = argument + std::strlen(argument);
    const auto parsed = std::from_chars(argument, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0)
        throw std::invalid_argument(std::string(name) + " must be a positive integer.");
    return value;
}

pm::DecoderOptions decoder_options_from_cli(const stim::DetectorErrorModel& dem, int argc, const char** argv) {
    pm::DecoderOptions options;
    if (const char* strategy = stim::find_argument("--decoder-strategy", argc, argv))
        options.strategy = pm::parse_decoder_strategy(strategy);
    options.enable_correlations = stim::find_bool_argument("--enable_correlations", argc, argv);
    if (const char* text = stim::find_argument("--decoder-cpus", argc, argv)) {
        std::string_view cpus(text);
        if (cpus.empty()) throw std::invalid_argument("--decoder-cpus must be a comma-separated list of CPU IDs.");
        while (!cpus.empty()) {
            const auto comma = cpus.find(',');
            const auto token = cpus.substr(0, comma);
            int value = -1;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value < 0)
                throw std::invalid_argument("--decoder-cpus requires nonnegative integer CPU IDs.");
            options.cpus.push_back(value);
            if (comma == std::string_view::npos) break;
            cpus.remove_prefix(comma + 1);
            if (cpus.empty()) throw std::invalid_argument("--decoder-cpus must not end with a comma.");
        }
    }
    if (options.strategy == pm::DecoderStrategy::Upstream) {
        for (const char* name : {"--grid-distance", "--grid-rounds", "--grid-geometry", "--grid-simd"})
            if (stim::find_argument(name, argc, argv))
                throw std::invalid_argument("Upstream does not use grid parameters; select a structured strategy explicitly.");
        return options;
    }
    const size_t distance = positive_grid_argument("--grid-distance", argc, argv);
    const size_t rounds = positive_grid_argument("--grid-rounds", argc, argv);
    const char* geometry = stim::find_argument("--grid-geometry", argc, argv);
    if (!geometry || (std::strcmp(geometry, "rotated") && std::strcmp(geometry, "unrotated")))
        throw std::invalid_argument("Structured decoding requires --grid-geometry rotated|unrotated.");
    pm::GridSimd simd = pm::GridSimd::automatic;
    if (const char* value = stim::find_argument("--grid-simd", argc, argv)) {
        const std::string_view name(value);
        if (name == "auto") simd = pm::GridSimd::automatic;
        else if (name == "scalar") simd = pm::GridSimd::scalar;
        else if (name == "avx2") simd = pm::GridSimd::avx2;
        else if (name == "avx512") simd = pm::GridSimd::avx512;
        else if (name == "avx512vl") simd = pm::GridSimd::avx512vl;
        else throw std::invalid_argument("--grid-simd must be auto, scalar, avx2, avx512, or avx512vl.");
    }
    options.grid = pm::static_grid_spec_from_dem(dem, distance, rounds, !std::strcmp(geometry, "rotated"), simd);
    return options;
}

void print_decoder_info(const pm::DecoderMetadata& info) {
    std::cerr << "{\"decoder_strategy\":\"" << pm::decoder_strategy_name(info.strategy)
              << "\",\"predecode_enabled\":" << (info.predecode_enabled ? "true" : "false")
              << ",\"requested_threads\":" << info.requested_threads << ",\"actual_threads\":" << info.actual_threads
              << ",\"partitions\":" << info.partitions << ",\"component_threshold\":" << info.component_threshold
              << ",\"shot_threshold\":" << info.shot_threshold << ",\"conflict_width\":" << info.conflict_width
              << ",\"prefix_schedule\":\"" << info.prefix_schedule << "\",\"conflict_geometry\":\"" << info.conflict_geometry
              << "\",\"fixed_seed_construction\":\"" << info.fixed_seed_construction
              << "\",\"fixed_seed_input\":\"" << info.fixed_seed_input << "\",\"import_preparation\":\"" << info.import_preparation
              << "\",\"import_reset_owner\":\"" << info.import_reset_owner << "\",\"seed_ordering\":\"" << info.seed_ordering << '"';
    if (info.grid)
        std::cerr << ",\"grid_distance\":" << info.grid->distance << ",\"grid_rounds\":" << info.grid->rounds
                  << ",\"grid_geometry\":\"" << (info.grid->rotated ? "rotated" : "unrotated") << '"';
    std::cerr << ",\"selected_simd\":[";
    for (size_t k = 0; k < info.selected_simd.size(); ++k) {
        if (k) std::cerr << ',';
        switch (info.selected_simd[k]) {
            case pm::GridSimd::automatic: std::cerr << "\"auto\""; break;
            case pm::GridSimd::scalar: std::cerr << "\"scalar\""; break;
            case pm::GridSimd::avx2: std::cerr << "\"avx2\""; break;
            case pm::GridSimd::avx512: std::cerr << "\"avx512\""; break;
            case pm::GridSimd::avx512vl: std::cerr << "\"avx512vl\""; break;
        }
    }
    std::cerr << "],\"threads\":[";
    for (size_t k = 0; k < info.threads.size(); ++k) {
        const auto& thread = info.threads[k];
        if (k) std::cerr << ',';
        std::cerr << "{\"buffer\":" << thread.buffer << ",\"component\":" << thread.component
                  << ",\"lane\":" << thread.lane << ",\"requested_cpu\":" << thread.requested_cpu
                  << ",\"tid\":" << thread.thread_id
                  << ",\"qos_class\":" << thread.qos_class
                  << ",\"qos_relative_priority\":" << thread.qos_relative_priority
                  << ",\"affinity\":[";
        for (size_t j = 0; j < thread.affinity.size(); ++j) {
            if (j) std::cerr << ',';
            std::cerr << thread.affinity[j];
        }
        std::cerr << "]}";
    }
    std::cerr << "]}\n";
}
}  // namespace

int main_predict(int argc, const char **argv) {
    stim::check_for_unknown_arguments(
        {
            "--in",
            "--in_format",
            "--in_includes_appended_observables",
            "--out",
            "--out_format",
            "--dem",
            "--enable_correlations",
            "--decoder-strategy", "--decoder-cpus", "--decoder-info",
            "--grid-distance", "--grid-rounds", "--grid-geometry", "--grid-simd",
        },
        {},
        "predict",
        argc,
        argv);

    FILE *shots_in = stim::find_open_file_argument("--in", stdin, "rb", argc, argv);
    FILE *predictions_out = stim::find_open_file_argument("--out", stdout, "wb", argc, argv);
    FILE *dem_file = stim::find_open_file_argument("--dem", nullptr, "r", argc, argv);
    stim::FileFormatData shots_in_format =
        stim::find_enum_argument("--in_format", "b8", stim::format_name_to_enum_map(), argc, argv);
    stim::FileFormatData predictions_out_format =
        stim::find_enum_argument("--out_format", "01", stim::format_name_to_enum_map(), argc, argv);
    bool append_obs = stim::find_bool_argument("--in_includes_appended_observables", argc, argv);

    stim::DetectorErrorModel dem = stim::DetectorErrorModel::from_file(dem_file);
    fclose(dem_file);

    size_t num_obs = dem.count_observables();
    auto reader = stim::MeasureRecordReader<stim::MAX_BITWORD_WIDTH>::make(
        shots_in, shots_in_format.id, 0, dem.count_detectors(), append_obs * num_obs);
    auto writer = stim::MeasureRecordWriter::make(predictions_out, predictions_out_format.id);
    writer->begin_result_type('L');

    pm::StrategyDecoder decoder(dem, decoder_options_from_cli(dem, argc, argv));

    stim::SparseShot sparse_shot;
    sparse_shot.clear();
    pm::ExtendedMatchingResult res(decoder.metadata().num_observables);
    decoder.run_with_team([&](pm::StrategyDecoder& active) {
        while (reader->start_and_read_entire_record(sparse_shot)) {
            active.decode(sparse_shot.hits, res.obs_crossed.data(), res.weight);
            for (size_t k = 0; k < num_obs; k++) {
                writer->write_bit(res.obs_crossed[k]);
            }
            writer->write_end();
            sparse_shot.clear();
            res.reset();
        }
    });
    if (stim::find_bool_argument("--decoder-info", argc, argv)) print_decoder_info(decoder.metadata());
    if (predictions_out != stdout) {
        fclose(predictions_out);
    }
    if (shots_in != stdin) {
        fclose(shots_in);
    }

    return EXIT_SUCCESS;
}

int main_count_mistakes(int argc, const char **argv) {
    stim::check_for_unknown_arguments(
        {
            "--in",
            "--in_format",
            "--in_includes_appended_observables",
            "--obs_in",
            "--obs_in_format",
            "--out",
            "--dem",
            "--time",
            "--enable_correlations",
            "--decoder-strategy", "--decoder-cpus", "--decoder-info",
            "--grid-distance", "--grid-rounds", "--grid-geometry", "--grid-simd",
        },
        {},
        "count_mistakes",
        argc,
        argv);

    FILE *shots_in = stim::find_open_file_argument("--in", stdin, "rb", argc, argv);
    FILE *obs_in = stim::find_open_file_argument("--obs_in", stdin, "rb", argc, argv);
    FILE *stats_out = stim::find_open_file_argument("--out", stdout, "wb", argc, argv);
    FILE *dem_file = stim::find_open_file_argument("--dem", nullptr, "r", argc, argv);
    stim::FileFormatData shots_in_format =
        stim::find_enum_argument("--in_format", "01", stim::format_name_to_enum_map(), argc, argv);
    stim::FileFormatData obs_in_format =
        stim::find_enum_argument("--obs_in_format", "01", stim::format_name_to_enum_map(), argc, argv);
    bool append_obs = stim::find_bool_argument("--in_includes_appended_observables", argc, argv);

    bool time = stim::find_bool_argument("--time", argc, argv);
    if (!append_obs && obs_in == nullptr) {
        throw std::invalid_argument("Must specify --in_includes_appended_observables or --obs_in.");
    }

    stim::DetectorErrorModel dem = stim::DetectorErrorModel::from_file(dem_file);
    fclose(dem_file);

    size_t num_obs = dem.count_observables();
    std::unique_ptr<stim::MeasureRecordReader<stim::MAX_BITWORD_WIDTH>> obs_reader;
    if (obs_in != stdin) {
        obs_reader = stim::MeasureRecordReader<stim::MAX_BITWORD_WIDTH>::make(obs_in, obs_in_format.id, 0, 0, num_obs);
    }
    auto reader = stim::MeasureRecordReader<stim::MAX_BITWORD_WIDTH>::make(
        shots_in, shots_in_format.id, 0, dem.count_detectors(), append_obs * num_obs);

    pm::StrategyDecoder decoder(dem, decoder_options_from_cli(dem, argc, argv));
    if (decoder.metadata().num_observables > 64)
        throw std::invalid_argument("count_mistakes supports at most 64 observables.");

    stim::SparseShot sparse_shot;
    stim::SparseShot obs_shot;
    size_t num_mistakes = 0;
    size_t num_shots = 0;
    auto start = std::chrono::steady_clock::now();
    decoder.run_with_team([&](pm::StrategyDecoder& active) {
        while (reader->start_and_read_entire_record(sparse_shot)) {
            if (obs_reader == nullptr) {
                obs_shot.obs_mask = sparse_shot.obs_mask;
            } else {
                if (!obs_reader->start_and_read_entire_record(obs_shot)) {
                    throw std::invalid_argument("Obs data ended before shot data ended.");
                }
            }
            auto res = active.decode(sparse_shot.hits);
            if (obs_shot.obs_mask_as_u64() != res.obs_mask) {
                num_mistakes++;
            }
            sparse_shot.clear();
            obs_shot.clear();
            num_shots++;
        }
    });
    if (stim::find_bool_argument("--decoder-info", argc, argv)) print_decoder_info(decoder.metadata());
    fprintf(stats_out, "%zu / %zu\n", num_mistakes, num_shots);
    if (stats_out != stdout) {
        fclose(stats_out);
    }
    if (shots_in != stdin) {
        fclose(shots_in);
    }

    auto end = std::chrono::steady_clock::now();
    auto microseconds = (double)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    if (time) {
        std::cerr << "Total decoding time: " << (int)microseconds << "us\n";
        std::cerr << "Decoding time per shot: " << (microseconds / num_shots) << "us\n";
    }

    return EXIT_SUCCESS;
}

int pm::main(int argc, const char **argv) {
    const char *command = "";
    if (argc >= 2) {
        command = argv[1];
    }
    try {
        if (strcmp(command, "predict") == 0) {
            return main_predict(argc, argv);
        }
        if (strcmp(command, "count_mistakes") == 0) {
            return main_count_mistakes(argc, argv);
        }
        if (strcmp(command, "animate") == 0) {
            return pm::main_animation(argc, argv);
        }
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    std::stringstream ss;
    ss << "Unrecognized command. Available commands are:\n";
    ss << "    pymatching predict --dem file [--in file] [--out file] [--in_format 01|b8|...] [--out_format 01|b8|...] "
          "[--in_includes_appended_observables]\n";
    ss << "    pymatching count_mistakes --dem file [--in file] [--out file] [--in_format 01|b8|...] [--out_format "
          "01|B8|...] [--in_includes_appended_observables] [--obs_in] [--obs_in_format]\n";
    ss << "    Decoder options for predict/count_mistakes: [--decoder-strategy upstream|gridsimd|predecode|component|tailreset|spatial] "
          "[--decoder-info]. Structured strategies require --grid-distance N --grid-rounds N --grid-geometry rotated|unrotated "
          "[--grid-simd auto|scalar|avx2|avx512|avx512vl]. Parallel strategies require --decoder-cpus CPU,CPU,... "
          "(2/4/8 CPUs); one persistent team covers the complete input stream and its final drain.\n";
    ss << "    pymatching animate "
          "--dets_in <file> "
          "--dets_in_format 01|b8|... "
          "--out_dir <directory> "
          "--dem_in <file> "
          "--held_frames_per_event # "
          "--held_frames_at_start # "
          "--held_frames_at_end # "
          "--max_growth_between_frames # "
          "--max_edge_weight # "
          "--pixels_per_unit_length # "
          "[--dets_in_includes_appended_observables] "
          "[--quiet]";
    throw std::invalid_argument(ss.str());
}
