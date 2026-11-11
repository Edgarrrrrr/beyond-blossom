    size_t seed_index(const DetectorNode* node) const noexcept {
        return node ? active[size_t(node - nodes)] : 0;
    }
