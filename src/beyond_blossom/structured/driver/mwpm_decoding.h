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

#ifndef BB_STRUCTURED_MWPM_DECODING_H
#define BB_STRUCTURED_MWPM_DECODING_H

#include "beyond_blossom/core/matching_result.h"
#include "beyond_blossom/core/observable_bits.h"
#include "beyond_blossom/structured/matcher/mwpm.h"

namespace beyond_blossom::structured {

using ExtendedMatchingResult = beyond_blossom::ExtendedMatchingResult;

// Stop after predecode and publication of the initial MWPM queue. No formal
// flooder/MWPM event is processed by this entry.
void prepare_timeline_for_mwpm(Mwpm& mwpm, bool run_predecoder = true, bool state_imported = false);
// Internal component/pipeline preparation; its immutable backing stays alive
// until packed extraction and the joined seed-node cleanup have completed.
void prepare_packed_timeline_for_mwpm(Mwpm& mwpm, bool run_predecoder = true, bool state_imported = false);

// These entries consume the component's prepared seed span. Pass false only
// after run_order/merge/finish has completed the predecode phase.
// state_imported requires run_predecoder=false and a completed import; queue
// events and counters then belong to this shot and must survive continuation.
MatchingResult decode_prepared_detection_events_for_up_to_64_observables(Mwpm& mwpm, bool run_predecoder = true, bool state_imported = false);
// Explicit split lifecycle only: final seed cleanup is completed before spans
// retire in reset_after_shot (or by the next preparation's implicit cleanup).
MatchingResult solve_prepared_detection_events_deferring_seed_nodes(Mwpm& mwpm);
void decode_prepared_detection_events(
    Mwpm& mwpm, uint8_t* observables, total_weight_int& weight, bool run_predecoder = true, bool state_imported = false);
void decode_prepared_detection_events_to_match_edges(Mwpm& mwpm, bool run_predecoder = true, bool state_imported = false);
void decode_prepared_detection_events_to_normalized_match_edges(Mwpm& mwpm, bool run_predecoder = true, bool state_imported = false);
void decode_prepared_detection_events_to_edges(Mwpm& mwpm, std::vector<int64_t>& edges, bool run_predecoder = true, bool state_imported = false);

MatchingResult decode_detection_events_for_up_to_64_observables(
    beyond_blossom::structured::Mwpm& mwpm, const std::vector<uint64_t>& detection_events, bool edge_correlations);

/// Used to decode detection events for an existing Mwpm object `mwpm', and a vector of
/// detection event indices `detection_events'. The predicted observables are XOR-ed into an
/// existing uint8_t array with at least `mwpm.flooder.graph.num_observables' elements,
/// the pointer to the first element of which is passed as the `obs_begin_ptr' argument.
/// The weight of the MWPM solution is added to the `weight' argument.
void decode_detection_events(
    beyond_blossom::structured::Mwpm& mwpm,
    const std::vector<uint64_t>& detection_events,
    uint8_t* obs_begin_ptr,
    beyond_blossom::total_weight_int& weight,
    bool edge_correlations);

/// Decode detection events using a Mwpm object and vector of detection event indices
/// Returns the compressed edges in the matching: the pairs of detection events that are
/// matched to each other via paths.
void decode_detection_events_to_match_edges(beyond_blossom::structured::Mwpm& mwpm, const std::vector<uint64_t>& detection_events);

/// Decode detection events using a Mwpm object and vector of detection event indices.
/// Returns the edges in the matching: these are pairs of *detectors* forming *edges* in the
/// matching solution (rather than pairs of detection *events* matched via *paths* as returned
/// instead by `decode_detection_events_to_match_edges`).
void decode_detection_events_to_edges(
    beyond_blossom::structured::Mwpm& mwpm, const std::vector<uint64_t>& detection_events, std::vector<int64_t>& edges);

void decode_detection_events_to_edges_with_edge_correlations(
    beyond_blossom::structured::Mwpm& mwpm, const std::vector<uint64_t>& detection_events, std::vector<int64_t>& edges);

}  // namespace beyond_blossom::structured

#endif  // BB_STRUCTURED_MWPM_DECODING_H
