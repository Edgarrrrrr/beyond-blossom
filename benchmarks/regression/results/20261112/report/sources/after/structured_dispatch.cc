inline void Mwpm::process_event(const MwpmEvent &event) {
    auto range =
        beyond_blossom::profile::counted_range(beyond_blossom::profile::RangeId::PrimalPhase, &beyond_blossom::profile::ShotStats::primal_phase_runs);
    record_primal_tree_snapshot();
    switch (event.event_type) {
        case REGION_HIT_REGION:
            handle_region_hit_region(event);
            break;
        case REGION_HIT_BOUNDARY:
            handle_tree_hitting_boundary(event.region_hit_boundary_event_data);
            break;
        case BLOSSOM_SHATTER:
            handle_blossom_shattering(event.blossom_shatter_event_data);
            break;
        case NO_EVENT:
            // Do nothing.
            break;
        default:
            throw std::invalid_argument("Unrecognized event type");
    }
    record_primal_tree_snapshot();
}
