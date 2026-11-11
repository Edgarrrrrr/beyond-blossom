// Untimed, first-use prepared-state evidence. No helper is called in a measured replay.
#pragma once

struct PreparedSeedRangeV2 {
    size_t bucket, begin, end, capacity_end, rank_begin;
};
template <typename Predecoder>
auto prepared_seed_storage(const Predecoder& pre) {
    if constexpr (requires { pre.seed_view.storage; }) return pre.seed_view.storage;
    else return pre.seeds;
}
template <typename Predecoder>
std::vector<PreparedSeedRangeV2> prepared_seed_ranges(const Predecoder& pre) {
    const auto storage = prepared_seed_storage(pre);
    if (storage.size() >= pm::structured::PredecodeSeed::BOUNDARY)
        throw std::runtime_error("Prepared seed capacity exceeds the slot/sentinel limit");
    std::vector<PreparedSeedRangeV2> result;
    if constexpr (requires { pre.seed_view.active_ranges(); }) {
        const auto ranges = pre.seed_view.active_ranges();
        if (ranges.size() > 1) {
            for (size_t b = 0; b < ranges.size(); ++b)
                result.push_back({b, ranges[b].begin, ranges[b].end,
                    b + 1 < ranges.size() ? ranges[b + 1].begin : storage.size(), ranges[b].rank_begin});
        }
    }
    if (result.empty()) {
        if constexpr (requires { pre.scratch_layout(); }) {
            size_t next = 0;
            for (const auto& r : pre.scratch_layout()) {
                if (r.seed_begin != next || r.seed_end < r.seed_begin || r.seed_end > storage.size()) {
                    result.clear(); break;
                }
                result.push_back({result.size(), r.seed_begin, r.seed_end, r.seed_end, r.seed_begin});
                next = r.seed_end;
            }
            if (next != storage.size()) result.clear();
        }
        if (result.empty()) result.push_back({0, 0, storage.size(), storage.size(), 0});
    }
    if (result.size() > 5) throw std::runtime_error("Prepared seed range count exceeds the supported bucket limit");
    size_t next_slot = 0, next_rank = 0;
    for (const auto& r : result) {
        if (r.begin != next_slot || r.end < r.begin || r.capacity_end < r.end || r.capacity_end > storage.size() ||
            r.rank_begin != next_rank) throw std::runtime_error("Invalid prepared seed validity/capacity ranges");
        next_slot = r.capacity_end; next_rank += r.end - r.begin;
    }
    if (next_slot != storage.size()) throw std::runtime_error("Prepared seed capacity is not covered");
    if constexpr (requires { pre.seed_view.active_count(); }) {
        if (next_rank != pre.seed_view.active_count()) throw std::runtime_error("Prepared seed active count differs");
    }
    return result;
}
inline uint32_t prepared_seed_rank(const std::vector<PreparedSeedRangeV2>& ranges, uint32_t slot) {
    for (const auto& r : ranges) if (slot >= r.begin && slot < r.end)
        return uint32_t(r.rank_begin + slot - r.begin);
    throw std::runtime_error("Prepared seed reference points to an inactive hole");
}
template <typename Predecoder>
const char* prepared_seed_layout(const Predecoder& pre) {
    if constexpr (requires { pre.seed_view.active_ranges(); })
        if (pre.seed_view.active_ranges().size() > 1) return "fixed";
    return "dense";
}

// Scratch capacity and actual source-order prefixes are separate evidence.
template <typename Predecoder>
void persist_scratch_v2(const Options& o, uint64_t shot, size_t component, const Predecoder& pre,
                        const std::vector<PreparedSeedRangeV2>& ranges) {
    if constexpr (requires { pre.scratch_layout(); }) {
        const auto mode = component == 0 ? std::ios::out : std::ios::app;
        std::ofstream raw(o.output / "scratch_layout_v2.csv", mode), active(o.output / "scratch_layout.csv", mode),
                      order(o.output / "prepared_seed_order_v2.csv", mode);
        if (component == 0) {
            raw << "shot_index,component_id,bucket_index,layout_kind,capacity_begin,capacity_end,capacity_count,valid_begin,valid_end,valid_count,mask_address,degree_address,order_address,mask_capacity_bytes,degree_capacity_bytes,order_capacity_bytes,mask_valid_bytes,degree_valid_bytes,order_valid_bytes\n";
            active << "shot_index,component_id,bucket_index,seed_begin,seed_end,seed_count,mask_address,degree_address,order_address,mask_bytes,degree_bytes,order_bytes,mask_mod64,degree_mod64,order_mod64\n";
            order << "shot_index,component_id,bucket_index,order_index,source_slot,source_rank\n";
        }
        const auto layouts = pre.scratch_layout();
        if (!layouts.empty() && layouts.size() != ranges.size()) throw std::runtime_error("Scratch/validity bucket count differs");
        for (size_t b = 0; b < layouts.size(); ++b) {
            const auto& s = layouts[b]; const auto& r = ranges[b];
            if (s.seed_begin != r.begin || s.seed_end != r.capacity_end)
                throw std::runtime_error("Scratch capacity differs from seed storage range");
            const auto capacity = s.seed_end - s.seed_begin, valid = r.end - r.begin;
            raw << shot << ',' << component << ',' << b << ',' << prepared_seed_layout(pre) << ','
                << s.seed_begin << ',' << s.seed_end << ',' << capacity << ',' << r.begin << ',' << r.end << ',' << valid
                << ',' << s.mask_address << ',' << s.degree_address << ',' << s.order_address << ','
                << capacity * 2 << ',' << capacity << ',' << capacity * 4 << ',' << valid * 2 << ',' << valid << ',' << valid * 4 << '\n';
            active << shot << ',' << component << ',' << b << ',' << r.begin << ',' << r.end << ',' << valid << ','
                   << s.mask_address << ',' << s.degree_address << ',' << s.order_address << ','
                   << valid * 2 << ',' << valid << ',' << valid * 4 << ',' << s.mask_address % 64 << ','
                   << s.degree_address % 64 << ',' << s.order_address % 64 << '\n';
            const auto* sources = reinterpret_cast<const uint32_t*>(s.order_address);
            for (size_t k = 0; k < valid; ++k) {
                if (!sources || sources[k] < r.begin || sources[k] >= r.end)
                    throw std::runtime_error("Scratch source order references another bucket or a hole");
                order << shot << ',' << component << ',' << b << ',' << k << ',' << sources[k] << ','
                      << prepared_seed_rank(ranges, sources[k]) << '\n';
            }
        }
        if (!raw || !active || !order) throw std::runtime_error("Scratch v2 persistence failed");
    }
}

template <typename Metadata, typename Decoder>
void persist_prepared_snapshot_v2(const Options& o, uint64_t shot, const Metadata& metadata, const Decoder& decoder) {
    using Seed = pm::structured::PredecodeSeed;
    std::ofstream state(o.output / "prepared_state.csv"), legacy(o.output / "prepared_seeds.csv"),
                  ranges_file(o.output / "prepared_seed_ranges.csv"), seeds(o.output / "prepared_seeds_v2.csv"),
                  semantic(o.output / "prepared_semantics_v2.json");
    state << "shot_index,component_id,seeds,residual_seeds,matched_pairs,matched_boundaries,complete,imported_nodes,queue_size,state_hash,route,active_threads\n";
    legacy << "shot_index,component_id,seed_index,global_detector,local_detector,cell,mate,radius,match_slot,imported_region_id,top_region_id\n";
    ranges_file << "shot_index,component_id,layout_kind,bucket_index,storage_capacity,static_begin,capacity_end,valid_begin,valid_end,rank_begin,rank_end\n";
    seeds << "shot_index,component_id,seed_rank,seed_slot,bucket_index,global_detector,local_detector,cell,mate_raw,mate_rank,radius,match_slot,active_raw,active_rank,grid_radius,cap_cache_present,cap_saturated,imported_region_id,top_region_id\n";
    semantic << "{\"schema_version\":2,\"shot_index\":" << shot << ",\"components\":[";
    for (size_t c = 0; c < metadata.components().size(); ++c) {
        const auto& part = metadata.components()[c]; const auto& mwpm = part.mwpm; const auto& pre = mwpm.predecoder;
        const auto storage = prepared_seed_storage(pre); const auto ranges = prepared_seed_ranges(pre);
        persist_scratch_v2(o, shot, c, pre, ranges);
        const auto& pool = *mwpm.flooder.regions;
        size_t residual = 0, paired = 0, boundaries = 0, imported = 0, count = 0;
        uint64_t digest = 0;
        std::vector<pm::structured::RegionId> region_ids;
        const auto node_id = [&](const pm::structured::DetectorNode* n) -> uint64_t {
            if (!n) return UINT64_MAX;
            if (n->detector_id >= mwpm.flooder.graph.num_nodes || &mwpm.flooder.graph.node(n->detector_id) != n)
                throw std::runtime_error("Prepared pointer is not a node in this component");
            return n->detector_id;
        };
        for (const auto& r : ranges) {
            ranges_file << shot << ',' << c << ',' << prepared_seed_layout(pre) << ',' << r.bucket << ',' << storage.size()
                        << ',' << r.begin << ',' << r.capacity_end << ',' << r.begin << ',' << r.end << ',' << r.rank_begin
                        << ',' << r.rank_begin + r.end - r.begin << '\n';
            for (size_t slot = r.begin; slot < r.end; ++slot) {
                const auto& s = storage[slot]; const auto rank = r.rank_begin + slot - r.begin;
                if (s.detector_id >= part.global_detectors.size() || !s.grid)
                    throw std::runtime_error("Prepared seed detector/grid differs");
                const auto& n = s.grid->node_at_cell(s.cell);
                if (node_id(&n) != s.detector_id) throw std::runtime_error("Prepared seed node differs");
                const auto mate = s.mate < Seed::BOUNDARY ? prepared_seed_rank(ranges, s.mate) : s.mate;
                if (s.mate < Seed::BOUNDARY && (s.mate == slot || storage[s.mate].mate != slot))
                    throw std::runtime_error("Prepared ordinary mate identity is not reciprocal");
                const auto active = s.grid->predecode.active[s.cell];
                const auto active_rank = active ? prepared_seed_rank(ranges, active - 1) + 1 : 0;
                if (active_rank != rank + 1) throw std::runtime_error("Prepared seed active identity differs");
                residual += s.mate == Seed::UNMATCHED; boundaries += s.mate == Seed::BOUNDARY;
                paired += s.mate < Seed::BOUNDARY; ++count;
                const auto id = n.region_that_arrived;
                imported += id != pm::structured::NO_REGION;
                if (id) {
                    if (id > pool.capacity() || !pool.is_live(id, pool.generation(id)) ||
                        std::find(region_ids.begin(), region_ids.end(), id) != region_ids.end())
                        throw std::runtime_error("Imported Region identity is not uniquely live");
                    region_ids.push_back(id);
                }
                legacy << shot << ',' << c << ',' << slot << ',' << part.global_detectors.at(s.detector_id) << ','
                       << s.detector_id << ',' << s.cell << ',' << s.mate << ',' << s.radius << ',' << unsigned(s.match_slot)
                       << ',' << id << ',' << n.top_region_id() << '\n';
                seeds << shot << ',' << c << ',' << rank << ',' << slot << ',' << r.bucket << ','
                      << part.global_detectors.at(s.detector_id) << ',' << s.detector_id << ',' << s.cell << ','
                      << s.mate << ',' << mate << ',' << s.radius << ',' << unsigned(s.match_slot) << ',' << active << ','
                      << active_rank << ',' << s.grid->predecode.radii[s.cell] << ',';
#if PM_PREDECODE_CAP_CACHE
                seeds << "1," << s.cap_saturated;
#else
                seeds << "0,0";
#endif
                seeds << ',' << id << ',' << n.top_region_id() << '\n';
                for (const uint64_t value : {uint64_t(s.detector_id), uint64_t(s.cell), uint64_t(s.mate),
                                             uint64_t(s.radius), uint64_t(s.match_slot)}) digest = mix(digest, value);
            }
        }
        if (paired % 2 || residual + paired + boundaries != count || (pre.complete && imported) || (!pre.complete && imported != count))
            throw std::runtime_error("Prepared-state seed/import conservation failed");
        int route = -1; size_t active = 1;
        if constexpr (requires { decoder.component_route(c); decoder.active_predecode_threads(c); }) {
            route = static_cast<int>(decoder.component_route(c)); active = decoder.active_predecode_threads(c);
        }
        state << shot << ',' << c << ',' << count << ',' << residual << ',' << paired / 2 << ',' << boundaries << ','
              << pre.complete << ',' << imported << ',' << mwpm.flooder.queue.size() << ',' << digest << ',' << route << ',' << active << '\n';
        if (c) semantic << ',';
        semantic << "{\"component_id\":" << c << ",\"predecode\":{\"complete\":" << pre.complete
                 << ",\"active_count\":" << count << ",\"result_pending\":" << pre.result_pending
                 << ",\"result_obs\":" << pre.result.obs_mask << ",\"result_weight\":" << pre.result.weight
                 << ",\"cell_state_live\":" << pre.has_live_cell_state()
                 << ",\"pending_seed_cleanup\":" << mwpm.has_pending_seed_cleanup()
                 << ",\"pool_empty\":" << pool.empty() << ",\"pending_neutral_reset\":" << pool.has_pending_neutral_reset()
                 << "},\"nodes\":[";
        for (size_t k = 0; k < mwpm.flooder.graph.num_nodes; ++k) {
            const auto& n = mwpm.flooder.graph.node(k); const auto& collision = n.grid_component->collision_states[n.grid_cell];
            if (k) semantic << ',';
            semantic << '[' << n.detector_id << ',' << part.global_detectors.at(k) << ',' << n.grid_cell << ','
                     << n.region_that_arrived << ',' << node_id(n.reached_from_source) << ','
                     << n.observables_crossed_from_source << ',' << uint64_t(n.radius_of_arrival) << ','
                     << collision.top_region_id << ',' << uint64_t(collision.wrapped_radius_cached) << ','
                     << n.node_event_tracker.desired_time.value << ',' << n.node_event_tracker.queued_time.value << ','
                     << n.node_event_tracker.has_desired_time << ',' << n.node_event_tracker.has_queued_time << ']';
        }
        const auto edge = [&](const auto& e) { semantic << '[' << node_id(e.loc_from) << ',' << node_id(e.loc_to) << ',' << e.obs_mask << ']'; };
        const auto tree_id = [&](const auto* root) -> uint64_t {
            if (!root) return 0;
            if (root->region_pool != &pool) throw std::runtime_error("AltTree pointer refers to another RegionPool");
            return root->outer_region;
        };
        semantic << "],\"regions\":[";
        for (size_t k = 0; k < region_ids.size(); ++k) {
            const auto id = region_ids[k]; const auto& r = pool[id];
            if (r.pool != &pool || r.region_id != id) throw std::runtime_error("Region pool ownership differs");
            if (k) semantic << ',';
            semantic << "{\"id\":" << id << ",\"generation\":" << pool.generation(id) << ",\"parent\":" << r.blossom_parent
                     << ",\"top\":" << r.blossom_parent_top << ",\"radius_bits\":" << uint64_t(r.radius_value().data)
                     << ",\"tree_id\":" << tree_id(r.alt_tree_node) << ",\"match_region\":" << r.match.region << ",\"match_edge\":";
            edge(r.match.edge);
            semantic << ",\"shrink_tracker\":[" << r.shrink_event_tracker.desired_time.value << ',' << r.shrink_event_tracker.queued_time.value
                     << ',' << r.shrink_event_tracker.has_desired_time << ',' << r.shrink_event_tracker.has_queued_time << "],\"shell\":[";
            for (size_t n = 0; n < r.shell_area.size(); ++n) { if (n) semantic << ','; semantic << node_id(r.shell_area[n]); }
            semantic << "],\"children\":[";
            for (size_t n = 0; n < r.blossom_children.size(); ++n) { if (n) semantic << ','; semantic << '[' << r.blossom_children[n].region << ','; edge(r.blossom_children[n].edge); semantic << ']'; }
            semantic << "]}";
        }
        semantic << "],\"roots\":[";
        size_t roots = 0;
        for (const auto* root : mwpm.node_arena.allocated) {
            if (std::find(mwpm.node_arena.available.begin(), mwpm.node_arena.available.end(), root) != mwpm.node_arena.available.end()) continue;
            if (roots++) semantic << ',';
            semantic << "{\"tree_id\":" << tree_id(root) << ",\"inner\":" << root->inner_region << ",\"outer\":" << root->outer_region
                     << ",\"visited\":" << root->visited << ",\"inner_outer_edge\":"; edge(root->inner_to_outer_edge);
            semantic << ",\"parent\":[" << tree_id(root->parent.alt_tree_node) << ','; edge(root->parent.edge);
            semantic << "],\"children\":[";
            for (size_t k = 0; k < root->children.size(); ++k) { if (k) semantic << ','; semantic << '[' << tree_id(root->children[k].alt_tree_node) << ','; edge(root->children[k].edge); semantic << ']'; }
            semantic << "]}";
        }
        semantic << "],\"queue\":{\"cur_time\":" << uint64_t(mwpm.flooder.queue.cur_time) << ",\"size\":" << mwpm.flooder.queue.size() << ",\"bit_buckets\":[";
        size_t queued = 0;
        for (size_t b = 0; b < mwpm.flooder.queue.bit_buckets.size(); ++b) {
            if (b) semantic << ','; semantic << '[';
            const auto& bucket = mwpm.flooder.queue.bit_buckets[b];
            for (size_t k = 0; k < bucket.size(); ++k) {
                const auto& e = bucket[k]; uint64_t target = 0, generation = 0;
                if (e.tentative_event_type == pm::structured::LOOK_AT_NODE) target = node_id(e.data_look_at_node);
                else if (e.tentative_event_type == pm::structured::LOOK_AT_SHRINKING_REGION) {
                    target = e.data_look_at_shrinking_region.id; generation = e.data_look_at_shrinking_region.generation;
                } else if (e.tentative_event_type != pm::structured::NO_FLOOD_CHECK_EVENT)
                    throw std::runtime_error("Unexpected search event in prepared MWPM queue");
                if (k) semantic << ',';
                semantic << '[' << unsigned(e.tentative_event_type) << ',' << e.time.value << ',' << target << ',' << generation << ']'; ++queued;
            }
            semantic << ']';
        }
        if (queued != mwpm.flooder.queue.size()) throw std::runtime_error("Prepared queue size differs from native buckets");
        semantic << "]}}";
    }
    semantic << "]}\n";
    if (!state || !legacy || !ranges_file || !seeds || !semantic) throw std::runtime_error("Prepared v2 persistence failed");
    std::ofstream schema(o.output / "prepared_state_schema.json");
    schema << R"({"schema_version":2,"types":{"seed_slot":"uint32","seed_rank":"uint32","mate":"uint32 sentinel or seed reference","active":"uint32 zero or seed slot+1","range_bounds":"size_t, native64","seed_radius":"weight_int signed integer","match_slot":"uint8","RegionID":"uint32","RegionGeneration":"uint32 raw","semantic_integer":"uint64 raw scalar representation unless named signed result weight","flags":"integer0or1"},"scope":"one cold prepared check before warmup; all real graph nodes; ordered live imported regions and active arena roots; original queue bit buckets and entry order","integer_representation":"unsigned raw bit patterns for signed radius/collision/queue storage; matching result weight and seed radius keep their signed integer value","sentinels":{"UNMATCHED":4294967295,"BOUNDARY":4294967294,"NULL_NODE":18446744073709551615,"NO_REGION":0},"legacy_projection":"prepared_seeds.csv seed_index and mate remain raw physical slots; prepared_state.csv state_hash mixes raw local_detector,cell,mate,radius,match_slot in valid range order","normalization_allowed":["ordinary seed mate slot to active rank","nonzero grid active slot+1 to active rank+1","scratch order source slot to active rank"],"raw_semantics":"Region IDs, generations, Node fields, root identities, edges and queue fields/order are not normalized","node_columns":["local_detector","global_detector","cell","arrived_region","source_local_detector","source_observable","arrival_radius_bits","top_region","wrapped_radius_bits","desired_time","queued_time","has_desired_time","has_queued_time"],"queue_entry_columns":["type","time","stable_target_id","region_generation"],"root_order":"active node_arena.allocated order at the first-use prepared check; available/unconstructed arena slots are never read","scratch":"scratch_layout_v2 capacity is immutable storage; scratch_layout.csv reports only current valid prefixes; source-order values are read only within valid prefixes","seed_count_phase":"FixedSpan Group: input validation only; legacy: input count; physical construction and capacity allocation are constructor work","constructor_storage":"constructed capacity is recorded separately from active seed count","range_publish_transition":"FixedSpan join-side publication remains in prefix; derive SharedJoin.end to ComponentTail.begin only when both successful-shot windows exist; no new internal clock"})" << '\n';
    if (!schema) throw std::runtime_error("Prepared v2 schema persistence failed");
}
