// Collection-only setup helper. Keep the original specification fields and checks.
// The two original loops called the linear DEM count_detectors() in each condition.
#ifndef STENCIL_GENERIC_ONLY
beyond_blossom::StaticGridSpec frozen_grid_spec_from_dem(
    const stim::DetectorErrorModel& dem, size_t distance, size_t rounds, bool rotated, beyond_blossom::GridSimd simd) {
    beyond_blossom::StaticGridSpec spec{distance, rounds, rotated, {}, {}, simd};
    const auto count = dem.count_detectors();
    std::set<uint64_t> ids;
    for (size_t k = 0; k < count; ++k) ids.insert(k);
    auto coordinates = dem.get_detector_coordinates(ids);
    for (size_t k = 0; k < count; ++k) {
        const auto& values = coordinates[k];
        if (values.size() < 3)
            throw std::invalid_argument("Static grid requires three coordinates for every detector.");
        std::array<int64_t, 3> p;
        for (size_t axis = 0; axis < 3; ++axis) {
            double value = values[axis];
            if (!std::isfinite(value) || value != std::floor(value) || std::abs(value) > double(int64_t{1} << 30))
                throw std::invalid_argument("Static grid requires finite, bounded integer coordinates.");
            p[axis] = int64_t(value);
        }
        spec.coordinates.push_back(p);
    }
    return spec;
}
#endif

// Constructor capacities, collected before warmup. This payload remains allocated
// when the current shot takes either serial fallback route.
template <typename OptionsT, typename Decoder>
void persist_private_storage(const OptionsT& options, const Decoder& decoder) {
    std::ofstream out(options.output / "storage.json");
    out << "{\"schema_version\":1,\"scope\":\"after construction, before warmup; private partition active/radii payload only\","
        << "\"execution\":\"" << options.execution << "\",\"configured_threads\":" << options.cpus.size()
        << ",\"exclusions\":[\"vector object headers\",\"allocator overhead\",\"shared arrays and metadata\","
        << "\"seeds and scratch vectors\",\"queues, regions and trees\"],\"shared_arrays\":\"not directly counted\",";
    if constexpr (requires { decoder.predecoder(size_t{}).storage(size_t{}); }) {
        size_t total = 0;
        out << "\"status\":\"measured_capacity\",\"components\":[";
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            if (c) out << ',';
            out << "{\"component_id\":" << c << ",\"partitions\":[";
            size_t lanes = 0, component_total = 0;
            for (;; ++lanes) {
                // Only the bounds-checked getter is in the catch scope.
                const auto* workspaces = [&]() {
                    try { return &decoder.predecoder(c).storage(lanes); }
                    catch (const std::out_of_range&) { return decltype(&decoder.predecoder(c).storage(lanes)){nullptr}; }
                }();
                if (!workspaces) break;
                if (lanes) out << ',';
                out << "{\"partition_id\":" << lanes << ",\"workspaces\":[";
                size_t partition_total = 0;
                for (size_t g = 0; g < workspaces->size(); ++g) {
                    const auto& w = workspaces->at(g);
                    const auto active_bytes = w.active.capacity() * sizeof(w.active[0]);
                    const auto radius_bytes = w.radii.capacity() * sizeof(w.radii[0]);
                    if (g) out << ',';
                    out << "{\"grid_id\":" << g << ",\"grid_cells\":" << w.grid->kinds.size()
                        << ",\"nx\":" << w.grid->nx << ",\"ny\":" << w.grid->ny << ",\"nt\":" << w.grid->nt
                        << ",\"active_size\":" << w.active.size() << ",\"active_capacity\":" << w.active.capacity()
                        << ",\"active_element_bytes\":" << sizeof(w.active[0]) << ",\"active_payload_bytes\":" << active_bytes
                        << ",\"radii_size\":" << w.radii.size() << ",\"radii_capacity\":" << w.radii.capacity()
                        << ",\"radii_element_bytes\":" << sizeof(w.radii[0]) << ",\"radii_payload_bytes\":" << radius_bytes << '}';
                    partition_total += active_bytes + radius_bytes;
                }
                out << "],\"private_payload_bytes\":" << partition_total << '}';
                component_total += partition_total;
            }
            out << "],\"allocated_partitions\":" << lanes << ",\"private_payload_bytes\":" << component_total << '}';
            total += component_total;
        }
        out << "],\"private_payload_bytes\":" << total;
    } else {
        out << "\"status\":\"not_applicable\",\"reason\":\"decoder has no partition-private workspace API\",\"private_payload_bytes\":null";
    }
    out << "}\n";
    if (!out) throw std::runtime_error("Storage receipt write failed");
}
