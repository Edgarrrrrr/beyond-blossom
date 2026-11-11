// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/structured/driver/component_decoder.h"

#include "beyond_blossom/driver/backend_user_graph.h"

namespace beyond_blossom::structured {

ComponentDecoder::ComponentDecoder(
    BackendUserGraph& graph,
    const StaticGridSpec& spec,
    weight_int num_distinct_weights,
    bool ensure_search_graph_included,
    bool predecode_enabled)
    : detector_addresses(graph.nodes.size()),
      observable_count(graph.get_num_observables()),
      search_graph_included(ensure_search_graph_included || observable_count > sizeof(obs_int) * 8),
      predecode_enabled_(predecode_enabled) {
    const size_t count = graph.nodes.size();
    if (!count || spec.coordinates.size() != count || count >= (INT32_MAX - 2) / 2 ||
        (!spec.groups.empty() && spec.groups.size() != count))
        throw std::invalid_argument("Static grid requires coordinates and optional groups for every detector.");

    std::vector<size_t> roots(count);
    std::iota(roots.begin(), roots.end(), 0);
    auto find = [&](size_t u) {
        while (u != roots[u]) {
            roots[u] = roots[roots[u]];
            u = roots[u];
        }
        return u;
    };
    for (const auto& edge : graph.edges) {
        if (graph.is_boundary_node(edge.node1) || graph.is_boundary_node(edge.node2))
            continue;
        if (!spec.groups.empty() && spec.groups[edge.node1] != spec.groups[edge.node2])
            throw std::invalid_argument("An ordinary edge connects different X/Z groups.");
        size_t a = find(edge.node1), b = find(edge.node2);
        roots[std::max(a, b)] = std::min(a, b);
    }
    std::map<size_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < count; i++) {
        if (!graph.is_boundary_node(i))
            groups[find(i)].push_back(i);
    }
    if (groups.empty() || groups.size() > 2)
        throw std::invalid_argument("Static surface grid needs one or two nonempty X/Z groups.");

    std::vector<MatchingGraph> matching_graphs;
    std::vector<SearchGraph> search_graphs;
    std::vector<StaticGridSpec> specifications;
    component_decoders.reserve(groups.size());
    matching_graphs.reserve(groups.size());
    search_graphs.reserve(groups.size());
    for (const auto& [root, ids] : groups) {
        const size_t label = spec.groups.empty() ? root : spec.groups[ids.front()];
        const size_t component = component_decoders.size();
        component_decoders.push_back({Mwpm(), ids});
        StaticGridSpec local_spec{spec.distance, spec.rounds, spec.rotated, {}, {}, spec.simd};
        for (size_t local = 0; local < ids.size(); local++) {
            detector_addresses[ids[local]] = {.component = uint16_t(component), .local = uint32_t(local)};
            local_spec.coordinates.push_back(spec.coordinates[ids[local]]);
            local_spec.groups.push_back(static_cast<uint32_t>(label));
        }
        specifications.push_back(std::move(local_spec));
        matching_graphs.emplace_back(ids.size(), observable_count);
        if (search_graph_included)
            search_graphs.emplace_back(ids.size());
    }

    // One global quantization and boundary-edge merge, before any component is
    // constructed. Per-component scales would change matching and weight sums.
    weight_scale = graph.to_matching_or_search_graph_helper(
        num_distinct_weights,
        [&](size_t u, size_t v, signed_weight_int weight, const auto& observables, const auto&) {
            const auto a = detector_addresses[u], b = detector_addresses[v];
            matching_graphs[a.component].add_edge(a.local, b.local, weight, observables);
            if (search_graph_included)
                search_graphs[a.component].add_edge(a.local, b.local, weight, observables);
        },
        [&](size_t u, signed_weight_int weight, const auto& observables, const auto&) {
            const auto a = detector_addresses[u];
            component_decoders[a.component].has_boundary = true;
            matching_graphs[a.component].add_boundary_edge(a.local, weight, observables);
            if (search_graph_included)
                search_graphs[a.component].add_boundary_edge(a.local, weight, observables);
        });
    for (size_t c = 0; c < component_decoders.size(); c++) {
        auto& matching_graph = matching_graphs[c];
        matching_graph.normalising_constant = weight_scale;
        matching_graph.loaded_from_dem_without_correlations = true;
        matching_graph.configure_grid(specifications[c]);
        if (search_graph_included) {
            search_graphs[c].configure_grid(specifications[c]);
            component_decoders[c].mwpm =
                Mwpm(GraphFlooder(std::move(matching_graph)), SearchFlooder(std::move(search_graphs[c])));
        } else {
            component_decoders[c].mwpm = Mwpm(GraphFlooder(std::move(matching_graph)));
        }
        component_decoders[c].mwpm.configure_predecode(predecode_enabled_);
        auto& flooder = component_decoders[c].mwpm.flooder;
        flooder.sync_negative_weight_observables_and_detection_events();
        has_negative_detection_events |= !flooder.negative_weight_detection_events.empty();
        // Every local spec has a single fixed group label. Bind its one grid
        // once so shot construction need not read mutable detector nodes.
        if (flooder.graph.grid->components.size() != 1)
            throw std::logic_error("A decoder component must own exactly one static grid.");
        auto* grid = flooder.graph.grid->components.front().get();
        grid->decoder_component = uint32_t(c);
        component_decoders[c].seed_grid = grid;
        for (size_t global : component_decoders[c].global_detectors) {
            auto& address = detector_addresses[global];
            const auto& node = flooder.graph.node(address.local);
            if (node.grid_cell > UINT32_MAX)
                throw std::length_error("Static grid cell exceeds the seed address range.");
            address.cell = uint32_t(node.grid_cell);
        }
    }
}

void ComponentDecoder::validate_correlations(bool enabled) {
    if (enabled)
        throw std::invalid_argument(
            "The independent static-grid backend does not support enable_correlations=True. "
            "Disable the static grid to use correlated decoding with the generic backend.");
}

void ComponentDecoder::reset() {
    prefix_ready = false;
    for (auto& component : component_decoders) {
        component.mwpm.reset();
        component.mwpm.search_flooder.reset();
        component.mwpm.flooder.match_edges.clear();
    }
    clear_shot();
    match_edges.clear();
}

void ComponentDecoder::clear_shot() {
    for (auto& component : component_decoders) {
        // A caller may start another shot without an explicit post-shot reset.
        // Retire deferred node fields before losing their seed span/index.
        component.mwpm.clear_pending_seed_nodes();
        component.mwpm.flooder.regions->cancel_neutral_reset();
        component.mwpm.predecoder.reset();
        component.seed_begin = component.seed_end = 0;
    }
    // Only legacy append/toggle can touch cells before publishing spans.
    // Staged retained records may be stale and must never be dereferenced.
    if (seed_storage_stage == SeedStorageStage::legacy_partial) {
        for (const auto& seed : std::span(shot_seeds).first(fixed_seed_storage ? logical_seed_count : shot_seeds.size())) {
            seed.grid->predecode.active[seed.cell] = 0;
            seed.grid->predecode.radii[seed.cell] = 0;
        }
    }
    retire_shot_storage();
}

void ComponentDecoder::retire_shot_storage() noexcept {
    if (seed_storage_stage != SeedStorageStage::retired) {
        assert(retired_shot_seeds.empty() && retired_shot_seeds.capacity() == 0);
        shot_seeds.swap(retired_shot_seeds);
        seed_storage_stage = SeedStorageStage::retired;
    }
    staged_partition_cached = false;
    if (fixed_seed_input_indexed) {
        fixed_index_counts = {};
#if BB_STRUCTURED_PROFILE_TIMERS
        fixed_index_classification_visits = fixed_index_entries_written = 0;
#endif
    }
    shot_clean = true;
}

void ComponentDecoder::acquire_seed_storage(SeedStorageStage stage) {
    if (seed_storage_generation == UINT64_MAX)
        throw std::overflow_error("Seed storage generation exhausted.");
    ++seed_storage_generation;
    assert(seed_storage_stage == SeedStorageStage::retired);
    assert(shot_seeds.empty() && shot_seeds.capacity() == 0);
    shot_seeds.swap(retired_shot_seeds);
    seed_storage_stage = stage;
    logical_seed_count = 0;
    if (stage == SeedStorageStage::legacy_partial && !fixed_seed_storage)
        shot_seeds.clear();
}

template <typename Count>
bool ComponentDecoder::count_validated_shot(const std::vector<uint64_t>& events, Count&& count) {
    // Failed positive-input attempts also consume an epoch. Ordered inputs
    // need no sparse stamp writes; unsorted inputs keep their original order.
    if (++shot_epoch == 0) {
        for (auto& address : detector_addresses)
            address.last_seen_epoch = 0;
        shot_epoch = 1;
    }
    bool ordered = true, first = true;
    uint64_t previous = 0;
    for (auto event : events) {
        if (event >= detector_addresses.size())
            throw std::invalid_argument("Detection event index " + std::to_string(event) + " is not in the graph.");
        ordered &= first || previous < event;
        first = false;
        previous = event;
        const auto& address = detector_addresses[event];
        if (address.component != NO_COMPONENT) count(address);
    }
    if (!ordered) {
        bool duplicate = false;
        for (auto event : events) {
            auto& address = detector_addresses[event];
            if (address.component == NO_COMPONENT)
                continue;
            duplicate |= address.last_seen_epoch == shot_epoch;
            address.last_seen_epoch = shot_epoch;
        }
        // Full range validation precedes duplicate rejection, as before.
        if (duplicate)
            throw std::invalid_argument("Duplicate detection event in predecoder input.");
    }
    return ordered;
}

void ComponentDecoder::scatter_shot(const std::vector<uint64_t>& events, bool build_index) {
    (this->*scatter_entry_)(events, build_index);
}

void ComponentDecoder::scatter_dynamic_shot(const std::vector<uint64_t>& events, bool build_index) {
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope count(profile.seed_count_ns BB_TIMELINE_ARG(&profile.seed_count_interval));
#endif
        count_validated_shot(events, [&](const DetectorAddress& address) {
            ++component_decoders[address.component].seed_end;
        });
    }
    size_t total = 0;
    std::array<size_t, 2> next{};
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope layout(profile.seed_layout_ns BB_TIMELINE_ARG(&profile.seed_layout_interval));
#endif
        for (size_t c = 0; c < component_decoders.size(); ++c) {
            auto& component = component_decoders[c];
            component.seed_begin = total;
            total += component.seed_end;
            component.seed_end = total;
            next[c] = component.seed_begin;
        }
        // Unique validated global IDs are bounded by the constructor's
        // count < (INT32_MAX-2)/2 guard, so local active indices fit uint32_t.
        assert(total <= detector_addresses.size());
        acquire_seed_storage(SeedStorageStage::staged);
        if (!fixed_seed_storage) shot_seeds.resize(total);
        assert(total <= shot_seeds.size());
        logical_seed_count = total;
    }
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope scatter(profile.seed_scatter_ns BB_TIMELINE_ARG(&profile.seed_scatter_interval));
#endif
        for (auto event : events) {
            const auto& address = detector_addresses[event];
            if (address.component == NO_COMPONENT)
                continue;
            const auto slot = next[address.component]++;
            PredecodeSeed seed{address.local, address.cell};
            seed.grid = component_decoders[address.component].seed_grid;
            shot_seeds[slot] = seed;
#if BB_STRUCTURED_PROFILE_COUNTERS
            ++component_decoders[address.component].mwpm.predecoder.stats.input_events;
#endif
        }
    }
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope bind(profile.seed_bind_ns BB_TIMELINE_ARG(&profile.seed_bind_interval));
#endif
        for (auto& component : component_decoders)
            component.mwpm.predecoder.bind_seeds(
                std::span<PredecodeSeed>(shot_seeds)
                    .subspan(component.seed_begin, component.seed_end - component.seed_begin),
                build_index);
        seed_storage_stage = SeedStorageStage::published;
    }
}

void ComponentDecoder::bind_partition_buckets(size_t partitions, size_t conflict_width, TimePartitionCuts::Geometry geometry) {
    if (seed_storage_stage == SeedStorageStage::staged)
        throw std::logic_error("Cannot rebind partition buckets while a shot is staged.");
    if (fixed_seed_storage) {
        if (partitions == bound_partition_count && conflict_width == bound_conflict_width && geometry == bound_conflict_geometry)
            return;
        throw std::logic_error("Cannot change fixed seed capacities after constructor binding.");
    }
    if ((partitions != 2 && partitions != 4 && partitions != 8) || component_decoders.empty() || component_decoders.size() > 2)
        throw std::invalid_argument("Cached partition buckets require one or two components and P2, P4, or P8.");
    // Validate every geometry before changing membership or binding metadata.
    // No borrowed cuts pointers survive this constructor-time operation.
    std::array<TimePartitionCuts, 2> cuts;
    for (size_t c = 0; c < component_decoders.size(); ++c) {
        const auto& grid = *component_decoders[c].seed_grid;
        cuts[c] = TimePartitionCuts(grid.nx, grid.ny, grid.nt, partitions, conflict_width, geometry);
    }
    bound_detector_ranges = {};
    for (size_t global = 0; global < detector_addresses.size(); ++global) {
        auto& address = detector_addresses[global];
        address.bucket = address.component == NO_COMPONENT ? NO_BUCKET
            : uint16_t(cuts[address.component].bucket(address.cell));
        if (address.component != NO_COMPONENT) {
            auto& range = bound_detector_ranges[address.component][address.bucket];
            range.min = std::min(range.min, uint64_t(global));
            range.max = std::max(range.max, uint64_t(global));
        }
    }
    bound_partition_count = partitions;
    bound_conflict_width = conflict_width;
    bound_conflict_geometry = geometry;
}

void ComponentDecoder::bind_fixed_seed_storage() {
    if (fixed_seed_storage) return;
    assert(!fixed_seed_storage && shot_clean && supports_owner_scatter() && bound_partition_count);
    std::array<std::array<size_t, 10>, 2> bases{};
    for (const auto& address : detector_addresses)
        if (address.component != NO_COMPONENT) ++bases[address.component][address.bucket + 1];
    size_t total = 0;
    for (size_t c = 0; c < component_decoders.size(); ++c) {
        for (size_t b = 0; b <= bound_partition_count; ++b) bases[c][b + 1] += bases[c][b];
        const size_t capacity = bases[c][bound_partition_count + 1];
        if (capacity >= PredecodeSeed::BOUNDARY || capacity > SIZE_MAX - 9 * 64 ||
            capacity > retired_shot_seeds.max_size() || total > retired_shot_seeds.max_size() - capacity)
            throw std::length_error("Fixed seed capacity exceeds its index or allocation range.");
        fixed_component_bases[c] = total;
        total += capacity;
    }
    fixed_component_bases[component_decoders.size()] = total;
    retired_shot_seeds.resize(total);
    for (size_t c = 0; c < component_decoders.size(); ++c)
        component_decoders[c].mwpm.predecoder.bind_fixed_scratch(
            std::span<const size_t>(bases[c]).first(bound_partition_count + 2));
    fixed_bucket_bases = bases;
    fixed_seed_storage = true;
}

bool ComponentDecoder::initialize_serial_seed_storage() {
    if (serial_seed_storage) return true;
    if (has_negative_detection_events || fixed_seed_storage || bound_partition_count || component_decoders.empty())
        return false;
    if (!shot_clean || prefix_ready || seed_storage_generation)
        throw std::logic_error("Configure serial seed storage before the first shot.");

    std::array<size_t, 3> bases{};
    for (size_t c = 0; c < component_decoders.size(); ++c)
        bases[c + 1] = bases[c] + component_decoders[c].global_detectors.size();
    // All allocations precede publication. A failed allocation leaves the
    // original dynamic decoder usable; already-bound scratch is also valid
    // for dense serial decoding and can be reused by a later attempt.
    std::vector<PredecodeSeed> backing(bases[component_decoders.size()]);
    std::vector<uint32_t> ids(backing.size());
    std::vector<uint8_t> routes(detector_addresses.size(), UINT8_MAX);
    for (size_t global = 0; global < detector_addresses.size(); ++global)
        if (detector_addresses[global].component != NO_COMPONENT)
            routes[global] = uint8_t(detector_addresses[global].component);
    for (size_t c = 0; c < component_decoders.size(); ++c)
        component_decoders[c].mwpm.predecoder.bind_serial_scratch(bases[c + 1] - bases[c]);
    retired_shot_seeds = std::move(backing);
    serial_detector_ids = std::move(ids);
    serial_detector_routes = std::move(routes);
    fixed_component_bases = bases;
    fixed_seed_storage = serial_seed_storage = true;
    scatter_entry_ = &ComponentDecoder::scatter_serial_shot;
    return true;
}

std::array<size_t, 2> ComponentDecoder::classify_serial_shot(const std::vector<uint64_t>& events) {
    assert(serial_seed_storage);
    if (++shot_epoch == 0) {
        for (auto& address : detector_addresses) address.last_seen_epoch = 0;
        shot_epoch = 1;
    }
    auto next = fixed_component_bases;
    const auto append = [&](uint64_t event) {
        const auto component = serial_detector_routes[event];
        if (component == UINT8_MAX) return;
        const size_t slot = next[component]++;
        assert(slot < fixed_component_bases[component + 1]);
        serial_detector_ids[slot] = uint32_t(event);
    };
    for (size_t i = 0; i < events.size(); ++i) {
        const auto event = events[i];
        if (event >= detector_addresses.size())
            throw std::invalid_argument("Detection event index " + std::to_string(event) + " is not in the graph.");
        if (i && events[i - 1] >= event) {
            // Validate the full input before a duplicate could overrun a
            // component's fixed prefix. Preserve range-before-duplicate errors
            // and the input order of valid unsorted events.
            validate_group_shot(events, false);
            next = fixed_component_bases;
            for (auto valid_event : events) append(valid_event);
            break;
        }
        append(event);
    }
    std::array<size_t, 2> counts{};
    for (size_t c = 0; c < component_decoders.size(); ++c)
        counts[c] = next[c] - fixed_component_bases[c];
    return counts;
}

void ComponentDecoder::scatter_serial_shot(const std::vector<uint64_t>& events, bool build_index) {
    std::array<size_t, 2> counts{};
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope count(profile.seed_count_ns BB_TIMELINE_ARG(&profile.seed_count_interval));
#endif
        counts = classify_serial_shot(events);
    }
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope layout(profile.seed_layout_ns BB_TIMELINE_ARG(&profile.seed_layout_interval));
#endif
        acquire_seed_storage(SeedStorageStage::staged);
        assert(shot_seeds.size() == serial_seed_storage_capacity());
    }
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope scatter(profile.seed_scatter_ns BB_TIMELINE_ARG(&profile.seed_scatter_interval));
#endif
        for (size_t c = 0; c < component_decoders.size(); ++c) {
            auto& component = component_decoders[c];
            component.seed_begin = fixed_component_bases[c];
            component.seed_end = component.seed_begin + counts[c];
            auto* seeds = shot_seeds.data() + component.seed_begin;
            auto* grid = component.seed_grid;
            for (size_t local = 0; local < counts[c]; ++local) {
                const auto& address = detector_addresses[serial_detector_ids[component.seed_begin + local]];
                PredecodeSeed seed{address.local, address.cell};
                seed.grid = grid;
                seeds[local] = seed;
                if (build_index) grid->predecode.active[address.cell] = uint32_t(local + 1);
            }
            // Construction and index publication cannot throw. Publish each
            // completed view immediately so reset owns exactly its live cells.
            component.mwpm.predecoder.bind_seeds(std::span(seeds, counts[c]), false);
#if BB_STRUCTURED_PROFILE_COUNTERS
            component.mwpm.predecoder.stats.input_events += counts[c];
#endif
            logical_seed_count += counts[c];
        }
    }
    seed_storage_stage = SeedStorageStage::published;
}

void ComponentDecoder::bind_fixed_seed_index() {
    assert(fixed_seed_storage && shot_clean);
    // The graph constructor already imposes the stricter INT32_MAX guard.
    assert(detector_addresses.size() < UINT32_MAX);
    fixed_bucket_detector_ids.resize(fixed_component_bases[component_decoders.size()]);
    fixed_detector_routes.resize(detector_addresses.size(), UINT8_MAX);
    for (size_t c = 0; c < component_decoders.size(); ++c) {
        for (size_t b = 0; b <= bound_partition_count; ++b) {
            const size_t route = c * 9 + b;
            fixed_index_starts[route] = fixed_component_bases[c] + fixed_bucket_bases[c][b];
            fixed_index_ends[route] = fixed_component_bases[c] + fixed_bucket_bases[c][b + 1];
        }
    }
    for (size_t event = 0; event < detector_addresses.size(); ++event) {
        const auto& address = detector_addresses[event];
        if (address.component != NO_COMPONENT)
            fixed_detector_routes[event] = uint8_t(address.component * 9 + address.bucket);
    }
    fixed_seed_input_indexed = true;
}

bool ComponentDecoder::validate_group_shot(const std::vector<uint64_t>& events, bool advance_epoch) {
    if (advance_epoch && ++shot_epoch == 0) {
        for (auto& address : detector_addresses) address.last_seen_epoch = 0;
        shot_epoch = 1;
    }
    bool ordered = true;
    for (size_t i = 1; i < events.size(); ++i) ordered &= events[i - 1] < events[i];
    if (ordered && (events.empty() || events.back() < detector_addresses.size())) return true;
    // Preserve the first invalid ID and range-before-duplicate error order.
    for (auto event : events)
        if (event >= detector_addresses.size())
            throw std::invalid_argument("Detection event index " + std::to_string(event) + " is not in the graph.");
    bool duplicate = false;
    for (auto event : events) {
        auto& address = detector_addresses[event];
        if (address.component == NO_COMPONENT) continue;
        duplicate |= address.last_seen_epoch == shot_epoch;
        address.last_seen_epoch = shot_epoch;
    }
    if (duplicate) throw std::invalid_argument("Duplicate detection event in predecoder input.");
    return false;
}

bool ComponentDecoder::classify_fixed_group_shot(const std::vector<uint64_t>& events) {
    assert(fixed_seed_input_indexed && fixed_seed_storage);
    fixed_index_counts = {};
#if BB_STRUCTURED_PROFILE_TIMERS
    fixed_index_classification_visits = fixed_index_entries_written = 0;
#endif
    // Consume one epoch even on a failed attempt, exactly as scan validation.
    if (++shot_epoch == 0) {
        for (auto& address : detector_addresses) address.last_seen_epoch = 0;
        shot_epoch = 1;
    }
    auto next = fixed_index_starts;
    const auto append = [&](uint64_t event) {
        const auto route = fixed_detector_routes[event];
        if (route == UINT8_MAX) return;
        const auto slot = next[route]++;
        assert(slot < fixed_index_ends[route]);
        fixed_bucket_detector_ids[slot] = uint32_t(event);
    };
    bool ordered = true;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto event = events[i];
        if (event >= detector_addresses.size())
            throw std::invalid_argument("Detection event index " + std::to_string(event) + " is not in the graph.");
        if (i && events[i - 1] >= event) {
            // No duplicate may overflow a fixed bucket. Validate the complete
            // input before rebuilding scratch, preserving range-before-duplicate
            // errors and ignored detectors' existing duplicate semantics.
            validate_group_shot(events, false);
#if BB_STRUCTURED_PROFILE_TIMERS
            for (size_t route = 0; route < next.size(); ++route)
                fixed_index_entries_written += next[route] - fixed_index_starts[route];
#endif
            next = fixed_index_starts;
            for (auto valid_event : events) append(valid_event);
            ordered = false;
#if BB_STRUCTURED_PROFILE_TIMERS
            // Only attempted/final classification visits: the fallback's
            // additional validation passes are excluded. This is not a count
            // of individual loads or cache events.
            fixed_index_classification_visits = i + 1 + events.size();
#endif
            break;
        }
        append(event);
    }
    for (size_t c = 0; c < component_decoders.size(); ++c)
        for (size_t b = 0; b <= bound_partition_count; ++b)
            fixed_index_counts[c][b] = next[c * 9 + b] - fixed_index_starts[c * 9 + b];
#if BB_STRUCTURED_PROFILE_TIMERS
    if (ordered) fixed_index_classification_visits = events.size();
    for (const auto& counts : fixed_index_counts)
        for (auto count : counts) fixed_index_entries_written += count;
#endif
    return ordered;
}

void ComponentDecoder::scatter_fixed_group(std::span<const uint64_t> events, size_t c, uint32_t mask,
                                          std::array<size_t, 9>& counts) noexcept {
    assert(fixed_seed_storage && seed_storage_stage == SeedStorageStage::staged);
    const auto& bases = fixed_bucket_bases[c];
    std::array<size_t, 10> next = bases;
    auto& component = component_decoders[c];
    auto* seeds = shot_seeds.data() + fixed_component_bases[c];
    auto* grid = component.seed_grid;
    for (auto event : events) {
        const auto& address = detector_addresses[event];
        if (address.component != c || !(mask & (uint32_t{1} << address.bucket))) continue;
        const size_t slot = next[address.bucket]++;
        assert(slot < bases[address.bucket + 1]);
        PredecodeSeed seed{address.local, address.cell};
        seed.grid = grid;
        seeds[slot] = seed;
        grid->predecode.active[address.cell] = uint32_t(slot + 1);
    }
    for (size_t b = 0; b <= bound_partition_count; ++b)
        counts[b] = (mask & (uint32_t{1} << b)) ? next[b] - bases[b] : 0;
}

void ComponentDecoder::scatter_indexed_fixed_group(size_t c, uint32_t mask,
                                                   std::array<size_t, 9>& counts) noexcept {
    assert(fixed_seed_input_indexed && seed_storage_stage == SeedStorageStage::staged);
    const auto& bases = fixed_bucket_bases[c];
    const auto* ids = fixed_bucket_detector_ids.data() + fixed_component_bases[c];
    auto* seeds = shot_seeds.data() + fixed_component_bases[c];
    auto* grid = component_decoders[c].seed_grid;
    counts = {};
    for (size_t b = 0; b <= bound_partition_count; ++b) {
        if (!(mask & (uint32_t{1} << b))) continue;
        const auto end = bases[b] + fixed_index_counts[c][b];
        for (size_t slot = bases[b]; slot < end; ++slot) {
            const auto& address = detector_addresses[ids[slot]];
            PredecodeSeed seed{address.local, address.cell};
            seed.grid = grid;
            seeds[slot] = seed;
            grid->predecode.active[address.cell] = uint32_t(slot + 1);
        }
        counts[b] = fixed_index_counts[c][b];
    }
}

void ComponentDecoder::stage_partitioned_shot(
    const std::vector<uint64_t>& events, std::span<PartitionSeedLayout> layouts, size_t partitions) {
    stage_partitioned_shot_impl<false>(events, layouts, partitions);
}

void ComponentDecoder::stage_bound_partitioned_shot(
    const std::vector<uint64_t>& events, std::span<BoundPartitionSeedLayout> layouts) {
    if (!bound_partition_count || component_decoders.empty())
        throw std::logic_error("Partition bucket cache is not bound to a component decoder.");
    stage_partitioned_shot_impl<true>(events, layouts, bound_partition_count);
}

void ComponentDecoder::stage_group_partitioned_shot(
    const std::vector<uint64_t>& events, std::span<BoundPartitionSeedLayout> layouts) {
    if (!bound_partition_count || component_decoders.empty())
        throw std::logic_error("Partition bucket cache is not bound to a component decoder.");
    if (prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
    assert(fixed_seed_storage && layouts.size() == component_decoders.size());
    if (!shot_clean) clear_shot();
    shot_clean = false;
    try {
        bool ordered;
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope validation(profile.seed_count_ns BB_TIMELINE_ARG(&profile.seed_count_interval));
#endif
            ordered = fixed_seed_input_indexed ? classify_fixed_group_shot(events) : validate_group_shot(events);
        }
        acquire_seed_storage(SeedStorageStage::staged);
        for (size_t c = 0; c < layouts.size(); ++c) {
            layouts[c].offsets = fixed_bucket_bases[c];
            layouts[c].input_ordered = ordered;
            auto& component = component_decoders[c];
            const auto storage_begin = fixed_component_bases[c];
            const auto storage_end = fixed_component_bases[c + 1];
            auto& pre = component.mwpm.predecoder;
            pre.seed_view.bind_fixed(std::span(shot_seeds).subspan(storage_begin, storage_end - storage_begin),
                std::span(fixed_bucket_bases[c]).first(bound_partition_count + 2));
            pre.activate_fixed_scratch();
        }
        staged_partition_cached = true;
    } catch (...) {
        clear_shot();
        throw;
    }
}

template <bool Cached, bool InputBounds, typename Layout>
void ComponentDecoder::stage_partitioned_shot_impl(
    const std::vector<uint64_t>& events, std::span<Layout> layouts, size_t partitions) {
    static_assert(!InputBounds || Cached);
    if (prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
    assert(supports_owner_scatter() && layouts.size() == component_decoders.size());
    assert(partitions == 2 || partitions == 4 || partitions == 8);
    if (!shot_clean)
        clear_shot();
    shot_clean = false;
    try {
        for (auto& layout : layouts) {
            layout.offsets = {};
            if constexpr (InputBounds) layout.input_ordered = false;
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope count(profile.seed_count_ns BB_TIMELINE_ARG(&profile.seed_count_interval));
#endif
            const bool ordered = count_validated_shot(events, [&](const DetectorAddress& address) {
                auto& layout = layouts[address.component];
                const auto bucket = [&] {
                    if constexpr (Cached) return size_t(address.bucket);
                    else return layout.cuts->bucket(address.cell);
                }();
                ++layout.offsets[bucket + 1];
            });
            if constexpr (InputBounds)
                for (auto& layout : layouts) layout.input_ordered = ordered;
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope layout_phase(profile.seed_layout_ns BB_TIMELINE_ARG(&profile.seed_layout_interval));
#endif
            size_t total = 0;
            for (size_t c = 0; c < layouts.size(); ++c) {
                auto& offsets = layouts[c].offsets;
                for (size_t b = 0; b <= partitions; ++b)
                    offsets[b + 1] += offsets[b];
                auto& component = component_decoders[c];
                component.seed_begin = total;
                total += offsets[partitions + 1];
                component.seed_end = total;
            }
            assert(total <= detector_addresses.size());
            acquire_seed_storage(SeedStorageStage::staged);
            if (!fixed_seed_storage) shot_seeds.resize(total);
            assert(total <= shot_seeds.size());
            logical_seed_count = total;
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope bind(profile.seed_bind_ns BB_TIMELINE_ARG(&profile.seed_bind_interval));
#endif
            for (auto& component : component_decoders) {
                component.mwpm.predecoder.bind_seeds_inactive(
                    std::span<PredecodeSeed>(shot_seeds).subspan(
                        component.seed_begin, component.seed_end - component.seed_begin));
#if BB_STRUCTURED_PROFILE_COUNTERS
                component.mwpm.predecoder.stats.input_events = component.seed_end - component.seed_begin;
#endif
            }
        }
        staged_partition_cached = Cached;
    } catch (...) {
        clear_shot();
        throw;
    }
}

template <bool Cached, typename Layout, typename MaskForComponent>
void ComponentDecoder::scatter_partitioned_shot_impl(
    std::span<const uint64_t> events, std::span<const Layout> layouts,
    const MaskForComponent& bucket_mask) noexcept {
    assert(seed_storage_stage == SeedStorageStage::staged && staged_partition_cached == Cached);
    std::array<std::array<size_t, 10>, 2> next{};
    for (size_t c = 0; c < layouts.size(); ++c)
        next[c] = layouts[c].offsets;
    for (auto event : events) {
        const auto& address = detector_addresses[event];
        if (address.component == NO_COMPONENT)
            continue;
        const auto c = address.component;
        const auto bucket = [&] {
            if constexpr (Cached) return size_t(address.bucket);
            else return layouts[c].cuts->bucket(address.cell);
        }();
        if (!(bucket_mask(c) & (uint32_t{1} << bucket)))
            continue;
        const auto local = next[c][bucket]++;
        assert(local < layouts[c].offsets[bucket + 1]);
        auto& component = component_decoders[c];
        PredecodeSeed seed{address.local, address.cell};
        seed.grid = component.seed_grid;
        shot_seeds[component.seed_begin + local] = seed;
        seed.grid->predecode.active[seed.cell] = uint32_t(local + 1);
    }
}

void ComponentDecoder::scatter_partitioned_shot(
    std::span<const uint64_t> events, std::span<const PartitionSeedLayout> layouts, uint32_t bucket_mask) noexcept {
    scatter_partitioned_shot_impl<false>(events, layouts, [bucket_mask](size_t) { return bucket_mask; });
}

void ComponentDecoder::scatter_partitioned_shot(
    std::span<const uint64_t> events, std::span<const PartitionSeedLayout> layouts,
    std::span<const uint32_t> component_bucket_masks) noexcept {
    assert(component_bucket_masks.size() == layouts.size());
    scatter_partitioned_shot_impl<false>(events, layouts, [component_bucket_masks](size_t c) {
        return component_bucket_masks[c];
    });
}

void ComponentDecoder::scatter_bound_partitioned_shot(
    std::span<const uint64_t> events, std::span<const BoundPartitionSeedLayout> layouts,
    uint32_t bucket_mask) noexcept {
    scatter_partitioned_shot_impl<true>(events, layouts, [bucket_mask](size_t) { return bucket_mask; });
}

void ComponentDecoder::scatter_bound_partitioned_shot(
    std::span<const uint64_t> events, std::span<const BoundPartitionSeedLayout> layouts,
    std::span<const uint32_t> component_bucket_masks) noexcept {
    assert(component_bucket_masks.size() == layouts.size());
    scatter_partitioned_shot_impl<true>(events, layouts, [component_bucket_masks](size_t c) {
        return component_bucket_masks[c];
    });
}

void ComponentDecoder::stage_component_shot(const std::vector<uint64_t>& events) {
    if (prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
    if (component_decoders.empty())
        throw std::logic_error("Whole-component staging requires a component decoder.");
    assert(supports_owner_scatter());
    if (!shot_clean)
        clear_shot();
    shot_clean = false;
    try {
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope count(profile.seed_count_ns BB_TIMELINE_ARG(&profile.seed_count_interval));
#endif
            count_validated_shot(events, [&](const DetectorAddress& address) {
                ++component_decoders[address.component].seed_end;
            });
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope layout(profile.seed_layout_ns BB_TIMELINE_ARG(&profile.seed_layout_interval));
#endif
            size_t total = 0;
            for (auto& component : component_decoders) {
                component.seed_begin = total;
                total += component.seed_end;
                component.seed_end = total;
            }
            assert(total <= detector_addresses.size());
            acquire_seed_storage(SeedStorageStage::staged);
            if (!fixed_seed_storage) shot_seeds.resize(total);
            assert(total <= shot_seeds.size());
            logical_seed_count = total;
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope bind(profile.seed_bind_ns BB_TIMELINE_ARG(&profile.seed_bind_interval));
#endif
            for (auto& component : component_decoders) {
                component.mwpm.predecoder.bind_seeds_inactive(
                    std::span<PredecodeSeed>(shot_seeds).subspan(
                        component.seed_begin, component.seed_end - component.seed_begin));
#if BB_STRUCTURED_PROFILE_COUNTERS
                component.mwpm.predecoder.stats.input_events = component.seed_end - component.seed_begin;
#endif
            }
        }
        staged_partition_cached = false;
    } catch (...) {
        clear_shot();
        throw;
    }
}

void ComponentDecoder::scatter_component_shot(std::span<const uint64_t> events, size_t c) noexcept {
    assert(seed_storage_stage == SeedStorageStage::staged && !staged_partition_cached);
    auto& component = component_decoders[c];
    auto seeds = std::span<PredecodeSeed>(shot_seeds).subspan(
        component.seed_begin, component.seed_end - component.seed_begin);
    auto* grid = component.seed_grid;
    size_t next = 0;
    for (auto event : events) {
        const auto& address = detector_addresses[event];
        if (address.component != c)
            continue;
        assert(next < seeds.size());
        PredecodeSeed seed{address.local, address.cell};
        seed.grid = grid;
        seeds[next] = seed;
        grid->predecode.active[address.cell] = uint32_t(++next);
    }
    assert(next == seeds.size());
}

void ComponentDecoder::publish_partitioned_shot() noexcept {
    publish_staged_shot();
}

void ComponentDecoder::publish_staged_shot() noexcept {
    publish_staged_shot_impl<true>();
}

void ComponentDecoder::publish_chained_partitioned_shot() noexcept {
    publish_staged_shot_impl<false>();
}

template <bool PublishCells>
void ComponentDecoder::publish_staged_shot_impl() noexcept {
    assert(seed_storage_stage == SeedStorageStage::staged);
    size_t total = 0;
    for (auto& component : component_decoders) {
        if constexpr (PublishCells) component.mwpm.predecoder.publish_cell_state();
        component.seed_begin = total;
        total += component.mwpm.predecoder.seed_view.active_count();
        component.seed_end = total;
    }
    logical_seed_count = total;
    seed_storage_stage = SeedStorageStage::published;
    staged_partition_cached = false;
}

void ComponentDecoder::finish_shot_build(bool build_index) {
    if (component_decoders.empty())
        return;
    bool has_negative_events = false;
    for (auto& component : component_decoders) {
        auto& flooder = component.mwpm.flooder;
        has_negative_events |= !flooder.negative_weight_detection_events.empty();
        for (auto detector : flooder.negative_weight_detection_events) {
            auto& node = flooder.graph.node(detector);
            if (node.grid_component->predecode.active[node.grid_cell])
                --component.seed_end;
            else
                ++component.seed_end;
            OneHopPredecoder::toggle_seed(shot_seeds, node);
        }
    }
    if (has_negative_events) {
        assert(!fixed_seed_storage);
        OneHopPredecoder::compact_seeds(shot_seeds);
        logical_seed_count = shot_seeds.size();
    }
    auto seeds = std::span(shot_seeds).first(logical_seed_count);
    const size_t split = component_decoders.front().seed_end;
    if (component_decoders.size() == 2)
        (this->*seed_order_entry_)(seeds, split);
    for (size_t c = 0; c < component_decoders.size(); c++) {
        auto& component = component_decoders[c];
        component.seed_begin = c == 0 ? 0 : split;
        component.seed_end = c == 0 ? split : seeds.size();
        component.mwpm.predecoder.bind_seeds(
            std::span<PredecodeSeed>(shot_seeds)
                .subspan(component.seed_begin, component.seed_end - component.seed_begin),
            build_index);
    }
    seed_storage_stage = SeedStorageStage::published;
}

void ComponentDecoder::configure_seed_ordering(SeedOrdering ordering) {
    if (!shot_clean || prefix_ready)
        throw std::logic_error("Configure seed ordering while the decoder is idle.");
    switch (ordering) {
        case SeedOrdering::reconstruct:
            seed_order_entry_ = &ComponentDecoder::reconstruct_seed_order;
            break;
        case SeedOrdering::cycle_permutation:
            seed_order_entry_ = &ComponentDecoder::permute_seed_order;
            break;
        default:
            throw std::invalid_argument("Unknown seed ordering policy.");
    }
}

void ComponentDecoder::reconstruct_seed_order(std::span<PredecodeSeed> seeds, size_t split) {
    partition_positions.resize(seeds.size());
    std::array<uint32_t, 2> next{0, uint32_t(split)};
    for (const auto& seed : seeds) {
        const auto c = seed.grid->decoder_component;
        partition_positions[next[c]++] = uint32_t(component_decoders[c].global_detectors[seed.detector_id]);
    }
    for (size_t k = 0; k < seeds.size(); ++k) {
        const auto& address = detector_addresses[partition_positions[k]];
        seeds[k] = PredecodeSeed{address.local, address.cell};
        seeds[k].grid = component_decoders[address.component].seed_grid;
    }
}

void ComponentDecoder::permute_seed_order(std::span<PredecodeSeed> seeds, size_t split) {
    partition_positions.resize(seeds.size());
    std::array<uint32_t, 2> next{0, uint32_t(split)};
    for (size_t k = 0; k < seeds.size(); ++k)
        partition_positions[k] = next[seeds[k].grid->decoder_component]++;
    for (uint32_t k = 0; k < seeds.size(); ++k) {
        while (partition_positions[k] != k) {
            const auto destination = partition_positions[k];
            std::swap(seeds[k], seeds[destination]);
            std::swap(partition_positions[k], partition_positions[destination]);
        }
    }
}

void ComponentDecoder::prepare_shot(const std::vector<uint64_t>& events, bool build_index) {
    if (prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
    if (!shot_clean)
        clear_shot();
    shot_clean = false;
    try {
        if (!has_negative_detection_events) {
            scatter_shot(events, build_index);
            return;
        }
        acquire_seed_storage(SeedStorageStage::legacy_partial);
        for (auto event : events) {
            if (event >= detector_addresses.size())
                throw std::invalid_argument("Detection event index " + std::to_string(event) + " is not in the graph.");
            const auto& address = detector_addresses[event];
            if (address.component != NO_COMPONENT) {
                OneHopPredecoder::append_seed(
                    shot_seeds, component_decoders[address.component].mwpm.flooder.graph.node(address.local));
                ++component_decoders[address.component].seed_end;
                logical_seed_count = shot_seeds.size();
#if BB_STRUCTURED_PROFILE_COUNTERS
                ++component_decoders[address.component].mwpm.predecoder.stats.input_events;
#endif
            }
        }
        finish_shot_build(build_index);
    } catch (...) {
        clear_shot();
        throw;
    }
}

void ComponentDecoder::prepare_for_mwpm(const std::vector<uint64_t>& events) {
    if (prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
    if (!predecode_enabled_)
        throw std::logic_error("The split pre-MWPM lifecycle requires predecode enabled.");
    if (observable_count > sizeof(obs_int) * 8)
        throw std::invalid_argument("Too many observables for a bit-packed matching result.");
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
    ComponentDecodePhaseScope prefix(profile.prefix_ns BB_TIMELINE_ARG(&profile.prefix_interval));
#endif
    try {
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope partition(profile.partition_ns BB_TIMELINE_ARG(&profile.partition_interval));
#endif
            prepare_shot(events);
        }
        for (auto& component : component_decoders)
            prepare_timeline_for_mwpm(component.mwpm);
        prefix_ready = true;
    } catch (...) {
        reset();
        throw;
    }
}

MatchingResult ComponentDecoder::solve_prepared() {
    if (!prefix_ready)
        throw std::logic_error("solve_prepared requires prepare_for_mwpm.");
    MatchingResult result;
    try {
        for (size_t c = 0; c < component_decoders.size(); ++c) {
            auto& mwpm = component_decoders[c].mwpm;
#if BB_STRUCTURED_PROFILE_TIMERS
            const auto local = [&] {
                ComponentDecodePhaseScope component(
                    profile.component_decode_ns BB_TIMELINE_ARG(profile.component_interval(c)));
                return solve_prepared_detection_events_deferring_seed_nodes(mwpm);
            }();
            ComponentDecodePhaseScope merge(profile.merge_ns BB_TIMELINE_ARG(profile.merge_interval(c)));
            result += local;
#else
            result += solve_prepared_detection_events_deferring_seed_nodes(mwpm);
#endif
        }
        prefix_ready = false;
        return result;
    } catch (...) {
        reset();
        throw;
    }
}

void ComponentDecoder::reset_after_shot() {
    if (prefix_ready) {
        // Abandoning an unsolved prefix must also retire its imported regions.
        reset();
    } else {
        for (auto& component : component_decoders) {
            component.mwpm.clear_pending_seed_nodes();
            auto& pool = *component.mwpm.flooder.regions;
            if (const auto end = pool.take_neutral_reset_end())
                pool.prepare_neutral_range(1, end);
        }
        clear_shot();
        match_edges.clear();
    }
#if BB_PREDECODE_DIAGNOSTICS || BB_STRUCTURED_PROFILE_COUNTERS || BB_STRUCTURED_PROFILE_TIMERS
    // The explicit reset has its own outside-window wall clock. Its Reset
    // interval must not survive into the next shot, which skips clear_shot.
    for (auto& component : component_decoders)
        component.mwpm.predecoder.stats = {};
#endif
}

ComponentDecoder::PartitionedSyndrome ComponentDecoder::partition_detection_events(
    const std::vector<uint64_t>& events) const {
    PartitionedSyndrome result(component_decoders.size());
    for (auto event : events) {
        if (event >= detector_addresses.size())
            throw std::invalid_argument("Detection event index " + std::to_string(event) + " is not in the graph.");
        const auto address = detector_addresses[event];
        if (address.component != NO_COMPONENT)
            result[address.component].push_back(address.local);
    }
    return result;
}

MatchingResult ComponentDecoder::decode_partitioned(const PartitionedSyndrome& events) {
    if (prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
#endif
#if BB_STRUCTURED_PROFILE_TIMELINE
    ComponentDecodePhaseScope decode_phase(profile.decode_ns, &profile.decode_interval);
#endif
    if (observable_count > sizeof(obs_int) * 8)
        throw std::invalid_argument("Too many observables for a bit-packed matching result.");
    if (events.size() != component_decoders.size())
        throw std::invalid_argument("Partitioned syndrome has the wrong number of components.");
    MatchingResult result;
    try {
        clear_shot();
        shot_clean = false;
        acquire_seed_storage(SeedStorageStage::legacy_partial);
        const auto append = [&]<bool Fixed>() {
            for (size_t c = 0; c < component_decoders.size(); c++) {
                auto& graph = component_decoders[c].mwpm.flooder.graph;
                for (auto event : events[c]) {
                    if (event >= graph.num_nodes)
                        throw std::invalid_argument("Partitioned detection event is outside its component.");
                    if constexpr (Fixed) {
                        auto& node = graph.node(event);
                        auto& active = node.grid_component->predecode.active[node.grid_cell];
                        if (active) throw std::invalid_argument("Duplicate detection event in predecoder input.");
                        assert(logical_seed_count < shot_seeds.size());
                        PredecodeSeed seed{uint32_t(node.detector_id), uint32_t(node.grid_cell)};
                        seed.grid = node.grid_component;
                        shot_seeds[logical_seed_count++] = seed;
                        active = uint32_t(logical_seed_count);
                    } else {
                        OneHopPredecoder::append_seed(shot_seeds, graph.node(event));
                        logical_seed_count = shot_seeds.size();
                    }
                    ++component_decoders[c].seed_end;
                }
#if BB_STRUCTURED_PROFILE_COUNTERS
                component_decoders[c].mwpm.predecoder.stats.input_events = events[c].size();
#endif
            }
        };
        if (fixed_seed_storage) append.template operator()<true>();
        else append.template operator()<false>();
        finish_shot_build(true);
        for (size_t c = 0; c < component_decoders.size(); c++) {
#if BB_STRUCTURED_PROFILE_TIMERS
            auto local = [&] {
                ComponentDecodePhaseScope component_phase(profile.component_decode_ns BB_TIMELINE_ARG(profile.component_interval(c)));
                return decode_prepared_detection_events_for_up_to_64_observables(component_decoders[c].mwpm);
            }();
            {
                ComponentDecodePhaseScope merge_phase(profile.merge_ns BB_TIMELINE_ARG(profile.merge_interval(c)));
                result += local;
            }
#else
            result += decode_prepared_detection_events_for_up_to_64_observables(component_decoders[c].mwpm);
#endif
        }
    } catch (...) {
        reset();
        throw;
    }
    return result;
}

MatchingResult ComponentDecoder::decode(const std::vector<uint64_t>& events) {
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
#endif
#if BB_STRUCTURED_PROFILE_TIMELINE
    ComponentDecodePhaseScope decode_phase(profile.decode_ns, &profile.decode_interval);
#endif
    if (observable_count > sizeof(obs_int) * 8)
        throw std::invalid_argument("Too many observables for a bit-packed matching result.");
    {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope partition_phase(profile.partition_ns BB_TIMELINE_ARG(&profile.partition_interval));
#endif
        prepare_shot(events);
    }
    MatchingResult result;
    try {
        for (auto& component : component_decoders) {
#if BB_STRUCTURED_PROFILE_TIMERS
            auto local = [&] {
                ComponentDecodePhaseScope component_phase(profile.component_decode_ns BB_TIMELINE_ARG(
                    profile.component_interval(&component - component_decoders.data())));
                return decode_prepared_detection_events_for_up_to_64_observables(component.mwpm);
            }();
            {
                ComponentDecodePhaseScope merge_phase(profile.merge_ns BB_TIMELINE_ARG(
                    profile.merge_interval(&component - component_decoders.data())));
                result += local;
            }
#else
            result += decode_prepared_detection_events_for_up_to_64_observables(component.mwpm);
#endif
        }
    } catch (...) {
        reset();
        throw;
    }
    return result;
}

void ComponentDecoder::decode(const std::vector<uint64_t>& events, uint8_t* observables, total_weight_int& weight) {
    prepare_shot(events);
    try {
        for (auto& component : component_decoders) {
            total_weight_int local_weight = 0;
            decode_prepared_detection_events(component.mwpm, observables, local_weight);
            weight += local_weight;
        }
    } catch (...) {
        reset();
        throw;
    }
}

void ComponentDecoder::append_global_edges(
    const Component& component, const std::vector<int64_t>& local, std::vector<int64_t>& out) {
    for (auto node : local)
        out.push_back(node == -1 ? -1 : static_cast<int64_t>(component.global_detectors[node]));
}

void ComponentDecoder::decode_to_edges(const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
    if (!search_graph_included)
        throw std::invalid_argument("Component decoder does not contain search flooders, required to decode to edges.");
    prepare_shot(events);
    try {
        for (auto& component : component_decoders) {
            std::vector<int64_t> local_edges;
            decode_prepared_detection_events_to_edges(component.mwpm, local_edges);
            append_global_edges(component, local_edges, edges);
        }
    } catch (...) {
        reset();
        throw;
    }
}

void ComponentDecoder::decode_to_match_edges(const std::vector<uint64_t>& events) {
    prepare_shot(events);
    match_edges.clear();
    try {
        for (auto& component : component_decoders) {
            decode_prepared_detection_events_to_match_edges(component.mwpm);
            for (const auto& edge : component.mwpm.flooder.match_edges) {
                const auto& graph = component.mwpm.flooder.graph;
                match_edges.push_back(component.global_detectors[graph.node_id(edge.loc_from)]);
                match_edges.push_back(
                    edge.loc_to ? static_cast<int64_t>(component.global_detectors[graph.node_id(edge.loc_to)]) : -1);
            }
        }
    } catch (...) {
        reset();
        throw;
    }
}

void ComponentDecoder::get_nodes_on_shortest_path_from_source(size_t src, size_t dst, std::vector<size_t>& out) {
    if ((src != SIZE_MAX && src >= detector_addresses.size()) || (dst != SIZE_MAX && dst >= detector_addresses.size()))
        throw std::invalid_argument("Shortest-path node is not in the graph.");
    if (!search_graph_included)
        throw std::invalid_argument("Component decoder does not contain search flooders.");
    const bool src_boundary = src == SIZE_MAX || detector_addresses[src].component == NO_COMPONENT;
    const bool dst_boundary = dst == SIZE_MAX || detector_addresses[dst].component == NO_COMPONENT;
    if (src_boundary) {
        if (dst_boundary)
            throw std::invalid_argument("Both the source and destination vertices provided are boundary nodes");
        std::vector<size_t> reverse;
        get_nodes_on_shortest_path_from_source(dst, src, reverse);
        out.insert(out.end(), reverse.rbegin(), reverse.rend());
        return;
    }
    if (src == dst) {
        out.push_back(src);
        return;
    }
    const auto from = detector_addresses[src];
    if (!dst_boundary && detector_addresses[dst].component != from.component)
        throw std::invalid_argument("No path exists between different X/Z components.");
    auto& component = component_decoders[from.component];
    if (dst_boundary && !component.has_boundary)
        throw std::invalid_argument("No path exists from this component to a boundary.");
    auto& search = component.mwpm.search_flooder;
    search.iter_edges_on_shortest_path_from_source(
        from.local, dst_boundary ? SIZE_MAX : detector_addresses[dst].local, [&](const SearchGraphEdge edge) {
            out.push_back(component.global_detectors[search.graph.node_id(edge.detector_node)]);
        });
    if (!dst_boundary)
        out.push_back(dst);
}

MatchingResult decode_detection_events_for_up_to_64_observables(
    ComponentDecoder& decoder, const std::vector<uint64_t>& events, bool correlations) {
    ComponentDecoder::validate_correlations(correlations);
    return decoder.decode(events);
}
void decode_detection_events(
    ComponentDecoder& decoder,
    const std::vector<uint64_t>& events,
    uint8_t* observables,
    total_weight_int& weight,
    bool correlations) {
    ComponentDecoder::validate_correlations(correlations);
    decoder.decode(events, observables, weight);
}
void decode_detection_events_to_match_edges(ComponentDecoder& decoder, const std::vector<uint64_t>& events) {
    decoder.decode_to_match_edges(events);
}
void decode_detection_events_to_edges(
    ComponentDecoder& decoder, const std::vector<uint64_t>& events, std::vector<int64_t>& edges) {
    decoder.decode_to_edges(events, edges);
}
void decode_detection_events_to_edges_with_edge_correlations(
    ComponentDecoder&, const std::vector<uint64_t>&, std::vector<int64_t>&) {
    ComponentDecoder::validate_correlations(true);
}

}  // namespace beyond_blossom::structured
