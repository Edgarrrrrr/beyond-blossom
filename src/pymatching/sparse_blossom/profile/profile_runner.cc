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

#include "pymatching/sparse_blossom/profile/profile_runner.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"
#include "pymatching/sparse_blossom/driver/user_graph.h"
#include "pymatching/sparse_blossom/driver/backend.h"
#include "pymatching/sparse_blossom/profile/profile.h"
#include "pymatching/sparse_blossom/profile/profile_io.h"
#include "pymatching/sparse_blossom/profile/profile_schema.h"
#include "stim.h"

namespace pm::profile {
namespace {

enum class ProfileMode {
    trace,
    stats,
    baseline,
    both,
};

enum class StatsGranularity {
    setting,
    shot,
};

enum class ShotMeasurementMode {
    auto_size,
    on,
    off,
};

enum class TailShotCaptureMode {
    off,
    on,
};

struct Options {
    std::string graph_backend = "generic";
    std::string grid_simd = "auto";
    std::vector<size_t> distances{5, 7, 9, 11, 13, 15, 17, 19, 23, 29, 39, 49};
    std::vector<double> error_rates{0.0001, 0.001, 0.003, 0.005, 0.01, 0.03, 0.05};
    std::vector<std::pair<size_t, double>> cases;
    std::vector<std::string> round_specs{"d"};
    size_t shots = 1000;
    uint64_t seed = 42;
    TraceLevel trace_level = TraceLevel::event;
    ProfileMode profile_mode = ProfileMode::stats;
    StatsGranularity stats_granularity = StatsGranularity::setting;
    ShotMeasurementMode shot_measurement_mode = ShotMeasurementMode::auto_size;
    TailShotCaptureMode tail_shot_capture_mode = TailShotCaptureMode::off;
    std::filesystem::path replay_shots_csv;
    size_t shot_measurement_auto_limit = 1000000;
    size_t shot_aggregate_memory_limit = 1000000;
    size_t shot_batch_size = 100000;
    size_t shot_generation_chunk_size = 1024;
    size_t latency_histogram_buckets_per_decade = 120;
    std::vector<double> tail_shot_k_values{2.0, 1.0, 0.1, 0.01};
    size_t tail_shots_per_interval = 100;
    size_t baseline_timing_repeats = 10;
    size_t baseline_timing_warmups = 1;
    double progress_interval_seconds = 5;
    std::filesystem::path output_dir = "sparse_blossom_profile_output";
    size_t max_settings = SIZE_MAX;
};

struct ShotMeasurement {
    double decode_us = 0;
    double decode_us_first = 0;
    double decode_us_min = 0;
    double decode_us_mean = 0;
    double decode_us_max = 0;
    size_t timing_repeats = 1;
    size_t timing_warmups = 0;
    uint64_t detection_events = 0;
    bool mistake = false;
    obs_int predicted_obs_mask = 0;
};

struct OutputWriters {
    std::ofstream settings;
    std::ofstream runs;
    std::ofstream shot_measurements;
    std::ofstream latency_histograms;
    std::ofstream latency_quantiles;
    std::ofstream latency_tail_cutoffs;
    std::ofstream tail_shots;
    std::ofstream progress;
    std::ofstream counters;
    std::ofstream histograms;
    std::ofstream aggregates;
    std::ofstream histogram_quantiles;
};

struct ReplayShotRow {
    size_t distance = 0;
    double error_rate = 0;
    std::string round_spec;
    size_t rounds = 0;
    size_t source_shots = 0;
    bool has_source_shots = false;
    uint64_t setting_seed = 0;
    bool has_setting_seed = false;
    size_t shot_generation_chunk_size = 0;
    bool has_shot_generation_chunk_size = false;
    size_t global_chunk_start = 0;
    bool has_global_chunk_start = false;
    uint64_t chunk_seed = 0;
    bool has_chunk_seed = false;
    size_t chunk_local_index = 0;
    bool has_chunk_local_index = false;
    size_t source_shot_index = 0;
    bool has_source_shot_index = false;
    obs_int obs_mask = 0;
    bool has_obs_mask = false;
    bool has_detector_hits_payload = false;
    std::vector<uint64_t> detector_hits;
};

struct TailShotRecord {
    size_t shot_index = 0;
    uint64_t setting_seed = 0;
    size_t shot_generation_chunk_size = 0;
    size_t global_chunk_start = 0;
    uint64_t chunk_seed = 0;
    size_t chunk_local_index = 0;
    double decode_us = 0;
    double decode_us_per_round = 0;
    double decode_us_first = 0;
    double decode_us_min = 0;
    double decode_us_mean = 0;
    double decode_us_max = 0;
    size_t timing_repeats = 1;
    size_t timing_warmups = 0;
    uint64_t detection_events = 0;
    bool mistake = false;
    obs_int obs_mask = 0;
    obs_int predicted_obs_mask = 0;
    std::string detector_hits;
};

struct LatencyMetricSummary {
    size_t buckets_per_decade = 120;
    std::map<int64_t, uint64_t> counts;
    uint64_t zero_count = 0;
    uint64_t total = 0;
    double sum = 0;
    double max_value = 0;

    explicit LatencyMetricSummary(size_t buckets_per_decade = 120)
        : buckets_per_decade(std::max<size_t>(1, buckets_per_decade)) {
    }

    int64_t bucket_for(double value) const {
        return (int64_t)std::floor(std::log10(value) * (double)buckets_per_decade);
    }

    double bucket_start(int64_t bucket) const {
        return std::pow(10.0, (double)bucket / (double)buckets_per_decade);
    }

    double bucket_end(int64_t bucket) const {
        return std::pow(10.0, (double)(bucket + 1) / (double)buckets_per_decade);
    }

    void add(double value) {
        if (!std::isfinite(value) || value < 0) {
            return;
        }
        if (value == 0) {
            zero_count++;
        } else {
            counts[bucket_for(value)]++;
        }
        total++;
        sum += value;
        max_value = std::max(max_value, value);
    }

    double mean() const {
        return total == 0 ? 0 : sum / (double)total;
    }

    double quantile(double probability) const {
        if (total == 0) {
            return 0;
        }
        uint64_t rank = (uint64_t)std::ceil(probability * (double)total);
        rank = std::max<uint64_t>(1, std::min<uint64_t>(rank, total));
        uint64_t cumulative = zero_count;
        if (cumulative >= rank) {
            return 0;
        }
        for (const auto &[bucket, count] : counts) {
            cumulative += count;
            if (cumulative >= rank) {
                return bucket_end(bucket);
            }
        }
        return max_value;
    }

    double tail_cutoff(double target_tail_probability, double &empirical_tail_probability) const {
        empirical_tail_probability = 0;
        if (total == 0 || !(target_tail_probability > 0) || target_tail_probability > 1) {
            return 0;
        }
        uint64_t rank = (uint64_t)std::ceil(target_tail_probability * (double)total);
        rank = std::max<uint64_t>(1, std::min<uint64_t>(rank, total));
        uint64_t cumulative = 0;
        for (auto iter = counts.rbegin(); iter != counts.rend(); iter++) {
            cumulative += iter->second;
            if (cumulative >= rank) {
                empirical_tail_probability = (double)cumulative / (double)total;
                return bucket_start(iter->first);
            }
        }
        if (zero_count > 0) {
            cumulative += zero_count;
            if (cumulative >= rank) {
                empirical_tail_probability = (double)cumulative / (double)total;
            }
        }
        return 0;
    }
};

struct LatencySummary {
    LatencyMetricSummary decode_us;
    LatencyMetricSummary decode_us_per_round;

    explicit LatencySummary(size_t buckets_per_decade = 120)
        : decode_us(buckets_per_decade), decode_us_per_round(buckets_per_decade) {
    }

    void add(double decode_us_value, size_t rounds) {
        decode_us.add(decode_us_value);
        decode_us_per_round.add(rounds == 0 ? 0 : decode_us_value / (double)rounds);
    }
};

class TailShotBucketRecorder {
   public:
    TailShotBucketRecorder(size_t buckets_per_decade, size_t per_bucket_limit)
        : summary_for_buckets(buckets_per_decade), per_bucket_limit(per_bucket_limit) {
    }

    void add(const TailShotRecord &record) {
        if (per_bucket_limit == 0 || !std::isfinite(record.decode_us_per_round) || record.decode_us_per_round <= 0) {
            return;
        }
        auto &bucket = records_by_bucket[summary_for_buckets.bucket_for(record.decode_us_per_round)];
        bucket.push_back(record);
        sort_and_trim(bucket);
    }

    void merge_from(const TailShotBucketRecorder &other) {
        for (const auto &[bucket_index, records] : other.records_by_bucket) {
            auto &bucket = records_by_bucket[bucket_index];
            bucket.insert(bucket.end(), records.begin(), records.end());
            sort_and_trim(bucket);
        }
    }

    std::vector<TailShotRecord> records_in_interval(double start_cutoff, std::optional<double> end_cutoff) const {
        std::vector<TailShotRecord> result;
        for (const auto &[_, records] : records_by_bucket) {
            for (const auto &record : records) {
                if (record.decode_us_per_round < start_cutoff) {
                    continue;
                }
                if (end_cutoff.has_value() && record.decode_us_per_round >= *end_cutoff) {
                    continue;
                }
                result.push_back(record);
            }
        }
        sort_and_trim(result);
        return result;
    }

   private:
    LatencyMetricSummary summary_for_buckets;
    size_t per_bucket_limit;
    std::map<int64_t, std::vector<TailShotRecord>> records_by_bucket;

    void sort_and_trim(std::vector<TailShotRecord> &records) const {
        std::sort(records.begin(), records.end(), [](const TailShotRecord &a, const TailShotRecord &b) {
            if (a.decode_us_per_round != b.decode_us_per_round) {
                return a.decode_us_per_round > b.decode_us_per_round;
            }
            return a.shot_index < b.shot_index;
        });
        if (records.size() > per_bucket_limit) {
            records.resize(per_bucket_limit);
        }
    }
};

std::string profile_mode_name(ProfileMode mode) {
    switch (mode) {
        case ProfileMode::trace:
            return "trace";
        case ProfileMode::stats:
            return "stats";
        case ProfileMode::baseline:
            return "baseline";
        case ProfileMode::both:
            return "both";
    }
    return "unknown";
}

ProfileMode profile_mode_from_name(const std::string &name) {
    if (name == "trace") {
        return ProfileMode::trace;
    }
    if (name == "stats") {
        return ProfileMode::stats;
    }
    if (name == "baseline" || name == "none") {
        return ProfileMode::baseline;
    }
    if (name == "both") {
        return ProfileMode::both;
    }
    throw std::invalid_argument("Unrecognized profile mode: " + name);
}

std::string stats_granularity_name(StatsGranularity granularity) {
    switch (granularity) {
        case StatsGranularity::setting:
            return "setting";
        case StatsGranularity::shot:
            return "shot";
    }
    return "unknown";
}

StatsGranularity stats_granularity_from_name(const std::string &name) {
    if (name == "setting") {
        return StatsGranularity::setting;
    }
    if (name == "shot") {
        return StatsGranularity::shot;
    }
    throw std::invalid_argument("Unrecognized stats granularity: " + name);
}

std::string shot_measurement_mode_name(ShotMeasurementMode mode) {
    switch (mode) {
        case ShotMeasurementMode::auto_size:
            return "auto";
        case ShotMeasurementMode::on:
            return "on";
        case ShotMeasurementMode::off:
            return "off";
    }
    return "unknown";
}

ShotMeasurementMode shot_measurement_mode_from_name(const std::string &name) {
    if (name == "auto") {
        return ShotMeasurementMode::auto_size;
    }
    if (name == "on") {
        return ShotMeasurementMode::on;
    }
    if (name == "off") {
        return ShotMeasurementMode::off;
    }
    throw std::invalid_argument("Unrecognized shot measurement mode: " + name);
}

std::string tail_shot_capture_mode_name(TailShotCaptureMode mode) {
    switch (mode) {
        case TailShotCaptureMode::off:
            return "off";
        case TailShotCaptureMode::on:
            return "on";
    }
    return "unknown";
}

TailShotCaptureMode tail_shot_capture_mode_from_name(const std::string &name) {
    if (name == "off") {
        return TailShotCaptureMode::off;
    }
    if (name == "on") {
        return TailShotCaptureMode::on;
    }
    throw std::invalid_argument("Unrecognized tail shot capture mode: " + name);
}

bool collects_stats(ProfileMode mode) {
    return mode == ProfileMode::stats || mode == ProfileMode::both;
}

bool binary_supports_profile_stats() {
#if PYMATCHING_ENABLE_PROFILE
    return true;
#else
    return false;
#endif
}

bool binary_supports_nvtx_trace() {
#if PYMATCHING_ENABLE_NVTX
    return true;
#else
    return false;
#endif
}

bool supports_clean_latency_outputs(ProfileMode mode) {
    return mode == ProfileMode::baseline;
}

bool writes_run_outputs(ProfileMode mode) {
    return mode == ProfileMode::trace || mode == ProfileMode::stats || mode == ProfileMode::baseline ||
           mode == ProfileMode::both;
}

bool writes_shot_measurements(const Options &options) {
    if (!writes_run_outputs(options.profile_mode)) {
        return false;
    }
    if (!supports_clean_latency_outputs(options.profile_mode)) {
        return false;
    }
    if (options.shot_measurement_mode == ShotMeasurementMode::on) {
        return true;
    }
    if (options.shot_measurement_mode == ShotMeasurementMode::off) {
        return false;
    }
    return options.shots <= options.shot_measurement_auto_limit;
}

bool writes_tail_shots(const Options &options) {
    return writes_run_outputs(options.profile_mode) && supports_clean_latency_outputs(options.profile_mode) &&
           options.tail_shot_capture_mode == TailShotCaptureMode::on && options.tail_shots_per_interval > 0;
}

bool keeps_shot_aggregates_in_memory(const Options &options) {
    return collects_stats(options.profile_mode) && options.shots <= options.shot_aggregate_memory_limit;
}

TraceLevel effective_trace_level(const Options &options) {
    if (options.profile_mode == ProfileMode::stats || options.profile_mode == ProfileMode::baseline) {
        return TraceLevel::off;
    }
    return options.trace_level;
}

bool activates_profile_session(const Options &options) {
    return collects_stats(options.profile_mode) || effective_trace_level(options) != TraceLevel::off;
}

bool openmp_available() {
#ifdef _OPENMP
    return true;
#else
    return false;
#endif
}

size_t openmp_max_threads() {
#ifdef _OPENMP
    return (size_t)std::max(1, omp_get_max_threads());
#else
    return 1;
#endif
}

bool can_parallelize_shots(const Options &options, size_t shot_count) {
    return openmp_available() && shot_count > 1 && effective_trace_level(options) == TraceLevel::off;
}

std::vector<std::string> split_any(const std::string &text, std::string_view delimiters) {
    std::vector<std::string> result;
    size_t start = 0;
    for (size_t k = 0; k <= text.size(); k++) {
        bool at_end = k == text.size();
        bool at_delimiter = !at_end && delimiters.find(text[k]) != std::string_view::npos;
        if (at_end || at_delimiter) {
            if (k > start) {
                result.push_back(text.substr(start, k - start));
            }
            start = k + 1;
        }
    }
    return result;
}

std::vector<std::string> parse_csv_line(const std::string &line) {
    std::vector<std::string> result;
    std::string field;
    bool quoted = false;
    for (size_t k = 0; k < line.size(); k++) {
        char c = line[k];
        if (quoted) {
            if (c == '"') {
                if (k + 1 < line.size() && line[k + 1] == '"') {
                    field.push_back('"');
                    k++;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            result.push_back(field);
            field.clear();
        } else if (c != '\r') {
            field.push_back(c);
        }
    }
    result.push_back(field);
    return result;
}

std::map<std::string, std::string> csv_row_map(
    const std::vector<std::string> &header, const std::vector<std::string> &fields) {
    std::map<std::string, std::string> result;
    for (size_t k = 0; k < header.size(); k++) {
        result[header[k]] = k < fields.size() ? fields[k] : "";
    }
    return result;
}

std::string row_value(const std::map<std::string, std::string> &row, std::string_view key) {
    auto iter = row.find(std::string(key));
    return iter == row.end() ? "" : iter->second;
}

bool has_row_value(const std::map<std::string, std::string> &row, std::string_view key) {
    auto iter = row.find(std::string(key));
    return iter != row.end() && !iter->second.empty();
}

uint64_t parse_u64_value(const std::string &text, std::string_view label) {
    if (text.empty() || text[0] == '-' || text[0] == '+') {
        throw std::invalid_argument("Invalid unsigned integer for " + std::string(label) + ": " + text);
    }
    size_t consumed = 0;
    uint64_t value = std::stoull(text, &consumed);
    if (consumed != text.size()) {
        throw std::invalid_argument("Invalid unsigned integer for " + std::string(label) + ": " + text);
    }
    return value;
}

size_t parse_size_value(const std::string &text, std::string_view label);
double parse_probability_value(const std::string &text, std::string_view label);

uint64_t parse_optional_u64_value(
    const std::map<std::string, std::string> &row,
    std::string_view key,
    uint64_t fallback = 0) {
    auto value = row_value(row, key);
    return value.empty() ? fallback : parse_u64_value(value, key);
}

size_t parse_optional_size_value(
    const std::map<std::string, std::string> &row,
    std::string_view key,
    size_t fallback = 0) {
    auto value = row_value(row, key);
    return value.empty() ? fallback : parse_size_value(value, key);
}

double parse_optional_probability_value(
    const std::map<std::string, std::string> &row,
    std::string_view key,
    double fallback = 0) {
    auto value = row_value(row, key);
    return value.empty() ? fallback : parse_probability_value(value, key);
}

std::vector<uint64_t> parse_detector_hits_payload(const std::string &text) {
    std::vector<uint64_t> result;
    for (const auto &part : split_any(text, " ;")) {
        result.push_back(parse_u64_value(part, "detector_hits"));
    }
    return result;
}

size_t parse_size_value(const std::string &text, std::string_view label) {
    uint64_t value = parse_u64_value(text, label);
    if (value > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument("Unsigned integer is too large for " + std::string(label) + ": " + text);
    }
    return (size_t)value;
}

double parse_probability_value(const std::string &text, std::string_view label) {
    if (text.empty() || text[0] == '-' || text[0] == '+') {
        throw std::invalid_argument("Invalid probability for " + std::string(label) + ": " + text);
    }
    size_t consumed = 0;
    double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < 0 || value > 1) {
        throw std::invalid_argument("Invalid probability for " + std::string(label) + ": " + text);
    }
    return value;
}

double parse_nonnegative_double_value(const std::string &text, std::string_view label) {
    if (text.empty() || text[0] == '-') {
        throw std::invalid_argument("Invalid nonnegative value for " + std::string(label) + ": " + text);
    }
    size_t consumed = 0;
    double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < 0) {
        throw std::invalid_argument("Invalid nonnegative value for " + std::string(label) + ": " + text);
    }
    return value;
}

std::vector<size_t> parse_size_list(const std::string &text, std::string_view label) {
    std::vector<size_t> result;
    for (const auto &part : split_any(text, ",")) {
        result.push_back(parse_size_value(part, label));
    }
    return result;
}

std::vector<double> parse_probability_list(const std::string &text, std::string_view label) {
    std::vector<double> result;
    for (const auto &part : split_any(text, ",")) {
        result.push_back(parse_probability_value(part, label));
    }
    return result;
}

std::vector<double> parse_nonnegative_double_list(const std::string &text, std::string_view label) {
    std::vector<double> result;
    for (const auto &part : split_any(text, ",")) {
        result.push_back(parse_nonnegative_double_value(part, label));
    }
    return result;
}

std::string detector_hits_payload(const stim::SparseShot &shot) {
    std::ostringstream out;
    for (size_t k = 0; k < shot.hits.size(); k++) {
        if (k) {
            out << " ";
        }
        out << shot.hits[k];
    }
    return out.str();
}

std::vector<std::pair<size_t, double>> parse_case_list(const std::string &text) {
    std::vector<std::pair<size_t, double>> result;
    for (const auto &part : split_any(text, ", \t\n")) {
        auto separator = part.find(':');
        if (separator == std::string::npos) {
            throw std::invalid_argument("Profile case must be distance:error_rate, got: " + part);
        }
        result.emplace_back(
            parse_size_value(part.substr(0, separator), "case distance"),
            parse_probability_value(part.substr(separator + 1), "case error rate"));
    }
    return result;
}

size_t scaled_count_for_spec(const std::string &spec, size_t distance) {
    if (!spec.empty() && spec.back() == 'd') {
        std::string multiplier = spec.substr(0, spec.size() - 1);
        if (multiplier.empty()) {
            return distance;
        }
        size_t value = parse_size_value(multiplier, "round multiplier");
        if (distance != 0 && value > std::numeric_limits<size_t>::max() / distance) {
            throw std::invalid_argument("Round spec is too large: " + spec);
        }
        return value * distance;
    }
    return parse_size_value(spec, "rounds");
}

std::optional<std::string> argument_value(
    const std::string &arg, const char *name, int &index, int argc, const char **argv) {
    std::string option_name(name);
    std::string prefix = option_name + "=";
    if (arg == option_name) {
        if (index + 1 >= argc) {
            throw std::invalid_argument("Missing value for " + option_name);
        }
        return argv[++index];
    }
    if (arg.rfind(prefix, 0) == 0) {
        return arg.substr(prefix.size());
    }
    return std::nullopt;
}

void print_usage_and_exit() {
    std::cout << "Usage: sparse_blossom_profile [options]\n"
              << "  --build-info\n"
              << "  --distances 5,7,9,11,13,15,17,19,23,29,39,49\n"
              << "  --error-rates 0.0001,0.001,0.003,0.005,0.01,0.03,0.05\n"
              << "  --cases 49:0.001,19:0.003\n"
              << "  --rounds d,3d,100,1000\n"
              << "  --shots 1000\n"
              << "  --seed 42\n"
              << "  --trace-level off|phase|event|queue|all\n"
              << "  --profile-mode trace|stats|baseline|both\n"
              << "  --stats-granularity setting|shot\n"
              << "  --shot-measurements auto|on|off\n"
              << "  --tail-shot-capture off|on\n"
              << "  --tail-shot-k-values 2,1,0.1,0.01\n"
              << "  --tail-shots-per-interval 100\n"
              << "  --replay-shots-csv path/to/selected_tail_shots.csv\n"
              << "  --graph-backend generic|static_grid\n"
              << "  --grid-simd auto|scalar|avx512\n"
              << "  --baseline-timing-repeats 10\n"
              << "  --baseline-timing-warmups 1\n"
              << "  --shot-measurement-auto-limit 1000000\n"
              << "  --shot-aggregate-memory-limit 1000000\n"
              << "  --shot-batch-size 100000\n"
              << "  --shot-generation-chunk-size 1024\n"
              << "  --latency-histogram-buckets-per-decade 120\n"
              << "  --progress-interval-seconds 5\n"
              << "  --output-dir path\n"
              << "  --max-settings count\n"
              << "Environment: OMP_NUM_THREADS controls shot-level OpenMP parallelism for trace-off passes.\n";
    std::exit(EXIT_SUCCESS);
}

void print_build_info_and_exit() {
    std::cout << "pymatching_enable_profile="
#if PYMATCHING_ENABLE_PROFILE
              << "1"
#else
              << "0"
#endif
              << "\n";
    std::cout << "pymatching_enable_nvtx="
#if PYMATCHING_ENABLE_NVTX
              << "1"
#else
              << "0"
#endif
              << "\n";
    std::cout << "openmp="
#ifdef _OPENMP
              << "1"
#else
              << "0"
#endif
              << "\n";
    std::exit(EXIT_SUCCESS);
}

Options parse_args(int argc, const char **argv) {
    Options options;
    for (int k = 1; k < argc; k++) {
        std::string arg = argv[k];
        if (arg == "--help" || arg == "-h") {
            print_usage_and_exit();
        }
        if (arg == "--build-info") {
            print_build_info_and_exit();
        }
        if (auto value = argument_value(arg, "--distances", k, argc, argv)) {
            options.distances = parse_size_list(*value, "distance");
        } else if (auto value = argument_value(arg, "--error-rates", k, argc, argv)) {
            options.error_rates = parse_probability_list(*value, "error rate");
        } else if (auto value = argument_value(arg, "--cases", k, argc, argv)) {
            options.cases = parse_case_list(*value);
        } else if (auto value = argument_value(arg, "--rounds", k, argc, argv)) {
            options.round_specs = split_any(*value, ",");
        } else if (auto value = argument_value(arg, "--shots", k, argc, argv)) {
            options.shots = parse_size_value(*value, "shots");
        } else if (auto value = argument_value(arg, "--seed", k, argc, argv)) {
            options.seed = parse_u64_value(*value, "seed");
        } else if (auto value = argument_value(arg, "--trace-level", k, argc, argv)) {
            options.trace_level = trace_level_from_name(*value);
        } else if (auto value = argument_value(arg, "--profile-mode", k, argc, argv)) {
            options.profile_mode = profile_mode_from_name(*value);
        } else if (auto value = argument_value(arg, "--stats-granularity", k, argc, argv)) {
            options.stats_granularity = stats_granularity_from_name(*value);
        } else if (auto value = argument_value(arg, "--shot-measurements", k, argc, argv)) {
            options.shot_measurement_mode = shot_measurement_mode_from_name(*value);
        } else if (auto value = argument_value(arg, "--tail-shot-capture", k, argc, argv)) {
            options.tail_shot_capture_mode = tail_shot_capture_mode_from_name(*value);
        } else if (auto value = argument_value(arg, "--tail-shot-k-values", k, argc, argv)) {
            options.tail_shot_k_values = parse_nonnegative_double_list(*value, "tail shot k value");
        } else if (auto value = argument_value(arg, "--tail-shots-per-interval", k, argc, argv)) {
            options.tail_shots_per_interval = parse_size_value(*value, "tail shots per interval");
        } else if (auto value = argument_value(arg, "--replay-shots-csv", k, argc, argv)) {
            options.replay_shots_csv = *value;
        } else if (auto value = argument_value(arg, "--graph-backend", k, argc, argv)) {
            options.graph_backend = *value;
            if (*value != "generic" && *value != "static_grid")
                throw std::invalid_argument("Invalid graph backend.");
        } else if (auto value = argument_value(arg, "--grid-simd", k, argc, argv)) {
            options.grid_simd = *value;
            if (*value != "auto" && *value != "scalar" && *value != "avx512")
                throw std::invalid_argument("Invalid grid SIMD mode.");
        } else if (auto value = argument_value(arg, "--baseline-timing-repeats", k, argc, argv)) {
            options.baseline_timing_repeats = parse_size_value(*value, "baseline timing repeats");
        } else if (auto value = argument_value(arg, "--baseline-timing-warmups", k, argc, argv)) {
            options.baseline_timing_warmups = parse_size_value(*value, "baseline timing warmups");
        } else if (auto value = argument_value(arg, "--shot-measurement-auto-limit", k, argc, argv)) {
            options.shot_measurement_auto_limit = parse_size_value(*value, "shot measurement auto limit");
        } else if (auto value = argument_value(arg, "--shot-aggregate-memory-limit", k, argc, argv)) {
            options.shot_aggregate_memory_limit = parse_size_value(*value, "shot aggregate memory limit");
        } else if (auto value = argument_value(arg, "--shot-batch-size", k, argc, argv)) {
            options.shot_batch_size = parse_size_value(*value, "shot batch size");
        } else if (auto value = argument_value(arg, "--shot-generation-chunk-size", k, argc, argv)) {
            options.shot_generation_chunk_size = parse_size_value(*value, "shot generation chunk size");
        } else if (auto value = argument_value(arg, "--latency-histogram-buckets-per-decade", k, argc, argv)) {
            options.latency_histogram_buckets_per_decade =
                parse_size_value(*value, "latency histogram buckets per decade");
        } else if (auto value = argument_value(arg, "--progress-interval-seconds", k, argc, argv)) {
            options.progress_interval_seconds = parse_nonnegative_double_value(*value, "progress interval seconds");
        } else if (auto value = argument_value(arg, "--output-dir", k, argc, argv)) {
            options.output_dir = *value;
        } else if (auto value = argument_value(arg, "--max-settings", k, argc, argv)) {
            options.max_settings = parse_size_value(*value, "max settings");
        } else {
            throw std::invalid_argument("Unrecognized argument: " + arg);
        }
    }
    if (options.round_specs.empty()) {
        throw std::invalid_argument("At least one round spec is required.");
    }
    if (options.shots == 0) {
        throw std::invalid_argument("Shots must be greater than zero.");
    }
    if (options.baseline_timing_repeats == 0) {
        throw std::invalid_argument("Baseline timing repeats must be greater than zero.");
    }
    if (options.shot_batch_size == 0) {
        throw std::invalid_argument("Shot batch size must be greater than zero.");
    }
    if (options.shot_generation_chunk_size == 0) {
        throw std::invalid_argument("Shot generation chunk size must be greater than zero.");
    }
    if (options.latency_histogram_buckets_per_decade == 0) {
        throw std::invalid_argument("Latency histogram buckets per decade must be greater than zero.");
    }
    if (options.tail_shot_capture_mode == TailShotCaptureMode::on && options.tail_shot_k_values.empty()) {
        throw std::invalid_argument("Tail shot capture requires at least one k value.");
    }
    for (size_t i = 0; i < options.tail_shot_k_values.size(); i++) {
        double value = options.tail_shot_k_values[i];
        if (!std::isfinite(value) || value <= 0) {
            throw std::invalid_argument("Tail shot k values must be positive finite values.");
        }
        if (i > 0 && options.tail_shot_k_values[i - 1] <= value) {
            throw std::invalid_argument("Tail shot k values must be strictly descending.");
        }
    }
    if (options.stats_granularity == StatsGranularity::shot && options.shots > options.shot_aggregate_memory_limit) {
        throw std::invalid_argument(
            "Shot stats granularity would write one counters/histograms row set per shot. "
            "Use --stats-granularity setting or raise --shot-aggregate-memory-limit intentionally.");
    }
    if (collects_stats(options.profile_mode) && !binary_supports_profile_stats()) {
        throw std::invalid_argument(
            "This binary was not built with PYMATCHING_ENABLE_PROFILE=1, so stats/both profile modes are unavailable.");
    }
    if (effective_trace_level(options) != TraceLevel::off && !binary_supports_nvtx_trace()) {
        throw std::invalid_argument(
            "This binary was not built with PYMATCHING_ENABLE_NVTX=1, so trace ranges are unavailable.");
    }
    return options;
}

stim::Circuit generate_surface_code_circuit(size_t distance, size_t rounds, double noise) {
    stim::CircuitGenParameters gen(rounds, distance, "rotated_memory_x");
    gen.after_clifford_depolarization = noise;
    gen.before_round_data_depolarization = noise;
    gen.before_measure_flip_probability = noise;
    gen.after_reset_flip_probability = noise;
    return stim::generate_surface_code_circuit(gen).circuit;
}

std::vector<stim::SparseShot> generate_sparse_shots(
    const stim::Circuit &circuit, size_t num_shots, std::mt19937_64 &rng) {
    size_t num_detectors = circuit.count_detectors();
    auto dets_obs = stim::sample_batch_detection_events<stim::MAX_BITWORD_WIDTH>(circuit, num_shots, rng);
    auto &dets = dets_obs.first;
    auto &obs = dets_obs.second;

    std::vector<stim::SparseShot> shots;
    shots.reserve(num_shots);
    for (size_t shot = 0; shot < num_shots; shot++) {
        shots.emplace_back();
        shots.back().obs_mask = stim::simd_bits<64>(1);
        shots.back().obs_mask[0] = obs[0][shot];
        for (size_t det = 0; det < num_detectors; det++) {
            if (dets[det][shot]) {
                shots.back().hits.push_back(det);
            }
        }
    }
    return shots;
}

std::vector<stim::SparseShot> generate_sparse_shots(const stim::Circuit &circuit, size_t num_shots, uint64_t seed) {
    std::mt19937_64 rng(seed);  // NOLINT(cert-msc51-cpp)
    return generate_sparse_shots(circuit, num_shots, rng);
}

uint64_t mix_seed_value(uint64_t state, uint64_t value) {
    state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    state ^= state >> 30;
    state *= 0xbf58476d1ce4e5b9ULL;
    state ^= state >> 27;
    state *= 0x94d049bb133111ebULL;
    state ^= state >> 31;
    return state;
}

uint64_t setting_seed_for(uint64_t seed, size_t distance, double error_rate, size_t rounds) {
    uint64_t state = mix_seed_value(seed, (uint64_t)distance);
    state = mix_seed_value(state, std::bit_cast<uint64_t>(error_rate));
    state = mix_seed_value(state, (uint64_t)rounds);
    return state;
}

uint64_t shot_chunk_seed_for(uint64_t setting_seed, size_t first_shot_index) {
    return mix_seed_value(setting_seed, first_shot_index);
}

std::vector<ReplayShotRow> read_replay_shot_rows(const std::filesystem::path &path) {
    std::vector<ReplayShotRow> result;
    if (path.empty()) {
        return result;
    }
    std::ifstream in(path);
    if (!in) {
        throw std::invalid_argument("Failed to open replay shots CSV: " + path.string());
    }
    std::string line;
    if (!std::getline(in, line)) {
        return result;
    }
    auto header = parse_csv_line(line);
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto row = csv_row_map(header, parse_csv_line(line));
        ReplayShotRow replay;
        replay.distance = parse_optional_size_value(row, "distance");
        replay.error_rate = parse_optional_probability_value(row, "error_rate");
        replay.round_spec = row_value(row, "round_spec");
        replay.rounds = parse_optional_size_value(row, "rounds");
        replay.has_source_shots = has_row_value(row, "shots");
        replay.source_shots = parse_optional_size_value(row, "shots");
        replay.has_setting_seed = has_row_value(row, "setting_seed") || has_row_value(row, "seed");
        replay.setting_seed = parse_optional_u64_value(row, "setting_seed", parse_optional_u64_value(row, "seed"));
        replay.has_shot_generation_chunk_size = has_row_value(row, "shot_generation_chunk_size");
        replay.shot_generation_chunk_size = parse_optional_size_value(row, "shot_generation_chunk_size");
        replay.has_global_chunk_start = has_row_value(row, "global_chunk_start");
        replay.global_chunk_start = parse_optional_size_value(row, "global_chunk_start");
        replay.has_chunk_seed = has_row_value(row, "chunk_seed");
        replay.chunk_seed = parse_optional_u64_value(row, "chunk_seed");
        replay.has_chunk_local_index = has_row_value(row, "chunk_local_index");
        replay.chunk_local_index = parse_optional_size_value(row, "chunk_local_index");
        replay.has_source_shot_index = has_row_value(row, "shot_index");
        replay.source_shot_index = parse_optional_size_value(row, "shot_index");
        replay.has_obs_mask = has_row_value(row, "obs_mask");
        replay.obs_mask = (obs_int)parse_optional_u64_value(row, "obs_mask");
        auto hits_iter = row.find("detector_hits");
        if (hits_iter != row.end()) {
            replay.has_detector_hits_payload = true;
            if (!hits_iter->second.empty()) {
                replay.detector_hits = parse_detector_hits_payload(hits_iter->second);
            }
        }
        if (replay.distance == 0 || replay.round_spec.empty() || replay.rounds == 0) {
            throw std::invalid_argument("Replay shot row is missing distance, round_spec, or rounds in: " + path.string());
        }
        result.push_back(std::move(replay));
    }
    return result;
}

std::vector<const ReplayShotRow *> replay_rows_for_case(
    const std::vector<ReplayShotRow> &rows,
    size_t distance,
    double error_rate,
    std::string_view round_spec,
    size_t rounds) {
    std::vector<const ReplayShotRow *> result;
    for (const auto &row : rows) {
        if (row.distance == distance && row.error_rate == error_rate && std::string_view(row.round_spec) == round_spec &&
            row.rounds == rounds) {
            result.push_back(&row);
        }
    }
    std::sort(result.begin(), result.end(), [](const ReplayShotRow *a, const ReplayShotRow *b) {
        return a->source_shot_index < b->source_shot_index;
    });
    return result;
}

stim::SparseShot sparse_shot_from_replay_row(const stim::Circuit &circuit, const ReplayShotRow &row) {
    if (row.has_detector_hits_payload) {
        stim::SparseShot shot;
        shot.obs_mask = stim::simd_bits<64>(1);
        shot.obs_mask[0] = (bool)(row.obs_mask & 1);
        shot.hits.reserve(row.detector_hits.size());
        size_t num_detectors = circuit.count_detectors();
        for (uint64_t hit : row.detector_hits) {
            if (hit >= num_detectors) {
                throw std::invalid_argument("Replay shot detector_hits contains detector index outside the circuit.");
            }
            shot.hits.push_back(hit);
        }
        return shot;
    }
    if (!row.has_source_shots || !row.has_setting_seed || !row.has_shot_generation_chunk_size ||
        !row.has_global_chunk_start || !row.has_chunk_seed || !row.has_chunk_local_index ||
        !row.has_source_shot_index || !row.has_obs_mask) {
        throw std::invalid_argument(
            "Replay shot row has no detector_hits payload and is missing required seed/chunk/shot provenance.");
    }
    if (row.source_shots == 0 || row.shot_generation_chunk_size == 0) {
        throw std::invalid_argument("Replay shot row has invalid source shots or chunk size.");
    }
    if (row.source_shot_index >= row.source_shots || row.global_chunk_start >= row.source_shots ||
        row.source_shot_index < row.global_chunk_start) {
        throw std::invalid_argument("Replay shot row has inconsistent source shot index and chunk start.");
    }
    if (row.source_shot_index != row.global_chunk_start + row.chunk_local_index) {
        throw std::invalid_argument("Replay shot row chunk_local_index does not match shot_index - global_chunk_start.");
    }
    const uint64_t expected_chunk_seed = shot_chunk_seed_for(row.setting_seed, row.global_chunk_start);
    if (row.chunk_seed != expected_chunk_seed) {
        throw std::invalid_argument("Replay shot row chunk_seed does not match setting_seed/global_chunk_start.");
    }
    size_t chunk_size = row.shot_generation_chunk_size;
    if (row.source_shots > row.global_chunk_start) {
        const size_t remaining = row.source_shots - row.global_chunk_start;
        if (chunk_size == 0 || chunk_size > remaining) {
            chunk_size = remaining;
        }
    }
    if (chunk_size == 0) {
        chunk_size = row.chunk_local_index + 1;
    }
    if (row.chunk_local_index >= chunk_size) {
        throw std::invalid_argument("Replay shot row chunk_local_index is outside the generated chunk.");
    }
    auto chunk_shots = generate_sparse_shots(circuit, chunk_size, row.chunk_seed);
    const auto &shot = chunk_shots[row.chunk_local_index];
    if (shot.obs_mask_as_u64() != row.obs_mask) {
        throw std::invalid_argument("Regenerated replay shot obs_mask does not match replay CSV row.");
    }
    return shot;
}

std::vector<std::pair<size_t, double>> setting_pairs(const Options &options) {
    if (!options.cases.empty()) {
        return options.cases;
    }
    std::vector<std::pair<size_t, double>> result;
    for (double error_rate : options.error_rates) {
        for (size_t distance : options.distances) {
            result.emplace_back(distance, error_rate);
        }
    }
    return result;
}

void write_settings_header(std::ostream &out) {
    out << "setting_id,distance,error_rate,round_spec,rounds,shots,num_detectors,seed\n";
}

void write_runs_header(std::ostream &out) {
    out << "setting_id,distance,error_rate,round_spec,rounds,shots,num_detectors,seed,total_decode_us,"
           "us_per_shot,us_per_round,mistakes,total_detection_events,"
           "logical_error_rate\n";
}

void write_shot_measurements_header(std::ostream &out) {
    out << "setting_id,shot_index,decode_us,decode_us_per_round,timing_repeats,timing_warmups,"
           "decode_us_first,decode_us_min,decode_us_mean,decode_us_max,detection_events,mistake\n";
}

void write_latency_histograms_header(std::ostream &out) {
    out << "setting_id,metric,bucket,bucket_start,bucket_end,count,density,total_count\n";
}

void write_latency_quantiles_header(std::ostream &out) {
    out << "setting_id,metric,mean,p50,p90,p95,p99,max,total_count\n";
}

void write_latency_tail_cutoffs_header(std::ostream &out) {
    out << "setting_id,metric,unit,logical_error_rate,p_logical_ref,k,target_tail_probability,cutoff_value,"
           "empirical_tail_probability,total_count\n";
}

void write_tail_shots_header(std::ostream &out) {
    out << "setting_id,metric,unit,interval_label,interval_start_k,interval_end_k,interval_start_cutoff,"
           "interval_end_cutoff,rank_in_interval,shot_index,setting_seed,shot_generation_chunk_size,"
           "global_chunk_start,chunk_seed,chunk_local_index,decode_us,decode_us_per_round,timing_repeats,"
           "timing_warmups,decode_us_first,decode_us_min,decode_us_mean,decode_us_max,detection_events,"
           "mistake,obs_mask,predicted_obs_mask,detector_hits\n";
}

void write_progress_header(std::ostream &out) {
    out << "setting_id,distance,error_rate,round_spec,rounds,processed_shots,total_shots,percent,elapsed_seconds,"
           "shots_per_second,eta_seconds,batch_wall_seconds,batch_decode_us,shot_parallelism,threads\n";
}

OutputWriters open_output_writers(
    const std::filesystem::path &output_dir,
    bool collect_stats,
    bool write_shot_measurement_rows,
    bool write_latency_summary_rows,
    bool write_tail_shot_rows) {
    OutputWriters writers;
    writers.settings = open_output_file(output_dir / "settings.csv");
    writers.runs = open_output_file(output_dir / "runs.csv");
    writers.progress = open_output_file(output_dir / "progress.csv");

    write_settings_header(writers.settings);
    write_runs_header(writers.runs);
    write_progress_header(writers.progress);
    if (write_shot_measurement_rows) {
        writers.shot_measurements = open_output_file(output_dir / "shot_measurements.csv");
        write_shot_measurements_header(writers.shot_measurements);
    }
    if (write_latency_summary_rows) {
        writers.latency_histograms = open_output_file(output_dir / "latency_histograms.csv");
        writers.latency_quantiles = open_output_file(output_dir / "latency_quantiles.csv");
        writers.latency_tail_cutoffs = open_output_file(output_dir / "latency_tail_cutoffs.csv");
        write_latency_histograms_header(writers.latency_histograms);
        write_latency_quantiles_header(writers.latency_quantiles);
        write_latency_tail_cutoffs_header(writers.latency_tail_cutoffs);
    }
    if (write_tail_shot_rows) {
        writers.tail_shots = open_output_file(output_dir / "tail_shots.csv");
        write_tail_shots_header(writers.tail_shots);
    }
    if (collect_stats) {
        writers.counters = open_output_file(output_dir / "counters.csv");
        writers.histograms = open_output_file(output_dir / "histograms.csv");
        writers.aggregates = open_output_file(output_dir / "aggregates.csv");
        writers.histogram_quantiles = open_output_file(output_dir / "histogram_quantiles.csv");

        write_counters_header(writers.counters);
        write_histograms_header(writers.histograms);
        write_aggregates_header(writers.aggregates);
        write_histogram_quantiles_header(writers.histogram_quantiles);
    }
    return writers;
}

void write_latency_histogram_rows(
    std::ostream &out,
    size_t setting_id,
    std::string_view metric,
    const LatencyMetricSummary &summary) {
    if (summary.zero_count > 0) {
        out << setting_id << "," << csv_quote(metric) << ",zero,0,0," << summary.zero_count << ","
            << (summary.total == 0 ? 0 : (double)summary.zero_count / (double)summary.total) << "," << summary.total
            << "\n";
    }
    for (const auto &[bucket, count] : summary.counts) {
        out << setting_id << "," << csv_quote(metric) << "," << bucket << "," << summary.bucket_start(bucket) << ","
            << summary.bucket_end(bucket) << "," << count << ","
            << (summary.total == 0 ? 0 : (double)count / (double)summary.total) << "," << summary.total << "\n";
    }
}

void write_latency_quantile_row(
    std::ostream &out,
    size_t setting_id,
    std::string_view metric,
    const LatencyMetricSummary &summary) {
    out << setting_id << "," << csv_quote(metric) << "," << summary.mean() << "," << summary.quantile(0.50) << ","
        << summary.quantile(0.90) << "," << summary.quantile(0.95) << "," << summary.quantile(0.99) << ","
        << summary.max_value << "," << summary.total << "\n";
}

void write_latency_tail_cutoff_rows(
    std::ostream &out,
    size_t setting_id,
    std::string_view metric,
    std::string_view unit,
    const LatencyMetricSummary &summary,
    double logical_error_rate,
    size_t shots,
    const std::vector<double> &k_values) {
    double p_logical_ref = std::max(logical_error_rate, shots == 0 ? 0.0 : 1.0 / (double)shots);
    for (double k : k_values) {
        double target = k * p_logical_ref;
        double empirical = 0;
        double cutoff = summary.tail_cutoff(target, empirical);
        out << setting_id << "," << csv_quote(metric) << "," << csv_quote(unit) << "," << logical_error_rate << ","
            << p_logical_ref << "," << k << "," << target << "," << cutoff << "," << empirical << ","
            << summary.total << "\n";
    }
}

void write_latency_summary_rows(
    OutputWriters &writers,
    size_t setting_id,
    double logical_error_rate,
    size_t shots,
    const LatencySummary &summary,
    const std::vector<double> &k_values) {
    write_latency_histogram_rows(writers.latency_histograms, setting_id, "decode_us", summary.decode_us);
    write_latency_histogram_rows(
        writers.latency_histograms, setting_id, "decode_us_per_round", summary.decode_us_per_round);
    write_latency_quantile_row(writers.latency_quantiles, setting_id, "decode_us", summary.decode_us);
    write_latency_quantile_row(
        writers.latency_quantiles, setting_id, "decode_us_per_round", summary.decode_us_per_round);
    write_latency_tail_cutoff_rows(
        writers.latency_tail_cutoffs,
        setting_id,
        "decode_us",
        "us",
        summary.decode_us,
        logical_error_rate,
        shots,
        k_values);
    write_latency_tail_cutoff_rows(
        writers.latency_tail_cutoffs,
        setting_id,
        "decode_us_per_round",
        "us/round",
        summary.decode_us_per_round,
        logical_error_rate,
        shots,
        k_values);
}

std::string tail_interval_label(double start_k, std::optional<double> end_k) {
    std::ostringstream out;
    out << "[" << start_k << ",";
    if (end_k.has_value()) {
        out << *end_k;
    } else {
        out << "inf";
    }
    out << "]";
    return out.str();
}

void write_tail_shot_rows(
    std::ostream &out,
    size_t setting_id,
    const LatencyMetricSummary &summary,
    const TailShotBucketRecorder &recorder,
    const std::vector<double> &k_values,
    double logical_error_rate,
    size_t shots) {
    if (k_values.empty() || shots == 0) {
        return;
    }
    double p_logical_ref = std::max(logical_error_rate, 1.0 / (double)shots);
    std::map<double, std::pair<double, double>> cutoff_by_k;
    for (double k : k_values) {
        double empirical = 0;
        double cutoff = summary.tail_cutoff(k * p_logical_ref, empirical);
        cutoff_by_k[k] = {cutoff, empirical};
    }
    for (size_t interval_index = 0; interval_index < k_values.size(); interval_index++) {
        const double start_k = k_values[interval_index];
        const bool has_end = interval_index + 1 < k_values.size();
        const double end_k = has_end ? k_values[interval_index + 1] : 0;
        auto start_iter = cutoff_by_k.find(start_k);
        if (start_iter == cutoff_by_k.end() || start_iter->second.first <= 0) {
            continue;
        }
        std::optional<double> end_cutoff;
        if (has_end) {
            auto end_iter = cutoff_by_k.find(end_k);
            if (end_iter == cutoff_by_k.end() || end_iter->second.first <= 0) {
                continue;
            }
            end_cutoff = end_iter->second.first;
        }
        auto records = recorder.records_in_interval(start_iter->second.first, end_cutoff);
        auto label = tail_interval_label(start_k, has_end ? std::optional<double>(end_k) : std::nullopt);
        for (size_t rank = 0; rank < records.size(); rank++) {
            const auto &record = records[rank];
            out << setting_id << ",decode_us_per_round,us/round," << csv_quote(label) << "," << start_k << ",";
            if (has_end) {
                out << end_k;
            } else {
                out << "inf";
            }
            out << "," << start_iter->second.first << ",";
            if (end_cutoff.has_value()) {
                out << *end_cutoff;
            }
            out << "," << (rank + 1) << "," << record.shot_index << "," << record.setting_seed << ","
                << record.shot_generation_chunk_size << "," << record.global_chunk_start << "," << record.chunk_seed
                << "," << record.chunk_local_index << "," << record.decode_us << "," << record.decode_us_per_round
                << "," << record.timing_repeats << "," << record.timing_warmups << "," << record.decode_us_first
                << "," << record.decode_us_min << "," << record.decode_us_mean << "," << record.decode_us_max
                << "," << record.detection_events << "," << (record.mistake ? 1 : 0) << ","
                << (uint64_t)record.obs_mask << "," << (uint64_t)record.predicted_obs_mask << ","
                << csv_quote(record.detector_hits) << "\n";
        }
    }
}

template <typename Value, typename Getter>
std::vector<double> values_for(const std::vector<Value> &measurements, Getter getter) {
    std::vector<double> result;
    result.reserve(measurements.size());
    for (const auto &measurement : measurements) {
        result.push_back(getter(measurement));
    }
    return result;
}

void write_setting_aggregates(
    OutputWriters &writers,
    size_t setting_id,
    size_t rounds,
    const std::vector<ShotMeasurement> &measurements,
    const std::vector<ShotStats> &shot_stats,
    const ShotStats &merged_stats) {
    write_aggregate_row(
        writers.aggregates, setting_id, "decode_us", "per_shot", values_for(measurements, [](const ShotMeasurement &m) {
            return m.decode_us;
        }));
    write_aggregate_row(
        writers.aggregates,
        setting_id,
        "decode_us_per_round",
        "per_shot_round",
        values_for(measurements, [&](const ShotMeasurement &m) {
            return rounds == 0 ? 0 : m.decode_us / (double)rounds;
        }));
    write_aggregate_row(
        writers.aggregates,
        setting_id,
        "logical_failure",
        "per_shot",
        values_for(measurements, [](const ShotMeasurement &m) {
            return m.mistake ? 1.0 : 0.0;
        }));

    for (const auto &field : SUM_COUNTER_FIELDS) {
        write_aggregate_row(
            writers.aggregates,
            setting_id,
            field.name,
            "per_shot_sample",
            values_for(shot_stats, [&](const ShotStats &stats) {
                return (double)(stats.*(field.member));
            }));
    }
    for (const auto &field : MAX_COUNTER_FIELDS) {
        write_aggregate_row(
            writers.aggregates,
            setting_id,
            field.name,
            "per_shot_peak",
            values_for(shot_stats, [&](const ShotStats &stats) {
                return (double)(stats.*(field.member));
            }));
    }
    write_histogram_quantile_rows(writers.histogram_quantiles, setting_id, merged_stats);
}

void write_summary(
    const Options &options,
    size_t setting_count,
    size_t effective_shots,
    size_t replay_shot_rows,
    double elapsed_seconds,
    bool wrote_run_outputs,
    bool wrote_stats_outputs,
    bool wrote_shot_measurements,
    bool wrote_latency_summary,
    bool wrote_tail_shots,
    bool used_parallel_shots) {
    auto summary = open_output_file(options.output_dir / "summary.json");
    summary << "{\n"
            << "  \"profile_mode\": ";
    write_json_string(summary, profile_mode_name(options.profile_mode));
    summary << ",\n  \"trace_level\": ";
    write_json_string(summary, trace_level_name(effective_trace_level(options)));
    summary << ",\n  \"stats_granularity\": ";
    write_json_string(summary, stats_granularity_name(options.stats_granularity));
    summary << ",\n  \"shot_measurements\": ";
    write_json_string(summary, shot_measurement_mode_name(options.shot_measurement_mode));
    summary << ",\n  \"requested_tail_shot_capture\": ";
    write_json_string(summary, tail_shot_capture_mode_name(options.tail_shot_capture_mode));
    summary << ",\n  \"tail_shot_capture\": ";
    write_json_string(summary, wrote_tail_shots ? "on" : "off");
    summary << ",\n  \"replay_shots_csv\": ";
    write_json_string(summary, options.replay_shots_csv.string());
    summary << ",\n  \"graph_backend\": ";
    write_json_string(summary, options.graph_backend);
    summary << ",\n  \"grid_simd\": ";
    write_json_string(summary, options.grid_simd);
    summary << ",\n  \"settings\": " << setting_count << ",\n"
            << "  \"shots\": " << options.shots << ",\n"
            << "  \"requested_shots\": " << options.shots << ",\n"
            << "  \"effective_shots\": " << effective_shots << ",\n"
            << "  \"replay_shot_rows\": " << replay_shot_rows << ",\n"
            << "  \"seed_policy\": \"stable_hash_seed_distance_error_rate_rounds_global_chunk_start\",\n"
            << "  \"shot_batch_size\": " << options.shot_batch_size << ",\n"
            << "  \"shot_generation_chunk_size\": " << options.shot_generation_chunk_size << ",\n"
            << "  \"decoder_lifecycle\": \"reuse_per_worker\",\n"
            << "  \"baseline_timing_repeats\": " << options.baseline_timing_repeats << ",\n"
            << "  \"baseline_timing_warmups\": " << options.baseline_timing_warmups << ",\n"
            << "  \"latency_histogram_buckets_per_decade\": " << options.latency_histogram_buckets_per_decade << ",\n"
            << "  \"progress_interval_seconds\": " << options.progress_interval_seconds << ",\n"
            << "  \"shot_measurement_auto_limit\": " << options.shot_measurement_auto_limit << ",\n"
            << "  \"shot_aggregate_memory_limit\": " << options.shot_aggregate_memory_limit << ",\n"
            << "  \"openmp_available\": " << (openmp_available() ? "true" : "false") << ",\n"
            << "  \"openmp_max_threads\": " << openmp_max_threads() << ",\n"
            << "  \"shot_parallelism\": ";
    write_json_string(summary, used_parallel_shots ? "openmp" : "serial");
    summary << ",\n"
            << "  \"run_outputs\": " << (wrote_run_outputs ? "true" : "false") << ",\n"
            << "  \"shot_measurement_rows\": " << (wrote_shot_measurements ? "true" : "false") << ",\n"
            << "  \"latency_summary_rows\": " << (wrote_latency_summary ? "true" : "false") << ",\n"
            << "  \"tail_shot_rows\": " << (wrote_tail_shots ? "true" : "false") << ",\n"
            << "  \"stats_outputs\": " << (wrote_stats_outputs ? "true" : "false") << ",\n"
            << "  \"elapsed_seconds\": " << elapsed_seconds << "\n"
            << "}\n";
}

void write_string_array(std::ostream &out, const std::vector<std::string> &values) {
    out << "[";
    for (size_t k = 0; k < values.size(); k++) {
        if (k) {
            out << ", ";
        }
        write_json_string(out, values[k]);
    }
    out << "]";
}

void write_size_array(std::ostream &out, const std::vector<size_t> &values) {
    out << "[";
    for (size_t k = 0; k < values.size(); k++) {
        if (k) {
            out << ", ";
        }
        out << values[k];
    }
    out << "]";
}

void write_double_array(std::ostream &out, const std::vector<double> &values) {
    out << "[";
    for (size_t k = 0; k < values.size(); k++) {
        if (k) {
            out << ", ";
        }
        out << values[k];
    }
    out << "]";
}

void write_cases_array(std::ostream &out, const std::vector<std::pair<size_t, double>> &cases) {
    out << "[";
    for (size_t k = 0; k < cases.size(); k++) {
        if (k) {
            out << ", ";
        }
        out << "{\"distance\": " << cases[k].first << ", \"error_rate\": " << cases[k].second << "}";
    }
    out << "]";
}

void write_optional_path(std::ostream &out, bool enabled, const std::filesystem::path &path) {
    if (!enabled) {
        out << "null";
        return;
    }
    write_json_string(out, path.string());
}

std::string indexed_name(std::string_view prefix, size_t index, size_t total) {
    size_t width = std::max<size_t>(3, std::to_string(total == 0 ? 0 : total - 1).size());
    std::stringstream out;
    out << prefix << "." << std::setw((int)width) << std::setfill('0') << index;
    return out.str();
}

std::string shot_trace_name(size_t index, size_t total, size_t syndrome_count) {
    std::stringstream out;
    out << indexed_name("shot", index, total) << " syndrome=" << syndrome_count;
    return out.str();
}

void write_manifest(
    const Options &options,
    size_t effective_shots,
    size_t replay_shot_rows,
    bool wrote_run_outputs,
    bool wrote_stats_outputs,
    bool wrote_shot_measurements,
    bool wrote_latency_summary,
    bool wrote_tail_shots,
    bool used_parallel_shots) {
    auto manifest = open_output_file(options.output_dir / "manifest.json");
    manifest << "{\n"
             << "  \"profile_mode\": ";
    write_json_string(manifest, profile_mode_name(options.profile_mode));
    manifest << ",\n  \"requested_trace_level\": ";
    write_json_string(manifest, trace_level_name(options.trace_level));
    manifest << ",\n  \"effective_trace_level\": ";
    write_json_string(manifest, trace_level_name(effective_trace_level(options)));
    manifest << ",\n  \"stats_granularity\": ";
    write_json_string(manifest, stats_granularity_name(options.stats_granularity));
    manifest << ",\n  \"shot_measurements\": ";
    write_json_string(manifest, shot_measurement_mode_name(options.shot_measurement_mode));
    manifest << ",\n  \"requested_tail_shot_capture\": ";
    write_json_string(manifest, tail_shot_capture_mode_name(options.tail_shot_capture_mode));
    manifest << ",\n  \"tail_shot_capture\": ";
    write_json_string(manifest, wrote_tail_shots ? "on" : "off");
    manifest << ",\n  \"tail_shots_per_interval\": " << options.tail_shots_per_interval << ",\n"
             << "  \"tail_shot_k_values\": ";
    write_double_array(manifest, options.tail_shot_k_values);
    manifest << ",\n  \"replay_shots_csv\": ";
    write_json_string(manifest, options.replay_shots_csv.string());
    manifest << ",\n  \"graph_backend\": ";
    write_json_string(manifest, options.graph_backend);
    manifest << ",\n  \"grid_simd\": ";
    write_json_string(manifest, options.grid_simd);
    manifest << ",\n  \"seed\": " << options.seed << ",\n"
             << "  \"seed_policy\": \"stable_hash_seed_distance_error_rate_rounds_global_chunk_start\",\n"
             << "  \"shots\": " << options.shots << ",\n"
             << "  \"requested_shots\": " << options.shots << ",\n"
             << "  \"effective_shots\": " << effective_shots << ",\n"
             << "  \"replay_shot_rows\": " << replay_shot_rows << ",\n"
             << "  \"shot_batch_size\": " << options.shot_batch_size << ",\n"
             << "  \"shot_generation_chunk_size\": " << options.shot_generation_chunk_size << ",\n"
             << "  \"decoder_lifecycle\": \"reuse_per_worker\",\n"
             << "  \"baseline_timing_repeats\": " << options.baseline_timing_repeats << ",\n"
             << "  \"baseline_timing_warmups\": " << options.baseline_timing_warmups << ",\n"
             << "  \"latency_histogram_buckets_per_decade\": " << options.latency_histogram_buckets_per_decade << ",\n"
             << "  \"progress_interval_seconds\": " << options.progress_interval_seconds << ",\n"
             << "  \"shot_measurement_auto_limit\": " << options.shot_measurement_auto_limit << ",\n"
             << "  \"shot_aggregate_memory_limit\": " << options.shot_aggregate_memory_limit << ",\n"
             << "  \"openmp_available\": " << (openmp_available() ? "true" : "false") << ",\n"
             << "  \"openmp_max_threads\": " << openmp_max_threads() << ",\n"
             << "  \"shot_parallelism\": ";
    write_json_string(manifest, used_parallel_shots ? "openmp" : "serial");
    manifest << ",\n"
             << "  \"max_settings\": " << options.max_settings << ",\n"
             << "  \"setting_source\": ";
    write_json_string(manifest, options.cases.empty() ? "grid" : "cases");
    manifest << ",\n"
             << "  \"distances\": ";
    if (options.cases.empty()) {
        write_size_array(manifest, options.distances);
    } else {
        manifest << "[]";
    }
    manifest << ",\n  \"error_rates\": ";
    if (options.cases.empty()) {
        write_double_array(manifest, options.error_rates);
    } else {
        manifest << "[]";
    }
    manifest << ",\n  \"cases\": ";
    write_cases_array(manifest, options.cases);
    manifest << ",\n  \"round_specs\": ";
    write_string_array(manifest, options.round_specs);
    manifest << ",\n  \"output_dir\": ";
    write_json_string(manifest, options.output_dir.string());
    manifest << ",\n  \"summary_json\": ";
    write_json_string(manifest, (options.output_dir / "summary.json").string());
    manifest << ",\n  \"settings_csv\": ";
    write_optional_path(manifest, wrote_run_outputs, options.output_dir / "settings.csv");
    manifest << ",\n  \"runs_csv\": ";
    write_optional_path(manifest, wrote_run_outputs, options.output_dir / "runs.csv");
    manifest << ",\n  \"progress_csv\": ";
    write_optional_path(manifest, wrote_run_outputs, options.output_dir / "progress.csv");
    manifest << ",\n  \"shot_measurements_csv\": ";
    write_optional_path(manifest, wrote_shot_measurements, options.output_dir / "shot_measurements.csv");
    manifest << ",\n  \"latency_histograms_csv\": ";
    write_optional_path(manifest, wrote_latency_summary, options.output_dir / "latency_histograms.csv");
    manifest << ",\n  \"latency_quantiles_csv\": ";
    write_optional_path(manifest, wrote_latency_summary, options.output_dir / "latency_quantiles.csv");
    manifest << ",\n  \"latency_tail_cutoffs_csv\": ";
    write_optional_path(manifest, wrote_latency_summary, options.output_dir / "latency_tail_cutoffs.csv");
    manifest << ",\n  \"tail_shots_csv\": ";
    write_optional_path(manifest, wrote_tail_shots, options.output_dir / "tail_shots.csv");
    manifest << ",\n  \"counters_csv\": ";
    write_optional_path(manifest, wrote_stats_outputs, options.output_dir / "counters.csv");
    manifest << ",\n  \"histograms_csv\": ";
    write_optional_path(manifest, wrote_stats_outputs, options.output_dir / "histograms.csv");
    manifest << ",\n  \"aggregates_csv\": ";
    write_optional_path(manifest, wrote_stats_outputs, options.output_dir / "aggregates.csv");
    manifest << ",\n  \"histogram_quantiles_csv\": ";
    write_optional_path(manifest, wrote_stats_outputs, options.output_dir / "histogram_quantiles.csv");
    manifest << "\n}\n";
}

}  // namespace

template <BackendKind Kind>
struct RunnerBackend;

template <>
struct RunnerBackend<BackendKind::Generic> {
    using Decoder = pm::Mwpm;
    static Decoder create(const stim::DetectorErrorModel& dem, size_t, size_t, const Options&) {
        return pm::detector_error_model_to_mwpm(dem, pm::NUM_DISTINCT_WEIGHTS);
    }
};

template <>
struct RunnerBackend<BackendKind::Structured> {
    using Decoder = pm::structured::ComponentDecoder;
    static Decoder create(const stim::DetectorErrorModel& dem, size_t distance, size_t rounds, const Options& options) {
        auto user = pm::detector_error_model_to_backend_user_graph(dem, false, pm::NUM_DISTINCT_WEIGHTS);
        auto mode = options.grid_simd == "scalar"   ? pm::GridSimd::scalar
                    : options.grid_simd == "avx512" ? pm::GridSimd::avx512
                                                    : pm::GridSimd::automatic;
        user.configure_static_grid(pm::static_grid_spec_from_dem(dem, distance, rounds, true, mode));
        return std::move(user.get_structured_mwpm());
    }
};

template <BackendKind Kind>
int run(const Options& options) {
    using Decoder = typename RunnerBackend<Kind>::Decoder;
    ensure_output_dir(options.output_dir);

    auto replay_rows = read_replay_shot_rows(options.replay_shots_csv);
    const bool replaying_shots = !options.replay_shots_csv.empty();
    bool collect_stats = collects_stats(options.profile_mode);
    bool write_run_output = writes_run_outputs(options.profile_mode);
    bool write_shot_measurement_rows = writes_shot_measurements(options);
    bool write_latency_summary_output = options.profile_mode == ProfileMode::baseline;
    bool write_tail_shot_rows_output = writes_tail_shots(options) && !replaying_shots;
    bool keep_shot_aggregates = keeps_shot_aggregates_in_memory(options);
    bool used_parallel_shots = !replaying_shots && can_parallelize_shots(options, options.shots);
    OutputWriters writers;
    if (write_run_output) {
        writers = open_output_writers(
            options.output_dir,
            collect_stats,
            write_shot_measurement_rows,
            write_latency_summary_output,
            write_tail_shot_rows_output);
    }

    Session profile_session;
    profile_session.trace_level = effective_trace_level(options);
    std::optional<ActiveSessionScope> active_profile;
    if (activates_profile_session(options)) {
        active_profile.emplace(&profile_session);
    }

    size_t setting_count = 0;
    size_t effective_shots_total = 0;
    auto suite_start = std::chrono::steady_clock::now();

    for (const auto &[distance, error_rate] : setting_pairs(options)) {
        if (setting_count >= options.max_settings) {
            break;
        }

        for (const auto &round_spec : options.round_specs) {
            if (setting_count >= options.max_settings) {
                break;
            }
            size_t rounds = scaled_count_for_spec(round_spec, distance);
            auto replay_case_rows = replay_rows_for_case(replay_rows, distance, error_rate, round_spec, rounds);
            if (replaying_shots && replay_case_rows.empty()) {
                continue;
            }
            size_t setting_id = setting_count++;
            size_t setting_shots = replaying_shots ? replay_case_rows.size() : options.shots;
            uint64_t setting_seed = setting_seed_for(options.seed, distance, error_rate, rounds);
            if (replaying_shots && !replay_case_rows.empty() && replay_case_rows.front()->setting_seed != 0) {
                setting_seed = replay_case_rows.front()->setting_seed;
            }
            effective_shots_total += setting_shots;
            auto circuit = generate_surface_code_circuit(distance, rounds, error_rate);
            auto dem =
                stim::ErrorAnalyzer::circuit_to_detector_error_model(circuit, true, true, false, 0, false, false);

            if (write_run_output) {
                writers.settings << setting_id << "," << distance << "," << error_rate << "," << csv_quote(round_spec)
                                 << "," << rounds << "," << setting_shots << "," << dem.count_detectors() << ","
                                 << setting_seed << "\n";
            }

            std::vector<ShotMeasurement> aggregate_measurements;
            std::vector<ShotStats> aggregate_shot_stats;
            std::optional<ShotStats> merged_setting_stats;
            if (collect_stats) {
                merged_setting_stats.emplace();
            }
            if (keep_shot_aggregates) {
                aggregate_measurements.reserve(setting_shots);
                if (collect_stats) {
                    aggregate_shot_stats.reserve(setting_shots);
                }
            }
            const bool need_per_shot_stats =
                collect_stats && (keep_shot_aggregates || options.stats_granularity == StatsGranularity::shot);
            const bool parallel_shots = !replaying_shots && can_parallelize_shots(options, setting_shots);
            LatencySummary latency_summary(options.latency_histogram_buckets_per_decade);
            TailShotBucketRecorder tail_shot_recorder(
                options.latency_histogram_buckets_per_decade,
                options.tail_shots_per_interval);
            auto make_decoder = [&]() {
                return RunnerBackend<Kind>::create(dem, distance, rounds, options);
            };
            std::vector<Decoder> worker_decoders;
            size_t worker_count = parallel_shots ? openmp_max_threads() : 1;
            worker_decoders.reserve(worker_count);
            for (size_t worker = 0; worker < worker_count; worker++)
                worker_decoders.push_back(make_decoder());
            auto setting_wall_start = std::chrono::steady_clock::now();
            auto last_progress_report =
                setting_wall_start - std::chrono::duration<double>(options.progress_interval_seconds);

            auto decode_one_shot = [&](
                                       Decoder &shot_mwpm,
                                       Session &shot_session,
                                       const stim::SparseShot &shot,
                                       size_t shot_index,
                                       std::optional<ShotStats> *shot_stats_output,
                                       ShotStats *setting_stats_output) {
                ShotMeasurement measurement;
                measurement.detection_events = shot.hits.size();
                auto record_single_timing = [&](double decode_us) {
                    measurement.decode_us = decode_us;
                    measurement.decode_us_first = decode_us;
                    measurement.decode_us_min = decode_us;
                    measurement.decode_us_mean = decode_us;
                    measurement.decode_us_max = decode_us;
                    measurement.timing_repeats = 1;
                    measurement.timing_warmups = 0;
                };
                if (collect_stats) {
                    ShotStats current_stats;
                    current_stats.record_detection_events(measurement.detection_events);
                    {
                        ActiveShotScope active_shot(shot_session, &current_stats);
                        std::optional<ScopedRange> shot_range;
                        if (trace_enabled(TraceLevel::phase)) {
                            shot_range.emplace(
                                TraceLevel::phase,
                                shot_trace_name(shot_index, setting_shots, shot.hits.size()),
                                Color::Blue);
                        }
                        auto shot_start = std::chrono::steady_clock::now();
                        auto res = decode_detection_events_for_up_to_64_observables(
                            shot_mwpm, shot.hits, /*edge_correlations=*/false);
                        auto shot_end = std::chrono::steady_clock::now();
                        measurement.mistake = shot.obs_mask_as_u64() != res.obs_mask;
                        measurement.predicted_obs_mask = res.obs_mask;
                        record_single_timing(
                            std::chrono::duration<double, std::micro>(shot_end - shot_start).count());
                    }
                    current_stats.record_shot_summary_samples();
                    if (setting_stats_output != nullptr) {
                        setting_stats_output->merge_from(current_stats);
                    }
                    if (shot_stats_output != nullptr) {
                        shot_stats_output->emplace(std::move(current_stats));
                    }
                } else {
                    std::optional<ScopedRange> shot_range;
                    if (trace_enabled(TraceLevel::phase)) {
                        shot_range.emplace(
                            TraceLevel::phase,
                            shot_trace_name(shot_index, setting_shots, shot.hits.size()),
                            Color::Blue);
                    }
                    if (options.profile_mode == ProfileMode::baseline) {
                        for (size_t repeat = 0; repeat < options.baseline_timing_warmups; repeat++) {
                            (void)decode_detection_events_for_up_to_64_observables(
                                shot_mwpm, shot.hits, /*edge_correlations=*/false);
                        }
                        double first_us = 0;
                        double min_us = std::numeric_limits<double>::infinity();
                        double max_us = 0;
                        double sum_us = 0;
                        obs_int best_predicted_obs_mask = 0;
                        bool best_mistake = false;
                        for (size_t repeat = 0; repeat < options.baseline_timing_repeats; repeat++) {
                            auto shot_start = std::chrono::steady_clock::now();
                            auto res = decode_detection_events_for_up_to_64_observables(
                                shot_mwpm, shot.hits, /*edge_correlations=*/false);
                            auto shot_end = std::chrono::steady_clock::now();
                            double decode_us =
                                std::chrono::duration<double, std::micro>(shot_end - shot_start).count();
                            if (repeat == 0) {
                                first_us = decode_us;
                            }
                            sum_us += decode_us;
                            max_us = std::max(max_us, decode_us);
                            if (decode_us < min_us) {
                                min_us = decode_us;
                                best_predicted_obs_mask = res.obs_mask;
                                best_mistake = shot.obs_mask_as_u64() != res.obs_mask;
                            }
                        }
                        measurement.decode_us = min_us;
                        measurement.decode_us_first = first_us;
                        measurement.decode_us_min = min_us;
                        measurement.decode_us_mean = sum_us / (double)options.baseline_timing_repeats;
                        measurement.decode_us_max = max_us;
                        measurement.timing_repeats = options.baseline_timing_repeats;
                        measurement.timing_warmups = options.baseline_timing_warmups;
                        measurement.mistake = best_mistake;
                        measurement.predicted_obs_mask = best_predicted_obs_mask;
                    } else {
                        auto shot_start = std::chrono::steady_clock::now();
                        auto res = decode_detection_events_for_up_to_64_observables(
                            shot_mwpm, shot.hits, /*edge_correlations=*/false);
                        auto shot_end = std::chrono::steady_clock::now();
                        measurement.mistake = shot.obs_mask_as_u64() != res.obs_mask;
                        measurement.predicted_obs_mask = res.obs_mask;
                        record_single_timing(
                            std::chrono::duration<double, std::micro>(shot_end - shot_start).count());
                    }
                }
                return measurement;
            };

            size_t mistakes = 0;
            uint64_t total_detection_events = 0;
            double total_decode_us = 0;
            for (size_t shot_offset = 0; shot_offset < setting_shots; shot_offset += options.shot_batch_size) {
                auto batch_wall_start = std::chrono::steady_clock::now();
                const size_t batch_size = std::min(options.shot_batch_size, setting_shots - shot_offset);
                std::vector<ShotMeasurement> shot_measurements(batch_size);
                std::vector<std::optional<ShotStats>> shot_stats;
                if (need_per_shot_stats) {
                    shot_stats.resize(batch_size);
                }
                ShotStats batch_setting_stats;
                {
                    ScopedRange decode_capture_range(RangeId::DecodeProfile);
                    if (replaying_shots) {
                        auto &mwpm = worker_decoders[0];
                        for (size_t batch_index = 0; batch_index < batch_size; batch_index++) {
                            const auto &replay_row = *replay_case_rows[shot_offset + batch_index];
                            auto replay_shot = sparse_shot_from_replay_row(circuit, replay_row);
                            auto *stats_output = need_per_shot_stats ? &shot_stats[batch_index] : nullptr;
                            auto *setting_stats_output = collect_stats ? &batch_setting_stats : nullptr;
                            shot_measurements[batch_index] = decode_one_shot(
                                mwpm,
                                profile_session,
                                replay_shot,
                                replay_row.source_shot_index,
                                stats_output,
                                setting_stats_output);
                        }
                    } else {
#ifdef _OPENMP
                    if (parallel_shots) {
                        const size_t batch_end = shot_offset + batch_size;
                        const size_t first_global_chunk_start =
                            (shot_offset / options.shot_generation_chunk_size) * options.shot_generation_chunk_size;
                        const size_t last_global_chunk_start =
                            ((batch_end - 1) / options.shot_generation_chunk_size) *
                            options.shot_generation_chunk_size;
                        const size_t chunk_count =
                            ((last_global_chunk_start - first_global_chunk_start) /
                             options.shot_generation_chunk_size) +
                            1;
#pragma omp parallel
                        {
                            auto &thread_mwpm = worker_decoders[omp_get_thread_num()];
                            Session thread_session;
                            thread_session.trace_level = TraceLevel::off;
                            std::optional<ActiveSessionScope> active_thread_profile;
                            if (collect_stats) {
                                active_thread_profile.emplace(&thread_session);
                            }
                            ShotStats thread_setting_stats;
                            TailShotBucketRecorder thread_tail_shot_recorder(
                                options.latency_histogram_buckets_per_decade,
                                options.tail_shots_per_interval);
#pragma omp for schedule(dynamic, 1)
                            for (int64_t chunk_index_i64 = 0; chunk_index_i64 < (int64_t)chunk_count;
                                 chunk_index_i64++) {
                                const auto chunk_index = (size_t)chunk_index_i64;
                                const size_t global_chunk_start =
                                    first_global_chunk_start + chunk_index * options.shot_generation_chunk_size;
                                const size_t global_chunk_size =
                                    std::min(options.shot_generation_chunk_size, options.shots - global_chunk_start);
                                const size_t global_begin = std::max(shot_offset, global_chunk_start);
                                const size_t global_end =
                                    std::min(batch_end, global_chunk_start + global_chunk_size);
                                std::mt19937_64 chunk_rng(shot_chunk_seed_for(setting_seed, global_chunk_start));
                                auto chunk_shots = generate_sparse_shots(circuit, global_chunk_size, chunk_rng);
                                for (size_t shot_index = global_begin; shot_index < global_end; shot_index++) {
                                    const size_t batch_index = shot_index - shot_offset;
                                    const size_t chunk_local_index = shot_index - global_chunk_start;
                                    auto *stats_output = need_per_shot_stats ? &shot_stats[batch_index] : nullptr;
                                    auto *setting_stats_output = collect_stats ? &thread_setting_stats : nullptr;
                                    auto measurement = decode_one_shot(
                                        thread_mwpm,
                                        thread_session,
                                        chunk_shots[chunk_local_index],
                                        shot_index,
                                        stats_output,
                                        setting_stats_output);
                                    shot_measurements[batch_index] = measurement;
                                    if (write_tail_shot_rows_output) {
                                        TailShotRecord record;
                                        record.shot_index = shot_index;
                                        record.setting_seed = setting_seed;
                                        record.shot_generation_chunk_size = options.shot_generation_chunk_size;
                                        record.global_chunk_start = global_chunk_start;
                                        record.chunk_seed = shot_chunk_seed_for(setting_seed, global_chunk_start);
                                        record.chunk_local_index = chunk_local_index;
                                        record.decode_us = measurement.decode_us;
                                        record.decode_us_per_round =
                                            rounds == 0 ? 0 : measurement.decode_us / (double)rounds;
                                        record.decode_us_first = measurement.decode_us_first;
                                        record.decode_us_min = measurement.decode_us_min;
                                        record.decode_us_mean = measurement.decode_us_mean;
                                        record.decode_us_max = measurement.decode_us_max;
                                        record.timing_repeats = measurement.timing_repeats;
                                        record.timing_warmups = measurement.timing_warmups;
                                        record.detection_events = measurement.detection_events;
                                        record.mistake = measurement.mistake;
                                        record.obs_mask = chunk_shots[chunk_local_index].obs_mask_as_u64();
                                        record.predicted_obs_mask = measurement.predicted_obs_mask;
                                        record.detector_hits = detector_hits_payload(chunk_shots[chunk_local_index]);
                                        thread_tail_shot_recorder.add(record);
                                    }
                                }
                            }
                            if (collect_stats || write_tail_shot_rows_output) {
#pragma omp critical
                                {
                                    if (collect_stats) {
                                        batch_setting_stats.merge_from(thread_setting_stats);
                                    }
                                    if (write_tail_shot_rows_output) {
                                        tail_shot_recorder.merge_from(thread_tail_shot_recorder);
                                    }
                                }
                            }
                        }
                    } else
#endif
                    {
                        auto &mwpm = worker_decoders[0];
                        const size_t batch_end = shot_offset + batch_size;
                        const size_t first_global_chunk_start =
                            (shot_offset / options.shot_generation_chunk_size) * options.shot_generation_chunk_size;
                        for (size_t global_chunk_start = first_global_chunk_start; global_chunk_start < batch_end;
                             global_chunk_start += options.shot_generation_chunk_size) {
                            const size_t global_chunk_size =
                                std::min(options.shot_generation_chunk_size, options.shots - global_chunk_start);
                            const size_t global_begin = std::max(shot_offset, global_chunk_start);
                            const size_t global_end =
                                std::min(batch_end, global_chunk_start + global_chunk_size);
                            std::mt19937_64 chunk_rng(shot_chunk_seed_for(setting_seed, global_chunk_start));
                            auto chunk_shots = generate_sparse_shots(circuit, global_chunk_size, chunk_rng);
                            for (size_t shot_index = global_begin; shot_index < global_end; shot_index++) {
                                const size_t batch_index = shot_index - shot_offset;
                                const size_t chunk_local_index = shot_index - global_chunk_start;
                                auto *stats_output = need_per_shot_stats ? &shot_stats[batch_index] : nullptr;
                                auto *setting_stats_output = collect_stats ? &batch_setting_stats : nullptr;
                                auto measurement = decode_one_shot(
                                    mwpm,
                                    profile_session,
                                    chunk_shots[chunk_local_index],
                                    shot_index,
                                    stats_output,
                                    setting_stats_output);
                                shot_measurements[batch_index] = measurement;
                                if (write_tail_shot_rows_output) {
                                    TailShotRecord record;
                                    record.shot_index = shot_index;
                                    record.setting_seed = setting_seed;
                                    record.shot_generation_chunk_size = options.shot_generation_chunk_size;
                                    record.global_chunk_start = global_chunk_start;
                                    record.chunk_seed = shot_chunk_seed_for(setting_seed, global_chunk_start);
                                    record.chunk_local_index = chunk_local_index;
                                    record.decode_us = measurement.decode_us;
                                    record.decode_us_per_round =
                                        rounds == 0 ? 0 : measurement.decode_us / (double)rounds;
                                    record.decode_us_first = measurement.decode_us_first;
                                    record.decode_us_min = measurement.decode_us_min;
                                    record.decode_us_mean = measurement.decode_us_mean;
                                    record.decode_us_max = measurement.decode_us_max;
                                    record.timing_repeats = measurement.timing_repeats;
                                    record.timing_warmups = measurement.timing_warmups;
                                    record.detection_events = measurement.detection_events;
                                    record.mistake = measurement.mistake;
                                    record.obs_mask = chunk_shots[chunk_local_index].obs_mask_as_u64();
                                    record.predicted_obs_mask = measurement.predicted_obs_mask;
                                    record.detector_hits = detector_hits_payload(chunk_shots[chunk_local_index]);
                                    tail_shot_recorder.add(record);
                                }
                            }
                        }
                    }
                    }
                }
                auto batch_wall_end = std::chrono::steady_clock::now();
                double batch_decode_us = 0;

                if (collect_stats) {
                    merged_setting_stats->merge_from(batch_setting_stats);
                }

                for (size_t batch_index = 0; batch_index < shot_measurements.size(); batch_index++) {
                    const size_t shot_index = replaying_shots
                                                  ? replay_case_rows[shot_offset + batch_index]->source_shot_index
                                                  : shot_offset + batch_index;
                    const auto &measurement = shot_measurements[batch_index];
                    total_detection_events += measurement.detection_events;
                    total_decode_us += measurement.decode_us;
                    batch_decode_us += measurement.decode_us;
                    if (write_latency_summary_output) {
                        latency_summary.add(measurement.decode_us, rounds);
                    }
                    if (measurement.mistake) {
                        mistakes++;
                    }
                    if (write_shot_measurement_rows) {
                        double shot_decode_us_per_round = rounds == 0 ? 0 : measurement.decode_us / (double)rounds;
                        writers.shot_measurements << setting_id << "," << shot_index << "," << measurement.decode_us << ","
                                                  << shot_decode_us_per_round << "," << measurement.timing_repeats << ","
                                                  << measurement.timing_warmups << "," << measurement.decode_us_first << ","
                                                  << measurement.decode_us_min << "," << measurement.decode_us_mean << ","
                                                  << measurement.decode_us_max << "," << measurement.detection_events << ","
                                                  << (measurement.mistake ? 1 : 0) << "\n";
                    }
                    if (keep_shot_aggregates) {
                        aggregate_measurements.push_back(measurement);
                        if (collect_stats) {
                            aggregate_shot_stats.push_back(std::move(*shot_stats[batch_index]));
                        }
                    }
                }

                if (collect_stats && options.stats_granularity == StatsGranularity::shot) {
                    for (size_t batch_index = 0; batch_index < shot_stats.size(); batch_index++) {
                        const size_t shot_index = replaying_shots
                                                      ? replay_case_rows[shot_offset + batch_index]->source_shot_index
                                                      : shot_offset + batch_index;
                        auto shot_scope = indexed_name("shot", shot_index, setting_shots);
                        write_counter_rows(writers.counters, setting_id, shot_scope, *shot_stats[batch_index], 1);
                        write_histogram_rows(writers.histograms, setting_id, shot_scope, *shot_stats[batch_index]);
                    }
                }

                size_t processed_shots = shot_offset + batch_size;
                auto progress_now = std::chrono::steady_clock::now();
                const double seconds_since_report =
                    std::chrono::duration<double>(progress_now - last_progress_report).count();
                const bool progress_due =
                    options.progress_interval_seconds == 0 ||
                    seconds_since_report >= options.progress_interval_seconds ||
                    processed_shots == setting_shots;
                if (write_run_output && progress_due) {
                    const double elapsed_seconds =
                        std::chrono::duration<double>(progress_now - setting_wall_start).count();
                    const double percent =
                        setting_shots == 0 ? 100.0 : 100.0 * (double)processed_shots / (double)setting_shots;
                    const double shots_per_second =
                        elapsed_seconds <= 0 ? 0 : (double)processed_shots / elapsed_seconds;
                    const double eta_seconds = shots_per_second <= 0
                                                   ? 0
                                                   : (double)(setting_shots - processed_shots) / shots_per_second;
                    const double batch_wall_seconds =
                        std::chrono::duration<double>(batch_wall_end - batch_wall_start).count();
                    const size_t bar_width = 24;
                    const size_t filled =
                        std::min(bar_width, (size_t)std::llround((percent / 100.0) * (double)bar_width));
                    std::string bar(filled, '#');
                    bar.append(bar_width - filled, '.');
                    const auto thread_count = parallel_shots ? openmp_max_threads() : (size_t)1;

                    writers.progress << setting_id << "," << distance << "," << error_rate << ","
                                     << csv_quote(round_spec) << "," << rounds << "," << processed_shots << ","
                                     << setting_shots << "," << percent << "," << elapsed_seconds << ","
                                     << shots_per_second << "," << eta_seconds << "," << batch_wall_seconds << ","
                                     << batch_decode_us << "," << (parallel_shots ? "openmp" : "serial") << ","
                                     << thread_count << "\n";
                    writers.progress.flush();

                    std::ostringstream line;
                    line << "progress mode=" << profile_mode_name(options.profile_mode) << " d=" << distance
                         << " rounds=" << rounds << " p=" << std::setprecision(6) << error_rate << " [" << bar
                         << "] " << std::fixed << std::setprecision(1) << percent << "% shots=" << processed_shots
                         << "/" << setting_shots << std::setprecision(0) << " rate=" << shots_per_second
                         << "/s eta=" << eta_seconds << "s" << std::setprecision(2)
                         << " wall_batch=" << batch_wall_seconds << "s"
                         << " parallel=" << (parallel_shots ? "openmp" : "serial")
                         << " threads=" << thread_count << "\n";
                    std::cout << line.str();
                    std::cout.flush();
                    last_progress_report = progress_now;
                }
            }

            double logical_error_rate = setting_shots == 0 ? 0 : (double)mistakes / (double)setting_shots;
            if (write_run_output) {
                double us_per_shot = setting_shots == 0 ? 0 : total_decode_us / (double)setting_shots;
                double us_per_round =
                    setting_shots == 0 || rounds == 0 ? 0 : total_decode_us / ((double)setting_shots * (double)rounds);
                writers.runs << setting_id << "," << distance << "," << error_rate << "," << csv_quote(round_spec)
                             << "," << rounds << "," << setting_shots << "," << dem.count_detectors() << ","
                             << setting_seed << "," << total_decode_us << "," << us_per_shot << "," << us_per_round
                             << "," << mistakes << "," << total_detection_events << "," << logical_error_rate << "\n";
            }

            if (write_latency_summary_output) {
                write_latency_summary_rows(
                    writers,
                    setting_id,
                    logical_error_rate,
                    setting_shots,
                    latency_summary,
                    options.tail_shot_k_values);
            }

            if (write_tail_shot_rows_output && !replaying_shots) {
                write_tail_shot_rows(
                    writers.tail_shots,
                    setting_id,
                    latency_summary.decode_us_per_round,
                    tail_shot_recorder,
                    options.tail_shot_k_values,
                    logical_error_rate,
                    setting_shots);
            }

            if (collect_stats) {
                write_counter_rows(writers.counters, setting_id, "setting", *merged_setting_stats, setting_shots);
                write_histogram_rows(writers.histograms, setting_id, "setting", *merged_setting_stats);
                if (keep_shot_aggregates) {
                    write_setting_aggregates(
                        writers,
                        setting_id,
                        rounds,
                        aggregate_measurements,
                        aggregate_shot_stats,
                        *merged_setting_stats);
                } else {
                    write_histogram_quantile_rows(writers.histogram_quantiles, setting_id, *merged_setting_stats);
                }
            }

            std::cout << "mode=" << profile_mode_name(options.profile_mode) << " d=" << distance << " rounds=" << rounds
                      << " p=" << error_rate;
            if (replaying_shots) {
                std::cout << " requested_shots=" << options.shots << " replay_shots=" << setting_shots;
            } else {
                std::cout << " shots=" << setting_shots;
            }
            std::cout
                      << " us/shot=" << (setting_shots == 0 ? 0 : total_decode_us / (double)setting_shots)
                      << " mistakes=" << mistakes
                      << " shot_parallelism=" << (parallel_shots ? "openmp" : "serial")
                      << " threads=" << (parallel_shots ? openmp_max_threads() : (size_t)1)
                      << " shot_measurements=" << (write_shot_measurement_rows ? "on" : "off")
                      << " shot_batch_size=" << options.shot_batch_size << "\n";
        }
    }

    auto suite_end = std::chrono::steady_clock::now();
    double suite_seconds = std::chrono::duration<double>(suite_end - suite_start).count();
    write_summary(
        options,
        setting_count,
        replaying_shots ? effective_shots_total : options.shots,
        replaying_shots ? effective_shots_total : 0,
        suite_seconds,
        write_run_output,
        collect_stats,
        write_shot_measurement_rows,
        write_latency_summary_output,
        write_tail_shot_rows_output,
        used_parallel_shots);
    write_manifest(
        options,
        replaying_shots ? effective_shots_total : options.shots,
        replaying_shots ? effective_shots_total : 0,
        write_run_output,
        collect_stats,
        write_shot_measurement_rows,
        write_latency_summary_output,
        write_tail_shot_rows_output,
        used_parallel_shots);
    return EXIT_SUCCESS;
}

int main(int argc, const char **argv) {
    auto options = parse_args(argc, argv);
    if (options.graph_backend == "generic")
        return run<BackendKind::Generic>(options);
    return run<BackendKind::Structured>(options);
}

}  // namespace pm::profile
