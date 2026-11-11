inline void Mwpm::handle_tree_hitting_boundary(const RegionHitBoundaryEventData &event) {
    auto range = beyond_blossom::profile::counted_range(
        beyond_blossom::profile::RangeId::MatchBoundary, &beyond_blossom::profile::ShotStats::matcher_tree_hit_boundary);
    auto node = region_for(event.region)->alt_tree_node;
    node->become_root();
    // Match descendents, deleting AltTreeNodes and freezing GraphFillRegions
    shatter_descendants_into_matches_and_freeze(*node);

    // Now match the event region to the boundary and freeze
    region_for(event.region)->match = Match{NO_REGION, event.edge};
    flooder.set_region_frozen(*region_for(event.region));
    assert(num_active_trees > 0);
    --num_active_trees;
}
