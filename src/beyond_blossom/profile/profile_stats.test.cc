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

#include <gtest/gtest.h>

using namespace beyond_blossom::profile;

TEST(profile_stats, summarize_reports_basic_sample_statistics) {
    auto stats = summarize({1, 2, 4, 5});
    ASSERT_DOUBLE_EQ(stats.mean, 3);
    ASSERT_DOUBLE_EQ(stats.median, 3);
    ASSERT_DOUBLE_EQ(stats.p90, 5);
    ASSERT_DOUBLE_EQ(stats.p95, 5);
    ASSERT_DOUBLE_EQ(stats.p99, 5);
    ASSERT_DOUBLE_EQ(stats.min, 1);
    ASSERT_DOUBLE_EQ(stats.max, 5);
    ASSERT_NEAR(stats.stddev, 1.825741858, 1e-9);
    ASSERT_NEAR(stats.cv, 0.608580619, 1e-9);
}

TEST(profile_stats, summarize_histogram_reports_weighted_quantiles) {
    Histogram histogram;
    histogram.add(1);
    histogram.add(2);
    histogram.add(2);
    histogram.add(10);

    auto summary = summarize_histogram(histogram);
    ASSERT_DOUBLE_EQ(summary.mean, 3.75);
    ASSERT_EQ(summary.p50, 2);
    ASSERT_EQ(summary.p90, 10);
    ASSERT_EQ(summary.p99, 10);
    ASSERT_EQ(summary.max, 10);
    ASSERT_EQ(summary.total_count, 4);
}
