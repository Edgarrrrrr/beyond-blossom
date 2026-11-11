    if (mwpm.flooder.negative_weight_detection_events.empty()) {
        // Just add detection events if graph has no negative weights
#if PYMATCHING_ENABLE_PROFILE
        pm::profile::ScopedRange create_range(pm::profile::RangeId::CreateDetectionEvents);
#endif
        for (auto& detection : detection_events) {
            if (detection >= mwpm.flooder.graph.nodes.size())
                throw std::invalid_argument(
                    "The detection event with index " + std::to_string(detection) +
                    " does not correspond to a node in the graph, which only has " +
                    std::to_string(mwpm.flooder.graph.nodes.size()) + " nodes.");
            if (detection + 1 > mwpm.flooder.graph.is_user_graph_boundary_node.size() ||
                !mwpm.flooder.graph.is_user_graph_boundary_node[detection])
                mwpm.create_detection_event(&mwpm.flooder.graph.nodes[detection]);
