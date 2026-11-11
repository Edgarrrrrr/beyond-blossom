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
#include "beyond_blossom/profile/profile.h"

#include <gtest/gtest.h>
#include <set>
#include <sstream>
#include <type_traits>

using namespace beyond_blossom::profile;

#if !(BB_ENABLE_PROFILE && BB_ENABLE_NVTX)
static_assert(std::is_empty_v<ScopedRange>);
static_assert(std::is_trivially_destructible_v<ScopedRange>);
#endif

TEST(profile_schema, range_guards_preserve_statistics_without_trace) {
    ShotStats stats;
    Session session;
    session.shot = &stats;
    session.trace_level = TraceLevel::off;
    ActiveSessionScope scope(&session);

    {
        auto counted = counted_range(RangeId::NodeNext, &ShotStats::neighbor_scans, 7);
        auto updated = updated_range(RangeId::NodeLook, [](ShotStats &shot) {
            shot.look_at_node += 3;
        });
        auto queued = queue_scoped_range(QueueKind::node, "test", Color::Blue);
        ScopedRange named(TraceLevel::queue, std::string("dynamic test range"));
    }

    ASSERT_EQ(stats.neighbor_scans, 7);
    ASSERT_EQ(stats.look_at_node, 3);
    ASSERT_FALSE(trace_enabled(TraceLevel::phase));
}

TEST(profile_schema, descriptor_counts_match_shot_stats_schema) {
    ASSERT_EQ(SUM_COUNTER_FIELDS.size(), 53);
    ASSERT_EQ(MAX_COUNTER_FIELDS.size(), 6);
    ASSERT_EQ(HISTOGRAM_FIELDS.size(), 15);

    std::set<std::string_view> names;
    for (const auto &field : SUM_COUNTER_FIELDS) {
        ASSERT_TRUE(names.insert(field.name).second) << field.name;
    }
    for (const auto &field : MAX_COUNTER_FIELDS) {
        ASSERT_TRUE(names.insert(field.name).second) << field.name;
    }
    for (const auto &field : HISTOGRAM_FIELDS) {
        ASSERT_TRUE(names.insert(field.name).second) << field.name;
    }
}

TEST(profile_schema, merge_from_uses_counter_and_histogram_descriptors) {
    ShotStats a;
    ShotStats b;
    for (size_t k = 0; k < SUM_COUNTER_FIELDS.size(); k++) {
        a.*(SUM_COUNTER_FIELDS[k].member) = k + 1;
        b.*(SUM_COUNTER_FIELDS[k].member) = 100 + k;
    }
    for (size_t k = 0; k < MAX_COUNTER_FIELDS.size(); k++) {
        a.*(MAX_COUNTER_FIELDS[k].member) = k;
        b.*(MAX_COUNTER_FIELDS[k].member) = 10 + k;
    }
    for (const auto &field : HISTOGRAM_FIELDS) {
        (a.*(field.member)).add(1);
        (b.*(field.member)).add(2);
        (b.*(field.member)).add(2);
    }

    a.merge_from(b);

    for (size_t k = 0; k < SUM_COUNTER_FIELDS.size(); k++) {
        ASSERT_EQ(a.*(SUM_COUNTER_FIELDS[k].member), (k + 1) + (100 + k)) << SUM_COUNTER_FIELDS[k].name;
    }
    for (size_t k = 0; k < MAX_COUNTER_FIELDS.size(); k++) {
        ASSERT_EQ(a.*(MAX_COUNTER_FIELDS[k].member), 10 + k) << MAX_COUNTER_FIELDS[k].name;
    }
    for (const auto &field : HISTOGRAM_FIELDS) {
        const auto &hist = a.*(field.member);
        ASSERT_EQ(hist.buckets.at(1), 1) << field.name;
        ASSERT_EQ(hist.buckets.at(2), 2) << field.name;
    }
}

TEST(profile_schema, writes_counter_and_histogram_tables_from_descriptors) {
    ShotStats stats;
    stats.timeline_runs = 3;
    stats.queue_max_total = 5;
    stats.queue_total_hist.add(7);

    std::stringstream counters;
    write_counters_header(counters);
    write_counter_rows(counters, 2, "setting", stats, 10);
    ASSERT_NE(counters.str().find("setting_id,scope,metric,value,per_shot"), std::string::npos);
    ASSERT_NE(counters.str().find("2,\"setting\",\"timeline_runs\",3,0.3"), std::string::npos);
    ASSERT_NE(counters.str().find("2,\"setting\",\"queue_max_total\",5,0.5"), std::string::npos);

    std::stringstream histograms;
    write_histograms_header(histograms);
    write_histogram_rows(histograms, 2, "setting", stats);
    ASSERT_NE(histograms.str().find("2,\"queue_total.setting\",7,1"), std::string::npos);
}

TEST(profile_schema, region_lifecycle_helpers_sample_active_and_shell_area_histograms) {
    ShotStats stats;
    Session session;
    session.shot = &stats;
    ActiveSessionScope scope(&session);

    record_region_created();
    record_region_created();
    record_region_shell_area(3);
    record_alt_tree_nodes_active(1);
    record_alt_tree_nodes_active(4);
    record_alt_tree_nodes_per_tree(4);
    stats.record_shot_summary_samples();
    record_region_destroyed();
    record_region_reset();
    record_alt_tree_nodes_reset();

    ASSERT_EQ(stats.regions_created, 2);
    ASSERT_EQ(stats.region_active, 0);
    ASSERT_EQ(stats.region_active_max, 2);
    ASSERT_EQ(stats.region_active_hist.buckets.at(0), 1);
    ASSERT_EQ(stats.region_active_hist.buckets.at(1), 2);
    ASSERT_EQ(stats.region_active_hist.buckets.at(2), 1);
    ASSERT_EQ(stats.region_shell_area_hist.buckets.at(3), 1);
    ASSERT_EQ(stats.alt_tree_nodes_active, 0);
    ASSERT_EQ(stats.alt_tree_nodes_active_max, 4);
    ASSERT_EQ(stats.alt_tree_nodes_active_hist.buckets.at(0), 1);
    ASSERT_EQ(stats.alt_tree_nodes_active_hist.buckets.at(1), 1);
    ASSERT_EQ(stats.alt_tree_nodes_active_hist.buckets.at(4), 1);
    ASSERT_EQ(stats.alt_tree_nodes_active_peak_hist.buckets.at(4), 1);
    ASSERT_EQ(stats.alt_tree_nodes_per_tree_hist.buckets.at(4), 1);
}
