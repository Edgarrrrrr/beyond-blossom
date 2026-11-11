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

#include "beyond_blossom/profile/profile_stats.h"

#include <algorithm>
#include <cmath>

namespace beyond_blossom::profile {

SummaryStats summarize(std::vector<double> values) {
    SummaryStats result;
    if (values.empty()) {
        return result;
    }

    std::sort(values.begin(), values.end());
    double sum = 0;
    for (double value : values) {
        sum += value;
    }
    result.mean = sum / (double)values.size();
    result.min = values.front();
    result.max = values.back();

    size_t mid = values.size() / 2;
    if (values.size() % 2 == 0) {
        result.median = (values[mid - 1] + values[mid]) / 2;
    } else {
        result.median = values[mid];
    }

    auto quantile = [&](double q) {
        size_t index = (size_t)std::ceil(q * (double)values.size());
        if (index == 0) {
            index = 1;
        }
        return values[index - 1];
    };
    result.p90 = quantile(0.90);
    result.p95 = quantile(0.95);
    result.p99 = quantile(0.99);

    if (values.size() > 1) {
        double variance_sum = 0;
        for (double value : values) {
            variance_sum += (value - result.mean) * (value - result.mean);
        }
        result.stddev = std::sqrt(variance_sum / (double)(values.size() - 1));
    }
    if (result.mean != 0) {
        result.cv = result.stddev / result.mean;
    }
    return result;
}

HistogramSummary summarize_histogram(const Histogram &histogram) {
    HistogramSummary result;
    if (histogram.buckets.empty()) {
        return result;
    }

    uint64_t weighted_sum = 0;
    for (const auto &[bucket, count] : histogram.buckets) {
        result.total_count += count;
        weighted_sum += bucket * count;
        result.max = std::max(result.max, bucket);
    }
    if (result.total_count != 0) {
        result.mean = (double)weighted_sum / (double)result.total_count;
    }

    auto quantile = [&](double q) {
        uint64_t target = (uint64_t)std::ceil(q * (double)result.total_count);
        if (target == 0) {
            target = 1;
        }
        uint64_t seen = 0;
        for (const auto &[bucket, count] : histogram.buckets) {
            seen += count;
            if (seen >= target) {
                return bucket;
            }
        }
        return result.max;
    };
    result.p50 = quantile(0.50);
    result.p90 = quantile(0.90);
    result.p99 = quantile(0.99);
    return result;
}

}  // namespace beyond_blossom::profile
