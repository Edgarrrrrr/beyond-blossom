void Mwpm::import_predecode_state() {
    if (predecoder.skipped) {
        // prepare() already validated and normalized these events. A dispatch
        // fallback needs no radius/matching import or second syndrome pass.
        for (const auto& seed : predecoder.seeds)
            create_detection_event(seed.node);
        return;
    }
    // Publish every singleton before scheduling any event: a provisional
    // match must remain visible to later alternating trees and blossoms.
    for (const auto& seed : predecoder.seeds) {
        auto* region = flooder.region_arena.alloc_default_constructed();
        auto* node = seed.node;
        region->radius = VaryingCT::from_base_and_growth(
            seed.radius, seed.mate == OneHopPredecoder::UNMATCHED ? +1 : 0);
        region->shell_area.push_back(node);
        node->reached_from_source = node;
        node->radius_of_arrival = 0;
        node->region_that_arrived = region;
        node->region_that_arrived_top = region;
        node->wrapped_radius_cached = 0;
#if BB_ENABLE_PROFILE
        beyond_blossom::profile::record_counter(&beyond_blossom::profile::ShotStats::detection_regions_created);
        beyond_blossom::profile::record_region_created();
        beyond_blossom::profile::record_region_shell_area(1);
#endif
    }
    for (const auto& seed : predecoder.seeds) {
        auto* region = seed.node->region_that_arrived;
        if (seed.mate == OneHopPredecoder::UNMATCHED) {
            auto* root = node_arena.alloc_unconstructed();
            new (root) AltTreeNode(region);
            region->alt_tree_node = root;
            flooder.reschedule_events_at_detector_node(*seed.node);
        } else {
            auto* mate = seed.mate == OneHopPredecoder::BOUNDARY ? nullptr : predecoder.seeds[seed.mate].node;
            region->match = {mate ? mate->region_that_arrived : nullptr,
                             {seed.node, mate, seed.match_observables}};
        }
    }
#if BB_ENABLE_PROFILE
    record_primal_tree_snapshot();
#endif
}
