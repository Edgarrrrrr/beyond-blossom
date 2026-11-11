// Copyright 2022 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
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

#include "beyond_blossom/profile/profile_schema.h"

#include <algorithm>
#include <utility>

#include "beyond_blossom/profile/profile_io.h"
#include "beyond_blossom/profile/profile_stats.h"

namespace beyond_blossom::profile {

void ShotStats::merge_from(const ShotStats &other) {
    for (const auto &field : SUM_COUNTER_FIELDS) {
        this->*(field.member) += other.*(field.member);
    }
    for (const auto &field : MAX_COUNTER_FIELDS) {
        this->*(field.member) = std::max(this->*(field.member), other.*(field.member));
    }
    for (const auto &field : HISTOGRAM_FIELDS) {
        (this->*(field.member)).merge_from(other.*(field.member));
    }
}

void write_counters_header(std::ostream &out) {
    out << "setting_id,scope,metric,value,per_shot\n";
}

void write_histograms_header(std::ostream &out) {
    out << "setting_id,histogram,bucket,count\n";
}

void write_aggregates_header(std::ostream &out) {
    out << "setting_id,metric,normalization,n,mean,median,p90,p95,p99,stddev,min,max,cv\n";
}

void write_histogram_quantiles_header(std::ostream &out) {
    out << "setting_id,histogram,mean,p50,p90,p99,max,total_count\n";
}

void write_counter_rows(
    std::ostream &out,
    size_t setting_id,
    std::string_view scope,
    const ShotStats &stats,
    size_t shots) {
    auto write_field = [&](const CounterField &field) {
        uint64_t value = stats.*(field.member);
        double per_shot = shots == 0 ? 0 : (double)value / (double)shots;
        out << setting_id << "," << csv_quote(scope) << "," << csv_quote(field.name) << "," << value << ","
            << per_shot << "\n";
    };
    for (const auto &field : SUM_COUNTER_FIELDS) {
        write_field(field);
    }
    for (const auto &field : MAX_COUNTER_FIELDS) {
        write_field(field);
    }
}

void write_histogram_rows(
    std::ostream &out,
    size_t setting_id,
    std::string_view histogram_scope,
    const ShotStats &stats) {
    for (const auto &field : HISTOGRAM_FIELDS) {
        std::string name(field.name);
        if (!histogram_scope.empty()) {
            name += ".";
            name += histogram_scope;
        }
        const auto &histogram = stats.*(field.member);
        for (const auto &[bucket, count] : histogram.buckets) {
            out << setting_id << "," << csv_quote(name) << "," << bucket << "," << count << "\n";
        }
    }
}

void write_aggregate_row(
    std::ostream &out,
    size_t setting_id,
    std::string_view metric,
    std::string_view normalization,
    std::vector<double> values) {
    size_t n = values.size();
    auto stats = summarize(std::move(values));
    out << setting_id << "," << csv_quote(metric) << "," << csv_quote(normalization) << "," << n << "," << stats.mean
        << "," << stats.median << "," << stats.p90 << "," << stats.p95 << "," << stats.p99 << "," << stats.stddev
        << "," << stats.min << "," << stats.max << "," << stats.cv << "\n";
}

void write_histogram_quantile_rows(std::ostream &out, size_t setting_id, const ShotStats &stats) {
    for (const auto &field : HISTOGRAM_FIELDS) {
        auto summary = summarize_histogram(stats.*(field.member));
        out << setting_id << "," << csv_quote(field.name) << "," << summary.mean << "," << summary.p50 << ","
            << summary.p90 << "," << summary.p99 << "," << summary.max << "," << summary.total_count << "\n";
    }
}

}  // namespace beyond_blossom::profile
