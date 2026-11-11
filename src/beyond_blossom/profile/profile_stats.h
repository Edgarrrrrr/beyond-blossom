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

#ifndef BB_SPARSE_BLOSSOM_PROFILE_PROFILE_STATS_H
#define BB_SPARSE_BLOSSOM_PROFILE_PROFILE_STATS_H

#include <cstdint>
#include <vector>

#include "beyond_blossom/profile/profile.h"

namespace beyond_blossom::profile {

struct SummaryStats {
    double mean = 0;
    double median = 0;
    double p90 = 0;
    double p95 = 0;
    double p99 = 0;
    double stddev = 0;
    double min = 0;
    double max = 0;
    double cv = 0;
};

struct HistogramSummary {
    double mean = 0;
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
    uint64_t max = 0;
    uint64_t total_count = 0;
};

SummaryStats summarize(std::vector<double> values);
HistogramSummary summarize_histogram(const Histogram &histogram);

}  // namespace beyond_blossom::profile

#endif  // BB_SPARSE_BLOSSOM_PROFILE_PROFILE_STATS_H
