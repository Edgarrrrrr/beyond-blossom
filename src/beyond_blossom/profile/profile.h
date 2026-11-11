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

#ifndef BB_SPARSE_BLOSSOM_PROFILE_PROFILE_H
#define BB_SPARSE_BLOSSOM_PROFILE_PROFILE_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef BB_ENABLE_PROFILE
#define BB_ENABLE_PROFILE 0
#endif

#ifndef BB_ENABLE_NVTX
#define BB_ENABLE_NVTX 0
#endif

#if BB_ENABLE_NVTX
#if __has_include(<nvtx3/nvToolsExt.h>)
#include <nvtx3/nvToolsExt.h>
#else
#error \
    "BB_ENABLE_NVTX was set, but nvtx3/nvToolsExt.h was not found. Set NVTX_INCLUDE_DIR or load the CUDA/Nsight spack package."
#endif
#endif

namespace beyond_blossom::profile {

enum class TraceLevel : uint8_t {
    // Do not emit NVTX ranges. This is the default for stats-only runs; it
    // does not disable ShotStats counters when a stats session is active.
    off = 0,

    // Emit coarse ranges for the main decode phases. Because trace levels are
    // cumulative, this includes only phase-level ranges unless a higher level
    // is selected.
    phase = 1,

    // Also emit ranges for algorithm events inside phases, such as flooder
    // notifications, matcher tree-hit handlers, and blossom operations.
    event = 2,

    // Also emit priority-queue push/pop ranges. Queue operations are frequent,
    // so this level is intended for tiny cases or targeted queue debugging.
    queue = 3,

    // Enable every range currently instrumented. This is a diagnostic setting,
    // not a recommended default for representative nsys timing.
    all = 4,
};

enum class QueueKind : uint8_t {
    // No queue subtype is known or useful for this counter. Use this when only
    // the aggregate total is meaningful.
    none = 0,

    // Detector-node event, typically a scheduled node interaction or boundary
    // check in the main flooder.
    node = 1,

    // Fill-region event, typically a shrink/implode schedule for an
    // alternating-tree region or blossom.
    region = 2,

    // Search-flooder event. Kept separate from node/region traffic so future
    // correlated-decoding search overhead is visible in queue counters.
    search = 3,
};

struct Color {
    // NVTX expects colors as 0xAARRGGBB when colorType is NVTX_COLOR_ARGB.
    // NVIDIA's C API examples use user-defined COLOR_* constants, so keep the
    // palette named here instead of scattering raw literals across call sites.
    static constexpr uint32_t Blue = 0xff4c78a8;
    static constexpr uint32_t Teal = 0xff72b7b2;
    static constexpr uint32_t Green = 0xff54a24b;
    static constexpr uint32_t Yellow = 0xffeeca3b;
    static constexpr uint32_t Orange = 0xfff58518;
    static constexpr uint32_t Red = 0xffe45756;
    static constexpr uint32_t Purple = 0xffb279a2;
    static constexpr uint32_t Brown = 0xff9d755d;
};

enum class RangeId : uint8_t {
    DecodeProfile,
    Timeline,
    DecodeShot,
    CreateDetectionEvents,
    DualPhase,
    PrimalPhase,
    FinalExtract,
    FinalExtractEdges,
    NodeLook,
    NodeNext,
    NodeArrive,
    NodeHitRegion,
    NodeHitBoundary,
    RegionLook,
    RegionSetGrowing,
    RegionSetFrozen,
    RegionSetShrinking,
    RegionLeave,
    RegionImplode,
    MatchTree,
    MatchOtherTree,
    MatchSelf,
    MatchBoundary,
    MatchBoundaryPair,
    BlossomCreate,
    BlossomImplode,
    BlossomShatter,
    BlossomFinalPair,
    BlossomFinalPairEdges,
};

inline const char *range_name(RangeId id) {
    switch (id) {
        case RangeId::DecodeProfile:
            return "decode_profile";
        case RangeId::Timeline:
            return "timeline";
        case RangeId::DecodeShot:
            return "decode.shot";
        case RangeId::CreateDetectionEvents:
            return "create_detection_events";
        case RangeId::DualPhase:
            return "dual_phase";
        case RangeId::PrimalPhase:
            return "primal_phase";
        case RangeId::FinalExtract:
            return "final.extract";
        case RangeId::FinalExtractEdges:
            return "final.extract_edges";
        case RangeId::NodeLook:
            return "node.look";
        case RangeId::NodeNext:
            return "node.next";
        case RangeId::NodeArrive:
            return "node.arrive";
        case RangeId::NodeHitRegion:
            return "node.hit_region";
        case RangeId::NodeHitBoundary:
            return "node.hit_boundary";
        case RangeId::RegionLook:
            return "region.look";
        case RangeId::RegionSetGrowing:
            return "region.set_growing";
        case RangeId::RegionSetFrozen:
            return "region.set_frozen";
        case RangeId::RegionSetShrinking:
            return "region.set_shrinking";
        case RangeId::RegionLeave:
            return "region.leave";
        case RangeId::RegionImplode:
            return "region.implode";
        case RangeId::MatchTree:
            return "match.tree";
        case RangeId::MatchOtherTree:
            return "match.other_tree";
        case RangeId::MatchSelf:
            return "match.self";
        case RangeId::MatchBoundary:
            return "match.boundary";
        case RangeId::MatchBoundaryPair:
            return "match.boundary_pair";
        case RangeId::BlossomCreate:
            return "blossom.create";
        case RangeId::BlossomImplode:
            return "blossom.implode";
        case RangeId::BlossomShatter:
            return "blossom.shatter";
        case RangeId::BlossomFinalPair:
            return "blossom.final_pair";
        case RangeId::BlossomFinalPairEdges:
            return "blossom.final_pair_edges";
    }
    return "unknown";
}

inline TraceLevel range_trace_level(RangeId id) {
    switch (id) {
        case RangeId::DecodeProfile:
        case RangeId::Timeline:
        case RangeId::DecodeShot:
        case RangeId::CreateDetectionEvents:
        case RangeId::DualPhase:
        case RangeId::PrimalPhase:
        case RangeId::FinalExtract:
        case RangeId::FinalExtractEdges:
            return TraceLevel::phase;
        case RangeId::NodeLook:
        case RangeId::NodeNext:
        case RangeId::NodeArrive:
        case RangeId::NodeHitRegion:
        case RangeId::NodeHitBoundary:
        case RangeId::RegionLook:
        case RangeId::RegionSetGrowing:
        case RangeId::RegionSetFrozen:
        case RangeId::RegionSetShrinking:
        case RangeId::RegionLeave:
        case RangeId::RegionImplode:
        case RangeId::MatchTree:
        case RangeId::MatchOtherTree:
        case RangeId::MatchSelf:
        case RangeId::MatchBoundary:
        case RangeId::MatchBoundaryPair:
        case RangeId::BlossomCreate:
        case RangeId::BlossomImplode:
        case RangeId::BlossomShatter:
        case RangeId::BlossomFinalPair:
        case RangeId::BlossomFinalPairEdges:
            return TraceLevel::event;
    }
    return TraceLevel::all;
}

inline uint32_t range_color(RangeId id) {
    switch (id) {
        case RangeId::DecodeProfile:
        case RangeId::Timeline:
        case RangeId::DecodeShot:
            return Color::Blue;
        case RangeId::CreateDetectionEvents:
        case RangeId::NodeLook:
        case RangeId::NodeNext:
        case RangeId::RegionLook:
        case RangeId::RegionSetFrozen:
            return Color::Teal;
        case RangeId::DualPhase:
        case RangeId::NodeArrive:
        case RangeId::RegionSetGrowing:
        case RangeId::RegionLeave:
            return Color::Green;
        case RangeId::PrimalPhase:
            return Color::Yellow;
        case RangeId::NodeHitRegion:
        case RangeId::RegionImplode:
        case RangeId::MatchTree:
        case RangeId::MatchOtherTree:
            return Color::Orange;
        case RangeId::NodeHitBoundary:
        case RangeId::MatchBoundary:
        case RangeId::MatchBoundaryPair:
            return Color::Red;
        case RangeId::FinalExtract:
        case RangeId::FinalExtractEdges:
        case RangeId::MatchSelf:
        case RangeId::RegionSetShrinking:
        case RangeId::BlossomCreate:
        case RangeId::BlossomImplode:
        case RangeId::BlossomShatter:
        case RangeId::BlossomFinalPair:
        case RangeId::BlossomFinalPairEdges:
            return Color::Purple;
    }
    return Color::Blue;
}

#if BB_ENABLE_NVTX
inline nvtxDomainHandle_t nvtx_domain() {
    static nvtxDomainHandle_t domain = nvtxDomainCreateA("beyond_blossom.sparse_blossom");
    return domain;
}

inline nvtxStringHandle_t registered_range_name(RangeId id) {
    switch (id) {
        case RangeId::DecodeProfile: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "decode_profile");
            return handle;
        }
        case RangeId::Timeline: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "timeline");
            return handle;
        }
        case RangeId::DecodeShot: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "decode.shot");
            return handle;
        }
        case RangeId::CreateDetectionEvents: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "create_detection_events");
            return handle;
        }
        case RangeId::DualPhase: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "dual_phase");
            return handle;
        }
        case RangeId::PrimalPhase: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "primal_phase");
            return handle;
        }
        case RangeId::FinalExtract: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "final.extract");
            return handle;
        }
        case RangeId::FinalExtractEdges: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "final.extract_edges");
            return handle;
        }
        case RangeId::NodeLook: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "node.look");
            return handle;
        }
        case RangeId::NodeNext: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "node.next");
            return handle;
        }
        case RangeId::NodeArrive: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "node.arrive");
            return handle;
        }
        case RangeId::NodeHitRegion: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "node.hit_region");
            return handle;
        }
        case RangeId::NodeHitBoundary: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "node.hit_boundary");
            return handle;
        }
        case RangeId::RegionLook: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "region.look");
            return handle;
        }
        case RangeId::RegionSetGrowing: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "region.set_growing");
            return handle;
        }
        case RangeId::RegionSetFrozen: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "region.set_frozen");
            return handle;
        }
        case RangeId::RegionSetShrinking: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "region.set_shrinking");
            return handle;
        }
        case RangeId::RegionLeave: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "region.leave");
            return handle;
        }
        case RangeId::RegionImplode: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "region.implode");
            return handle;
        }
        case RangeId::MatchTree: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "match.tree");
            return handle;
        }
        case RangeId::MatchOtherTree: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "match.other_tree");
            return handle;
        }
        case RangeId::MatchSelf: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "match.self");
            return handle;
        }
        case RangeId::MatchBoundary: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "match.boundary");
            return handle;
        }
        case RangeId::MatchBoundaryPair: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "match.boundary_pair");
            return handle;
        }
        case RangeId::BlossomCreate: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "blossom.create");
            return handle;
        }
        case RangeId::BlossomImplode: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "blossom.implode");
            return handle;
        }
        case RangeId::BlossomShatter: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "blossom.shatter");
            return handle;
        }
        case RangeId::BlossomFinalPair: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "blossom.final_pair");
            return handle;
        }
        case RangeId::BlossomFinalPairEdges: {
            static nvtxStringHandle_t handle = nvtxDomainRegisterStringA(nvtx_domain(), "blossom.final_pair_edges");
            return handle;
        }
    }
    return nullptr;
}
#endif

struct Histogram {
    std::map<uint64_t, uint64_t> buckets;

    void add(uint64_t value) {
        buckets[value]++;
    }

    void merge_from(const Histogram &other) {
        for (const auto &entry : other.buckets) {
            buckets[entry.first] += entry.second;
        }
    }

    std::string json() const {
        std::stringstream out;
        out << "{";
        bool first = true;
        for (const auto &entry : buckets) {
            if (!first) {
                out << ",";
            }
            first = false;
            out << "\"" << entry.first << "\":" << entry.second;
        }
        out << "}";
        return out.str();
    }
};

struct ShotStats {
    // Number of detector clicks in this shot. Recorded before decode starts and
    // used to relate sparse-blossom work to syndrome density.
    uint64_t detection_events = 0;

    // Number of initial flooder regions created from detector clicks. This can
    // differ from detection_events when boundary nodes or negative-weight
    // pre-marking affect which detections become regions.
    uint64_t detection_regions_created = 0;

    // Number of GraphFillRegion objects created. This includes initial
    // detection regions and blossom regions.
    uint64_t regions_created = 0;

    // Number of times process_timeline_until_completion is entered. For normal
    // decoding this should be one per decoded shot.
    uint64_t timeline_runs = 0;

    // Number of run_until_next_mwpm_notification calls. This includes the
    // terminal call that returns NO_EVENT and may hide multiple internal queue
    // pops before one matcher notification is produced.
    uint64_t dual_phase_runs = 0;

    // Number of primal-phase event handlers run after flooder notifications.
    uint64_t primal_phase_runs = 0;

    // Number of final extraction passes that shatter remaining blossoms and
    // extract observables or match edges.
    uint64_t final_shatter_extract_runs = 0;

    // Number of full MWPM resets. Nonzero values usually indicate an error
    // path or a decoder state cleanup after a failed perfect matching.
    uint64_t reset_runs = 0;

    // A growing region reached a previously empty detector node.
    uint64_t flooder_arrive = 0;

    // A shrinking region released one detector node from its shell.
    uint64_t flooder_leave = 0;

    // Two active regions met, producing a region-hit-region matcher event.
    uint64_t flooder_collide_region = 0;

    // A region reached a boundary, producing a region-hit-boundary event.
    uint64_t flooder_collide_boundary = 0;

    // A blossom region shrank to nothing and was shattered by the flooder.
    uint64_t flooder_implode_blossom = 0;

    // A non-blossom singleton region shrank away by degenerate implosion.
    uint64_t flooder_implode_trivial = 0;

    // Flooder inspected an event whose source was a detector node.
    uint64_t look_at_node = 0;

    // Flooder inspected an event whose source was a fill region.
    uint64_t look_at_region = 0;

    // Matcher found that a tree hit an already matched node/pair endpoint.
    uint64_t matcher_tree_hit_match = 0;

    // Matcher connected two different alternating trees.
    uint64_t matcher_tree_hit_other_tree = 0;

    // Matcher found an odd cycle inside the same alternating tree, creating a
    // blossom.
    uint64_t matcher_tree_hit_self = 0;

    // Matcher shattered a blossom in response to a flooder implode event.
    uint64_t matcher_blossom_shatter = 0;

    // Matcher handled a tree reaching a boundary.
    uint64_t matcher_tree_hit_boundary = 0;

    // Matcher handled a tree hitting a region that was already matched to the
    // boundary, represented by a match record without another region endpoint.
    uint64_t matcher_tree_hit_boundary_match = 0;

    // Matcher created a child alternating-tree node while growing an augmenting
    // structure.
    uint64_t matcher_make_child = 0;

    // Matcher shattered blossoms during final pairing/extraction after the main
    // timeline loop is complete.
    uint64_t matcher_final_blossom_shatter = 0;

    // Number of blossom regions created from odd cycles.
    uint64_t blossoms_created = 0;

    // Region state transitioned to growing at the current flooder time.
    uint64_t region_set_growing = 0;

    // Region state transitioned to frozen. Frozen regions retain their current
    // radius while the matcher resolves related tree structure.
    uint64_t region_set_frozen = 0;

    // Region state transitioned to shrinking, which schedules future implode or
    // leave events.
    uint64_t region_set_shrinking = 0;

    // Number of reschedule_events_at_detector_node calls. This counts repeated
    // recomputations for the same detector node, not distinct nodes.
    uint64_t node_reschedules = 0;

    // Number of tentative shrink/implode events scheduled for regions.
    uint64_t shrink_event_schedules = 0;

    // Number of neighbor edge entries scanned while searching for the next node
    // event. This is a proxy for graph-local search pressure, not queue traffic.
    uint64_t neighbor_scans = 0;

    // Total number of events pushed into the radix heap queue.
    uint64_t queue_enqueue = 0;

    // Enqueued events whose source is a detector node.
    uint64_t queue_enqueue_node = 0;

    // Enqueued events whose source is a fill region.
    uint64_t queue_enqueue_region = 0;

    // Enqueued events from the search flooder.
    uint64_t queue_enqueue_search = 0;

    // Total number of events popped from the queue, before validity checks.
    uint64_t queue_dequeue = 0;

    // Dequeued detector-node events.
    uint64_t queue_dequeue_node = 0;

    // Dequeued fill-region events.
    uint64_t queue_dequeue_region = 0;

    // Dequeued search-flooder events.
    uint64_t queue_dequeue_search = 0;

    // Popped events that were still current and therefore processed.
    uint64_t queue_valid = 0;

    // Valid popped detector-node events.
    uint64_t queue_valid_node = 0;

    // Valid popped fill-region events.
    uint64_t queue_valid_region = 0;

    // Valid popped search-flooder events.
    uint64_t queue_valid_search = 0;

    // Popped events that had become obsolete due to rescheduling or state
    // changes before reaching the queue head.
    uint64_t queue_stale = 0;

    // Stale popped detector-node events.
    uint64_t queue_stale_node = 0;

    // Stale popped fill-region events.
    uint64_t queue_stale_region = 0;

    // Stale popped search-flooder events.
    uint64_t queue_stale_search = 0;

    // Older queued look-at-me events whose desired time moved later while they
    // were waiting; the tracker discards the old pop and re-enqueues at the
    // current desired time.
    uint64_t queue_deferred_requeue = 0;

    // Deferred requeues of detector-node events.
    uint64_t queue_deferred_requeue_node = 0;

    // Deferred requeues of fill-region events.
    uint64_t queue_deferred_requeue_region = 0;

    // Deferred requeues of search-flooder events.
    uint64_t queue_deferred_requeue_search = 0;

    // Number of radix-heap bucket redistribution passes.
    uint64_t queue_redistributions = 0;

    // Total number of queue entries moved during redistributions. A high value
    // means the queue is paying bucket-maintenance cost beyond simple push/pop.
    uint64_t queue_redistributed_events = 0;

    // Current live queue entries tracked by instrumentation. This is working
    // state used to update max counters and occupancy histograms.
    uint64_t queue_live_total = 0;

    // Current live detector-node queue entries.
    uint64_t queue_live_node = 0;

    // Current live fill-region queue entries.
    uint64_t queue_live_region = 0;

    // Current live search-flooder queue entries.
    uint64_t queue_live_search = 0;

    // Current live GraphFillRegion objects tracked by instrumentation.
    uint64_t region_active = 0;

    // Current live AltTreeNode objects sampled from the matcher arena.
    uint64_t alt_tree_nodes_active = 0;

    // Maximum live queue occupancy observed during this shot or merged scope.
    uint64_t queue_max_total = 0;

    // Maximum live detector-node queue occupancy.
    uint64_t queue_max_node = 0;

    // Maximum live fill-region queue occupancy.
    uint64_t queue_max_region = 0;

    // Maximum live search-flooder queue occupancy.
    uint64_t queue_max_search = 0;

    // Maximum live GraphFillRegion count observed during this shot.
    uint64_t region_active_max = 0;

    // Maximum live AltTreeNode count observed during this shot.
    uint64_t alt_tree_nodes_active_max = 0;

    // Samples total live queue occupancy after enqueue/dequeue/remove events.
    Histogram queue_total_hist;

    // Samples live detector-node queue occupancy.
    Histogram queue_node_hist;

    // Samples live fill-region queue occupancy.
    Histogram queue_region_hist;

    // Samples live search-flooder queue occupancy.
    Histogram queue_search_hist;

    // Records radix bucket indices used by enqueued events.
    Histogram queue_bucket_hist;

    // Records radix bucket indices that triggered redistribution.
    Histogram queue_redistribution_bucket_hist;

    // Records the number of contained region edges when a blossom is created.
    Histogram blossom_created_cycle_length_hist;

    // Records cycle sizes for blossoms shattered during flooder implosion.
    Histogram blossom_shattered_cycle_length_hist;

    // Records cycle sizes for blossoms shattered during final extraction.
    Histogram final_blossom_cycle_length_hist;

    // Records syndrome size distribution across shots in the current setting.
    Histogram detection_events_per_shot_hist;

    // Samples current live GraphFillRegion count after region create/delete/reset.
    Histogram region_active_hist;

    // Samples direct GraphFillRegion::shell_area.size() after shell-area changes.
    Histogram region_shell_area_hist;

    // Samples current live AltTreeNode count at primal matcher boundaries.
    Histogram alt_tree_nodes_active_hist;

    // Records one sample per shot: the peak live AltTreeNode count in that shot.
    Histogram alt_tree_nodes_active_peak_hist;

    // Samples the node count of each active alternating-tree root.
    Histogram alt_tree_nodes_per_tree_hist;

    void record_detection_events(uint64_t count) {
        detection_events = count;
        detection_events_per_shot_hist.add(count);
    }

    void record_region_active() {
        region_active_max = std::max(region_active_max, region_active);
        region_active_hist.add(region_active);
    }

    void record_region_created() {
        regions_created++;
        region_active++;
        record_region_active();
    }

    void record_region_destroyed() {
        if (region_active > 0) {
            region_active--;
        }
        record_region_active();
    }

    void record_region_reset() {
        region_active = 0;
        record_region_active();
    }

    void record_region_shell_area(uint64_t shell_area_size) {
        region_shell_area_hist.add(shell_area_size);
    }

    void record_alt_tree_nodes_active(uint64_t count) {
        alt_tree_nodes_active = count;
        alt_tree_nodes_active_max = std::max(alt_tree_nodes_active_max, alt_tree_nodes_active);
        alt_tree_nodes_active_hist.add(alt_tree_nodes_active);
    }

    void record_alt_tree_nodes_reset() {
        record_alt_tree_nodes_active(0);
    }

    void record_alt_tree_nodes_per_tree(uint64_t count) {
        alt_tree_nodes_per_tree_hist.add(count);
    }

    void record_shot_summary_samples() {
        alt_tree_nodes_active_peak_hist.add(alt_tree_nodes_active_max);
    }

    void record_queue_size() {
        queue_max_total = std::max(queue_max_total, queue_live_total);
        queue_max_node = std::max(queue_max_node, queue_live_node);
        queue_max_region = std::max(queue_max_region, queue_live_region);
        queue_max_search = std::max(queue_max_search, queue_live_search);
        queue_total_hist.add(queue_live_total);
        queue_node_hist.add(queue_live_node);
        queue_region_hist.add(queue_live_region);
        queue_search_hist.add(queue_live_search);
    }

    static void record_queue_kind(QueueKind kind, uint64_t &node, uint64_t &region, uint64_t &search) {
        if (kind == QueueKind::node) {
            node++;
        } else if (kind == QueueKind::region) {
            region++;
        } else if (kind == QueueKind::search) {
            search++;
        }
    }

    void record_queue_enqueue(QueueKind kind, uint64_t bucket) {
        queue_enqueue++;
        record_queue_kind(kind, queue_enqueue_node, queue_enqueue_region, queue_enqueue_search);
        queue_live_total++;
        if (kind == QueueKind::node) {
            queue_live_node++;
        } else if (kind == QueueKind::region) {
            queue_live_region++;
        } else if (kind == QueueKind::search) {
            queue_live_search++;
        }
        queue_bucket_hist.add(bucket);
        record_queue_size();
    }

    void record_queue_dequeue(QueueKind kind) {
        queue_dequeue++;
        record_queue_kind(kind, queue_dequeue_node, queue_dequeue_region, queue_dequeue_search);
        record_queue_remove(kind);
    }

    void record_queue_validity(QueueKind kind, bool valid) {
        if (valid) {
            queue_valid++;
            record_queue_kind(kind, queue_valid_node, queue_valid_region, queue_valid_search);
        } else {
            queue_stale++;
            record_queue_kind(kind, queue_stale_node, queue_stale_region, queue_stale_search);
        }
    }

    void record_queue_deferred_requeue(QueueKind kind) {
        queue_deferred_requeue++;
        record_queue_kind(
            kind, queue_deferred_requeue_node, queue_deferred_requeue_region, queue_deferred_requeue_search);
    }

    void record_queue_remove(QueueKind kind) {
        if (queue_live_total > 0) {
            queue_live_total--;
        }
        if (kind == QueueKind::node && queue_live_node > 0) {
            queue_live_node--;
        } else if (kind == QueueKind::region && queue_live_region > 0) {
            queue_live_region--;
        } else if (kind == QueueKind::search && queue_live_search > 0) {
            queue_live_search--;
        }
        record_queue_size();
    }

    void record_queue_redistribution(uint64_t bucket, uint64_t events) {
        queue_redistributions++;
        queue_redistributed_events += events;
        queue_redistribution_bucket_hist.add(bucket);
    }

    void merge_from(const ShotStats &other);
};

struct Session {
    TraceLevel trace_level = TraceLevel::off;
    ShotStats *shot = nullptr;

    bool trace_enabled(TraceLevel minimum) const {
        return static_cast<uint8_t>(trace_level) >= static_cast<uint8_t>(minimum);
    }

    ShotStats *stats() const {
        return shot;
    }
};

#if BB_ENABLE_PROFILE

inline Session *&active_session() {
    static thread_local Session *session = nullptr;
    return session;
}

struct ActiveSessionScope {
    Session *previous;

    explicit ActiveSessionScope(Session *session) : previous(active_session()) {
        active_session() = session;
    }

    ~ActiveSessionScope() {
        active_session() = previous;
    }
};

#else

inline constexpr Session *active_session() {
    return nullptr;
}

struct ActiveSessionScope {
    explicit ActiveSessionScope(Session *) {
    }
};

#endif

inline ShotStats *active_stats() {
#if BB_ENABLE_PROFILE
    auto session = active_session();
    return session ? session->stats() : nullptr;
#else
    return nullptr;
#endif
}

inline bool stats_enabled() {
    return active_stats() != nullptr;
}

using CounterMember = uint64_t ShotStats::*;

// Fast-path helpers for instrumentation call sites. Keep direct session
// plumbing here so algorithm code can express only what it records.
inline void record_counter(CounterMember counter, uint64_t amount = 1) {
#if BB_ENABLE_PROFILE
    if (auto stats = active_stats()) {
        stats->*counter += amount;
    }
#else
    (void)counter;
    (void)amount;
#endif
}

template <typename Update>
inline void update_stats(Update &&update) {
#if BB_ENABLE_PROFILE
    if (auto stats = active_stats()) {
        std::forward<Update>(update)(*stats);
    }
#else
    (void)update;
#endif
}

inline bool trace_enabled(TraceLevel minimum) {
#if BB_ENABLE_PROFILE && BB_ENABLE_NVTX
    auto session = active_session();
    return session && session->trace_enabled(minimum);
#else
    (void)minimum;
    return false;
#endif
}

struct ActiveShotScope {
    Session &session;
    ShotStats *previous;

    ActiveShotScope(Session &session, ShotStats *shot) : session(session), previous(session.shot) {
        session.shot = shot;
    }

    ~ActiveShotScope() {
        session.shot = previous;
    }
};

inline TraceLevel trace_level_from_name(const std::string &name) {
    if (name == "off") {
        return TraceLevel::off;
    }
    if (name == "phase") {
        return TraceLevel::phase;
    }
    if (name == "event") {
        return TraceLevel::event;
    }
    if (name == "queue") {
        return TraceLevel::queue;
    }
    if (name == "all") {
        return TraceLevel::all;
    }
    throw std::invalid_argument("Unrecognized trace level: " + name);
}

inline const char *trace_level_name(TraceLevel level) {
    switch (level) {
        case TraceLevel::off:
            return "off";
        case TraceLevel::phase:
            return "phase";
        case TraceLevel::event:
            return "event";
        case TraceLevel::queue:
            return "queue";
        case TraceLevel::all:
            return "all";
    }
    return "unknown";
}

inline const char *queue_kind_name(QueueKind kind) {
    switch (kind) {
        case QueueKind::none:
            return "none";
        case QueueKind::node:
            return "node";
        case QueueKind::region:
            return "region";
        case QueueKind::search:
            return "search";
    }
    return "none";
}

inline std::string queue_range_name(QueueKind kind, std::string_view action) {
    std::string result = "q.";
    result += queue_kind_name(kind);
    result += ".";
    result += action;
    return result;
}

inline void record_queue_valid(QueueKind kind, bool valid) {
    update_stats([&](ShotStats &stats) {
        stats.record_queue_validity(kind, valid);
    });
}

inline void record_queue_valid(bool valid) {
    record_queue_valid(QueueKind::none, valid);
}

inline void record_queue_deferred_requeue(QueueKind kind) {
    update_stats([&](ShotStats &stats) {
        stats.record_queue_deferred_requeue(kind);
    });
}

inline void record_queue_deferred_requeue() {
    record_queue_deferred_requeue(QueueKind::none);
}

inline void record_queue_enqueue(QueueKind kind, uint64_t bucket) {
    update_stats([&](ShotStats &stats) {
        stats.record_queue_enqueue(kind, bucket);
    });
}

inline void record_queue_dequeue(QueueKind kind) {
    update_stats([&](ShotStats &stats) {
        stats.record_queue_dequeue(kind);
    });
}

inline void record_queue_remove(QueueKind kind) {
    update_stats([&](ShotStats &stats) {
        stats.record_queue_remove(kind);
    });
}

inline void record_queue_redistribution(uint64_t bucket, uint64_t events) {
    update_stats([&](ShotStats &stats) {
        stats.record_queue_redistribution(bucket, events);
    });
}

inline void record_region_created() {
    update_stats([&](ShotStats &stats) {
        stats.record_region_created();
    });
}

inline void record_region_destroyed() {
    update_stats([&](ShotStats &stats) {
        stats.record_region_destroyed();
    });
}

inline void record_region_reset() {
    update_stats([&](ShotStats &stats) {
        stats.record_region_reset();
    });
}

inline void record_region_shell_area(uint64_t shell_area_size) {
    update_stats([&](ShotStats &stats) {
        stats.record_region_shell_area(shell_area_size);
    });
}

inline void record_alt_tree_nodes_active(uint64_t count) {
    update_stats([&](ShotStats &stats) {
        stats.record_alt_tree_nodes_active(count);
    });
}

inline void record_alt_tree_nodes_reset() {
    update_stats([&](ShotStats &stats) {
        stats.record_alt_tree_nodes_reset();
    });
}

inline void record_alt_tree_nodes_per_tree(uint64_t count) {
    update_stats([&](ShotStats &stats) {
        stats.record_alt_tree_nodes_per_tree(count);
    });
}

#if BB_ENABLE_PROFILE && BB_ENABLE_NVTX

struct [[nodiscard]] ScopedRange {
   private:
    bool active = false;
    std::string owned_name;

    void push(RangeId id) {
#if BB_ENABLE_PROFILE && BB_ENABLE_NVTX
        auto session = active_session();
        if (!session || !session->trace_enabled(range_trace_level(id))) {
            return;
        }
        nvtxEventAttributes_t event{};
        event.version = NVTX_VERSION;
        event.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        event.colorType = NVTX_COLOR_ARGB;
        event.color = range_color(id);
        event.messageType = NVTX_MESSAGE_TYPE_REGISTERED;
        event.message.registered = registered_range_name(id);
        nvtxDomainRangePushEx(nvtx_domain(), &event);
        active = true;
#else
        (void)id;
#endif
    }

    void push(TraceLevel minimum, const char *name, uint32_t color) {
#if BB_ENABLE_PROFILE && BB_ENABLE_NVTX
        auto session = active_session();
        if (!session || !session->trace_enabled(minimum)) {
            return;
        }
        nvtxEventAttributes_t event{};
        event.version = NVTX_VERSION;
        event.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        event.colorType = NVTX_COLOR_ARGB;
        event.color = color;
        event.messageType = NVTX_MESSAGE_TYPE_ASCII;
        event.message.ascii = name;
        nvtxDomainRangePushEx(nvtx_domain(), &event);
        active = true;
#else
        (void)minimum;
        (void)name;
        (void)color;
#endif
    }

    void pop() {
#if BB_ENABLE_PROFILE && BB_ENABLE_NVTX
        if (active) {
            nvtxDomainRangePop(nvtx_domain());
            active = false;
        }
#endif
    }

   public:
    ScopedRange() = default;
    ScopedRange(const ScopedRange &) = delete;
    ScopedRange &operator=(const ScopedRange &) = delete;

    ScopedRange(ScopedRange &&other) noexcept = delete;
    ScopedRange &operator=(ScopedRange &&other) noexcept = delete;

    explicit ScopedRange(RangeId id) {
        push(id);
    }

    ScopedRange(TraceLevel minimum, const char *name, uint32_t color = Color::Blue) {
        push(minimum, name, color);
    }

    ScopedRange(TraceLevel minimum, std::string name, uint32_t color = Color::Blue) : owned_name(std::move(name)) {
        push(minimum, owned_name.c_str(), color);
    }

    ~ScopedRange() {
        pop();
    }
};

#else

// A disabled range must have no state or destructor work. In particular, an
// unused std::string member can survive optimization around indirect calls.
// Statistics are independent: counted_range/updated_range still record them.
struct [[nodiscard]] ScopedRange {
    ScopedRange() = default;
    ScopedRange(const ScopedRange &) = delete;
    ScopedRange &operator=(const ScopedRange &) = delete;
    ScopedRange(ScopedRange &&) noexcept = delete;
    ScopedRange &operator=(ScopedRange &&) noexcept = delete;

    explicit constexpr ScopedRange(RangeId) noexcept {
    }

    constexpr ScopedRange(TraceLevel, const char *, uint32_t = Color::Blue) noexcept {
    }

    constexpr ScopedRange(TraceLevel, const std::string &, uint32_t = Color::Blue) noexcept {
    }

    ~ScopedRange() = default;
};

#endif

[[nodiscard]] inline ScopedRange counted_range(RangeId id, CounterMember counter, uint64_t amount = 1) {
    record_counter(counter, amount);
    return ScopedRange(id);
}

// Use updated_range for one range that updates multiple counters or histograms.
template <typename Update>
[[nodiscard]] inline ScopedRange updated_range(RangeId id, Update &&update) {
    update_stats(std::forward<Update>(update));
    return ScopedRange(id);
}

[[nodiscard]] inline ScopedRange queue_scoped_range(QueueKind kind, std::string_view action, uint32_t color) {
#if BB_ENABLE_PROFILE && BB_ENABLE_NVTX
    if (!trace_enabled(TraceLevel::queue)) {
        return {};
    }
    return ScopedRange(TraceLevel::queue, queue_range_name(kind, action), color);
#else
    (void)kind;
    (void)action;
    (void)color;
    return {};
#endif
}

}  // namespace beyond_blossom::profile

#endif  // BB_SPARSE_BLOSSOM_PROFILE_PROFILE_H
