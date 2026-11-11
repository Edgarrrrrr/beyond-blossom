inline void AltTreeNode::become_root() {
    // Performs a tree rotation turning this node into the root of the tree/
    if (!parent.alt_tree_node)
        return;
    auto old_parent = parent.alt_tree_node;
    old_parent->become_root();
    parent.alt_tree_node->inner_region = inner_region;
    parent.alt_tree_node->inner_to_outer_edge = parent.edge;
    inner_region = NO_REGION;
    unstable_erase(parent.alt_tree_node->children, [&](const AltTreeEdge &x) {
        return x.alt_tree_node == this;
    });
    parent = AltTreeEdge();
    add_child(AltTreeEdge(old_parent, inner_to_outer_edge.reversed()));
    inner_to_outer_edge = CompressedEdge{nullptr, nullptr, 0};
}
