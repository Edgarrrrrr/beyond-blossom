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

#ifndef PYMATCHING_SPARSE_BLOSSOM_PROFILE_PROFILE_SCHEMA_H
#define PYMATCHING_SPARSE_BLOSSOM_PROFILE_PROFILE_SCHEMA_H

#include <array>
#include <ostream>
#include <string_view>
#include <vector>

#include "pymatching/sparse_blossom/profile/profile.h"

namespace pm::profile {

struct CounterField {
    std::string_view name;
    uint64_t ShotStats::*member;
};

struct HistogramField {
    std::string_view name;
    Histogram ShotStats::*member;
};

// Summed counters are additive across shots.
inline constexpr auto SUM_COUNTER_FIELDS = std::to_array<CounterField>({
    CounterField{"detection_events", &ShotStats::detection_events},
    CounterField{"detection_regions_created", &ShotStats::detection_regions_created},
    CounterField{"regions_created", &ShotStats::regions_created},
    CounterField{"timeline_runs", &ShotStats::timeline_runs},
    CounterField{"dual_phase_runs", &ShotStats::dual_phase_runs},
    CounterField{"primal_phase_runs", &ShotStats::primal_phase_runs},
    CounterField{"final_shatter_extract_runs", &ShotStats::final_shatter_extract_runs},
    CounterField{"reset_runs", &ShotStats::reset_runs},
    CounterField{"flooder_arrive", &ShotStats::flooder_arrive},
    CounterField{"flooder_leave", &ShotStats::flooder_leave},
    CounterField{"flooder_collide_region", &ShotStats::flooder_collide_region},
    CounterField{"flooder_collide_boundary", &ShotStats::flooder_collide_boundary},
    CounterField{"flooder_implode_blossom", &ShotStats::flooder_implode_blossom},
    CounterField{"flooder_implode_trivial", &ShotStats::flooder_implode_trivial},
    CounterField{"look_at_node", &ShotStats::look_at_node},
    CounterField{"look_at_region", &ShotStats::look_at_region},
    CounterField{"matcher_tree_hit_match", &ShotStats::matcher_tree_hit_match},
    CounterField{"matcher_tree_hit_other_tree", &ShotStats::matcher_tree_hit_other_tree},
    CounterField{"matcher_tree_hit_self", &ShotStats::matcher_tree_hit_self},
    CounterField{"matcher_blossom_shatter", &ShotStats::matcher_blossom_shatter},
    CounterField{"matcher_tree_hit_boundary", &ShotStats::matcher_tree_hit_boundary},
    CounterField{"matcher_tree_hit_boundary_match", &ShotStats::matcher_tree_hit_boundary_match},
    CounterField{"matcher_make_child", &ShotStats::matcher_make_child},
    CounterField{"matcher_final_blossom_shatter", &ShotStats::matcher_final_blossom_shatter},
    CounterField{"blossoms_created", &ShotStats::blossoms_created},
    CounterField{"region_set_growing", &ShotStats::region_set_growing},
    CounterField{"region_set_frozen", &ShotStats::region_set_frozen},
    CounterField{"region_set_shrinking", &ShotStats::region_set_shrinking},
    CounterField{"node_reschedules", &ShotStats::node_reschedules},
    CounterField{"shrink_event_schedules", &ShotStats::shrink_event_schedules},
    CounterField{"neighbor_scans", &ShotStats::neighbor_scans},
    CounterField{"queue_enqueue", &ShotStats::queue_enqueue},
    CounterField{"queue_enqueue_node", &ShotStats::queue_enqueue_node},
    CounterField{"queue_enqueue_region", &ShotStats::queue_enqueue_region},
    CounterField{"queue_enqueue_search", &ShotStats::queue_enqueue_search},
    CounterField{"queue_dequeue", &ShotStats::queue_dequeue},
    CounterField{"queue_dequeue_node", &ShotStats::queue_dequeue_node},
    CounterField{"queue_dequeue_region", &ShotStats::queue_dequeue_region},
    CounterField{"queue_dequeue_search", &ShotStats::queue_dequeue_search},
    CounterField{"queue_valid", &ShotStats::queue_valid},
    CounterField{"queue_valid_node", &ShotStats::queue_valid_node},
    CounterField{"queue_valid_region", &ShotStats::queue_valid_region},
    CounterField{"queue_valid_search", &ShotStats::queue_valid_search},
    CounterField{"queue_stale", &ShotStats::queue_stale},
    CounterField{"queue_stale_node", &ShotStats::queue_stale_node},
    CounterField{"queue_stale_region", &ShotStats::queue_stale_region},
    CounterField{"queue_stale_search", &ShotStats::queue_stale_search},
    CounterField{"queue_deferred_requeue", &ShotStats::queue_deferred_requeue},
    CounterField{"queue_deferred_requeue_node", &ShotStats::queue_deferred_requeue_node},
    CounterField{"queue_deferred_requeue_region", &ShotStats::queue_deferred_requeue_region},
    CounterField{"queue_deferred_requeue_search", &ShotStats::queue_deferred_requeue_search},
    CounterField{"queue_redistributions", &ShotStats::queue_redistributions},
    CounterField{"queue_redistributed_events", &ShotStats::queue_redistributed_events},
});

// Max counters track peak queue occupancy and must merge with max(), not sum().
// Raw counter rows still include a value/shots column for schema uniformity, but
// aggregate rows intentionally report only raw peak occupancy.
inline constexpr auto MAX_COUNTER_FIELDS = std::to_array<CounterField>({
    CounterField{"queue_max_total", &ShotStats::queue_max_total},
    CounterField{"queue_max_node", &ShotStats::queue_max_node},
    CounterField{"queue_max_region", &ShotStats::queue_max_region},
    CounterField{"queue_max_search", &ShotStats::queue_max_search},
    CounterField{"region_active_max", &ShotStats::region_active_max},
    CounterField{"alt_tree_nodes_active_max", &ShotStats::alt_tree_nodes_active_max},
});

// Histograms are merged bucket-wise and written both raw and as quantiles.
inline constexpr auto HISTOGRAM_FIELDS = std::to_array<HistogramField>({
    HistogramField{"queue_total", &ShotStats::queue_total_hist},
    HistogramField{"queue_node", &ShotStats::queue_node_hist},
    HistogramField{"queue_region", &ShotStats::queue_region_hist},
    HistogramField{"queue_search", &ShotStats::queue_search_hist},
    HistogramField{"queue_bucket", &ShotStats::queue_bucket_hist},
    HistogramField{"queue_redistribution_bucket", &ShotStats::queue_redistribution_bucket_hist},
    HistogramField{"blossom_created_cycle_length", &ShotStats::blossom_created_cycle_length_hist},
    HistogramField{"blossom_shattered_cycle_length", &ShotStats::blossom_shattered_cycle_length_hist},
    HistogramField{"final_blossom_cycle_length", &ShotStats::final_blossom_cycle_length_hist},
    HistogramField{"detection_events_per_shot", &ShotStats::detection_events_per_shot_hist},
    HistogramField{"region_active", &ShotStats::region_active_hist},
    HistogramField{"region_shell_area", &ShotStats::region_shell_area_hist},
    HistogramField{"alt_tree_nodes_active", &ShotStats::alt_tree_nodes_active_hist},
    HistogramField{"alt_tree_nodes_active_peak", &ShotStats::alt_tree_nodes_active_peak_hist},
    HistogramField{"alt_tree_nodes_per_tree", &ShotStats::alt_tree_nodes_per_tree_hist},
});

void write_counters_header(std::ostream &out);
void write_histograms_header(std::ostream &out);
void write_aggregates_header(std::ostream &out);
void write_histogram_quantiles_header(std::ostream &out);

void write_counter_rows(
    std::ostream &out,
    size_t setting_id,
    std::string_view scope,
    const ShotStats &stats,
    size_t shots);
void write_histogram_rows(
    std::ostream &out,
    size_t setting_id,
    std::string_view histogram_scope,
    const ShotStats &stats);
void write_aggregate_row(
    std::ostream &out,
    size_t setting_id,
    std::string_view metric,
    std::string_view normalization,
    std::vector<double> values);
void write_histogram_quantile_rows(std::ostream &out, size_t setting_id, const ShotStats &stats);

}  // namespace pm::profile

#endif  // PYMATCHING_SPARSE_BLOSSOM_PROFILE_PROFILE_SCHEMA_H
