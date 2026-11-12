#ifndef STENCIL_GENERIC_ONLY
beyond_blossom::GridSimd selected_backend(const std::string& backend) {
    if (backend == "scalar") return beyond_blossom::GridSimd::scalar;
    if (backend == "avx512") return beyond_blossom::GridSimd::avx512;
    if (backend == "avx2") return beyond_blossom::GridSimd::avx2;
    if (backend == "avx512vl") return beyond_blossom::GridSimd::avx512vl;
    throw std::invalid_argument("Unavailable structured backend " + backend);
}
template <typename Decoder>
void persist_kernel_selection(const Options& o, Decoder& decoder) {
    std::ofstream out(o.output / "kernel_selection.csv");
    out << "component_id,grid_id,requested_backend,requested_simd,actual_selected_simd,matching_nodes,grid_cells\n";
    const auto requested = selected_backend(o.backend);
    auto& metadata = metadata_decoder(decoder);
    for (size_t c = 0; c < metadata.components().size(); ++c) {
        const auto& storage = metadata.components()[c].mwpm.flooder.graph.grid;
        if (!storage || storage->components.empty()) throw std::runtime_error("Missing structured matching grid");
        for (size_t g = 0; g < storage->components.size(); ++g) {
            const auto& grid = *storage->components[g];
            if (grid.selected_simd != requested) throw std::runtime_error("Actual matching SIMD differs from frozen request");
            out << c << ',' << g << ',' << o.backend << ',' << static_cast<int>(requested) << ','
                << static_cast<int>(grid.selected_simd) << ',' << grid.nodes.size() << ',' << grid.kinds.size() << '\n';
        }
    }
    if (!out) throw std::runtime_error("Kernel selection record write failed");
}
#if BB_EXPERIMENT_SHARED
// Bind the existing eb66e671 strategy configuration once, outside timing.
template <typename Shared>
Shared make_shared_decoder(const Options& o, beyond_blossom::structured::ComponentDecoder&& serial) {
    const bool spatial = o.prefix_schedule == "component_group_owned";
    beyond_blossom::structured::SharedDecoderConfiguration config;
    if (spatial) {
        config.fixed_seed_construction = beyond_blossom::structured::FixedSeedConstruction::fused;
        config.fixed_seed_input = beyond_blossom::structured::FixedSeedInput::indexed;
        config.import_preparation = beyond_blossom::structured::ImportPreparation::captained_serial;
        config.import_reset_owner = beyond_blossom::structured::ImportResetOwner::captain;
        config.seed_ordering = beyond_blossom::structured::SeedOrdering::cycle_permutation;
    }
    return Shared(std::move(serial), o.cpus, o.partitions, o.component_threshold,
        o.shot_threshold, o.conflict_width, o.parallel_import,
        spatial ? Shared::PrefixSchedule::component_group_owned : Shared::PrefixSchedule::component_sequential,
        Shared::ContinuationPlacement::component_captain,
        spatial ? beyond_blossom::structured::TimePartitionCuts::Geometry::three_planes
                : beyond_blossom::structured::TimePartitionCuts::Geometry::symmetric_width, config);
}
#endif
template <typename Callback>
void with_structured_decoder(const Options& o, const stim::DetectorErrorModel& dem, bool search, Callback&& callback) {
    auto graph = beyond_blossom::detector_error_model_to_backend_user_graph(dem, false, beyond_blossom::NUM_DISTINCT_WEIGHTS);
    const auto spec = frozen_grid_spec_from_dem(dem, o.distance, o.distance, true, selected_backend(o.backend));
    beyond_blossom::structured::ComponentDecoder serial(graph, spec, beyond_blossom::NUM_DISTINCT_WEIGHTS, search, o.predecode);
    if (serial.components().size() != 2) throw std::runtime_error("Expected independent X/Z components");
    // The core's OneHopPredecoder::run binds the compiled AE predecode
    // policy for serial decoding too. Keep normal timing on the unchanged decode
    // lifecycle; only the separate statistics mode uses the prepared-state API.
    if (o.execution == "serial") { serial.initialize_serial_seed_storage(); callback(serial); return; }
    if (o.execution == "pipeline") {
#if BB_EXPERIMENT_PIPELINE
        Options even = o, odd = o;
        const bool single = o.pipeline_schedule == "single_thread";
        const size_t group = single ? 1 : o.cpus.size() / 2;
        even.cpus.assign(o.cpus.begin(), o.cpus.begin() + group);
        odd.cpus = single ? even.cpus : std::vector<int>(o.cpus.begin() + group, o.cpus.end());
        even.main_cpu = even.cpus.front(); odd.main_cpu = odd.cpus.front();
        beyond_blossom::structured::ComponentDecoder other(graph, spec, beyond_blossom::NUM_DISTINCT_WEIGHTS, search, o.predecode);
        auto first = make_shared_decoder<beyond_blossom::structured::SharedComponentDecoder>(even, std::move(serial));
        auto second = make_shared_decoder<beyond_blossom::structured::SharedComponentDecoder>(odd, std::move(other));
        using Pipe = beyond_blossom::structured::DoubleBufferComponentDecoder;
        const auto schedule = single ? Pipe::Schedule::single_thread
            : o.pipeline_schedule == "serial_control" ? Pipe::Schedule::serial_control : Pipe::Schedule::overlap;
        Pipe decoder(std::move(first), std::move(second), schedule);
        callback(decoder); return;
#else
        throw std::runtime_error("Frozen source has no double-buffer pipeline");
#endif
    }
    if (o.execution == "shared") {
#if BB_EXPERIMENT_SHARED
        auto decoder = make_shared_decoder<beyond_blossom::structured::SharedComponentDecoder>(o, std::move(serial));
        callback(decoder); return;
#else
        throw std::runtime_error("Frozen source has no shared component decoder");
#endif
    }
    if (o.execution == "openmp2") {
#if BB_EXPERIMENT_OPENMP
        beyond_blossom::structured::OpenMpComponentDecoder decoder(std::move(serial), o.worker_cpu, o.main_cpu);
        callback(decoder); return;
#else
        throw std::runtime_error("Frozen source has no OpenMP component decoder");
#endif
    }
    throw std::runtime_error("Unsupported execution policy");
}

// Recover the parity-constrained shortest path of one actual compressed match.
// Benchmark circuits have one observable. Boundary is a terminal, so detector
// pairs cannot take an artificial shortcut through the virtual boundary.
std::vector<std::pair<int64_t,int64_t>> constrained_path(
    const beyond_blossom::MatchingGraph& graph, int64_t from, int64_t to, beyond_blossom::obs_int obs) {
    if (graph.num_observables > 1 || obs > 1) throw std::runtime_error("Witness currently supports at most one observable");
    if (from==-1) std::swap(from,to);
    const size_t n=graph.nodes.size();
    if (from<0 || size_t(from)>=n || to < -1 || (to!=-1 && size_t(to)>=n)) throw std::runtime_error("Invalid compressed endpoint");
    const auto target=(to==-1?n:size_t(to))*2+size_t(obs);
    const auto start=size_t(from)*2;
    constexpr int64_t INF=INT64_MAX/4;
    std::vector<int64_t> dist((n+1)*2,INF);
    std::vector<size_t> prev(dist.size(),SIZE_MAX);
    using Item=std::pair<int64_t,size_t>;
    std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
    dist[start]=0;queue.push({0,start});
    while (!queue.empty()) {
        const auto [cost,state]=queue.top();queue.pop();
        if (cost!=dist[state]) continue;
        if (state==target) break;
        const auto node=state/2;const auto parity=state%2;
        if (node==n) continue;
        const auto& src=graph.nodes[node];
        for (size_t slot=0;slot<src.neighbors.size();++slot) {
            const auto* neighbor=src.neighbors[slot];
            if (!neighbor && to!=-1) continue;
            const size_t next_node=neighbor?size_t(neighbor-graph.nodes.data()):n;
            if (src.neighbor_observables[slot]>1 || src.neighbor_weights[slot]<0) throw std::runtime_error("Unsupported witness graph edge");
            const auto next=2*next_node+(parity^size_t(src.neighbor_observables[slot]));
            const int64_t next_cost=cost+src.neighbor_weights[slot];
            if (next_cost<dist[next]) { dist[next]=next_cost;prev[next]=state;queue.push({next_cost,next}); }
        }
    }
    if (dist[target]==INF) throw std::runtime_error("Actual compressed observable has no original-graph path");
    std::vector<std::pair<int64_t,int64_t>> edges;
    for (auto cur=target;cur!=start;cur=prev[cur]) {
        if (prev[cur]==SIZE_MAX) throw std::runtime_error("Broken witness path");
        const auto a=prev[cur]/2,b=cur/2;
        edges.push_back(std::minmax(a==n?int64_t{-1}:int64_t(a),b==n?int64_t{-1}:int64_t(b)));
    }
    return edges;
}

// The SAME measured candidate supplies its matching and observable mask. The
// original graph independently recovers and checks a consistent correction.
void verify_candidate(const Options& o, const Dataset& data, const stim::DetectorErrorModel& dem,
                      const std::vector<beyond_blossom::MatchingResult>& values) {
    std::ofstream differences(o.output / "differences.csv");
    differences << "shot_index,expected_observable,actual_observable,expected_weight,actual_weight\n";
    std::vector<size_t> selected;
    size_t weights = 0, observables = 0;
    for (size_t k = 0; k < values.size(); ++k) {
        const bool wd = values[k].weight != data.expected[k].weight;
        const bool od = values[k].obs_mask != data.expected[k].obs_mask;
        weights += wd; observables += od;
        if (wd || od)
            differences << data.shot_ids[k] << ',' << data.expected[k].obs_mask << ',' << values[k].obs_mask << ','
                        << data.expected[k].weight << ',' << values[k].weight << '\n';
        if (od || o.witness_all) selected.push_back(k);
    }
    differences.close();
    if (weights || (o.verify == "exact" && observables))
        throw std::runtime_error("Frozen result verification policy failed; raw records retained");
    size_t parent_differences = 0;
    if (!data.parent.empty()) {
        if (data.parent.size() != values.size()) throw std::runtime_error("Parent reference result count differs");
        for (size_t k = 0; k < values.size(); ++k) parent_differences += data.parent[k] != values[k];
        if (parent_differences) throw std::runtime_error("Exact parent reference comparison failed");
    }
    size_t verified = 0;
    if (!selected.empty()) {
        auto original = beyond_blossom::detector_error_model_to_mwpm(dem, beyond_blossom::NUM_DISTINCT_WEIGHTS);
        auto& graph = original.flooder.graph;
        if (graph.negative_weight_sum != 0)
            throw std::runtime_error("Benchmark witness checker expects nonnegative circuit edge weights");
        std::ofstream edges_out(o.output / "witness_edges.csv"), checks(o.output / "witness_checks.csv"), compressed(o.output / "candidate_compressed_matches.csv");
        edges_out << "shot_index,edge_index,u,v,integer_weight,observable_mask\n";
        checks << "shot_index,edge_count,integer_weight,observable_mask,parity_valid,actual_candidate\n";
        compressed << "shot_index,match_index,u,v,observable_mask\n";
        // Search graph construction and every replay happen after all timing.
        Options witness_options = o;
#if BB_AE_TAILRESET
        // Replay the identical per-buffer P1 component algorithm with matching
        // export. The pipeline wrapper has no export API. Observables, weight
        // and parity must reproduce the measured result before acceptance.
        if (o.execution == "pipeline") {
            witness_options.execution = "shared";
            witness_options.cpus.resize(o.cpus.size() / 2);
        }
#endif
        with_structured_decoder(witness_options, dem, true, [&](auto& candidate) {
            auto body = [&] {
                for (const auto k : selected) {
                    std::vector<int64_t> edges;
                    if constexpr (requires { candidate.decode_to_matches(data.hits[k]); }) {
                        const auto matches=candidate.decode_to_matches(data.hits[k]);
                        std::set<std::pair<int64_t,int64_t>> parity_edges;
                        beyond_blossom::obs_int match_observable=0;
                        size_t match_index=0;
                        for (const auto& match : matches) {
                            compressed << data.shot_ids[k] << ',' << match_index++ << ',' << match.u << ',' << match.v << ',' << match.obs_mask << '\n';
                            match_observable ^= match.obs_mask;
                            for (const auto& edge : constrained_path(graph,match.u,match.v,match.obs_mask)) {
                                auto [it,inserted]=parity_edges.insert(edge);
                                if (!inserted) parity_edges.erase(it);
                            }
                        }
                        if (match_observable!=values[k].obs_mask) throw std::runtime_error("Candidate compressed witness differs from timed observable");
                        for (const auto& edge : parity_edges) { edges.push_back(edge.first);edges.push_back(edge.second); }
                    } else if constexpr (requires { candidate.decode_to_edges(data.hits[k], edges); }) {
                        candidate.decode_to_edges(data.hits[k], edges);
                    } else {
                        throw std::runtime_error("This execution API has no matching witness export");
                    }
                    if (edges.size() % 2) throw std::runtime_error("Odd witness edge array");
                    std::vector<bool> expected(graph.nodes.size()), parity(graph.nodes.size());
                    for (auto hit : data.hits[k]) expected.at(hit) = !expected.at(hit);
                    std::set<std::pair<int64_t, int64_t>> unique;
                    beyond_blossom::total_weight_int weight = 0;
                    beyond_blossom::obs_int observable = 0;
                    for (size_t j = 0; j < edges.size(); j += 2) {
                        auto u = edges[j], v = edges[j + 1];
                        if (u == -1) std::swap(u, v);
                        if (u < 0 || size_t(u) >= graph.nodes.size() || v < -1 || (v != -1 && size_t(v) >= graph.nodes.size()))
                            throw std::runtime_error("Invalid global witness detector");
                        if (!unique.insert(std::minmax(u, v)).second) throw std::runtime_error("Repeated correction edge");
                        const auto& node = graph.nodes[u];
                        const auto index = node.index_of_neighbor(v == -1 ? nullptr : &graph.nodes[v]);
                        if (index >= node.neighbors.size()) throw std::runtime_error("Witness edge absent from original graph");
                        weight += node.neighbor_weights[index]; observable ^= node.neighbor_observables[index];
                        parity[u] = !parity[u]; if (v != -1) parity[v] = !parity[v];
                        edges_out << data.shot_ids[k] << ',' << j / 2 << ',' << u << ',' << v << ',' << node.neighbor_weights[index]
                                  << ',' << node.neighbor_observables[index] << '\n';
                    }
                    if (parity != expected || weight != data.expected[k].weight || weight != values[k].weight || observable != values[k].obs_mask)
                        throw std::runtime_error("Actual candidate witness failed at shot " + std::to_string(k));
                    checks << data.shot_ids[k] << ',' << edges.size() / 2 << ',' << weight << ',' << observable << ",1,1\n";
                    ++verified;
                }
            };
            if constexpr (requires { candidate.run_with_team(body); }) candidate.run_with_team(body);
            else body();
        });
        if (!edges_out || !checks || !compressed) throw std::runtime_error("Witness persistence failed");
    }
    std::ofstream out(o.output / "correctness.json");
    out << "{\"status\":\"passed\",\"shots\":" << values.size() << ",\"weight_differences\":" << weights
        << ",\"observable_differences\":" << observables << ",\"parent_differences\":" << parent_differences
        << ",\"parent_reference_checked\":" << (!data.parent.empty() ? "true" : "false") << ",\"actual_candidate_witnesses\":" << verified
        << ",\"policy\":\"" << (o.verify == "exact" ? "exact upstream observable mask and integer optimum"
            : "upstream integer optimum; equal-weight observable differences require actual candidate parity/weight/observable witness")
        << "\"}\n";
}
#endif
