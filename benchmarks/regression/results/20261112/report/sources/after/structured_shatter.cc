inline void Mwpm::shatter_descendants_into_matches_and_freeze(AltTreeNode &alt_tree_node) {
    for (auto &child_edge : alt_tree_node.children) {
        shatter_descendants_into_matches_and_freeze(*child_edge.alt_tree_node);
    }
    if (alt_tree_node.inner_region) {
        alt_tree_node.parent = AltTreeEdge();
        alt_tree_node.inner()->add_match(alt_tree_node.outer(), alt_tree_node.inner_to_outer_edge);
        flooder.set_region_frozen(*alt_tree_node.inner());
        flooder.set_region_frozen(*alt_tree_node.outer());
        alt_tree_node.inner()->alt_tree_node = nullptr;
        alt_tree_node.outer()->alt_tree_node = nullptr;
    }
    if (alt_tree_node.outer_region) {
        alt_tree_node.outer()->alt_tree_node = nullptr;
    }
    node_arena.del(&alt_tree_node);
}
