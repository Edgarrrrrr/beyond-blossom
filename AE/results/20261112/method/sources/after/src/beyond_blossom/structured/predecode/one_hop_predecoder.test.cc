// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/structured/predecode/one_hop_predecoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"

#include "beyond_blossom/structured/driver/mwpm_decoding.h"

namespace {
using beyond_blossom::structured::Mwpm;
using beyond_blossom::structured::PredecodeSeed;
struct Edge {
    size_t u;
    int v;
    int weight;
};

Mwpm decoder_for(size_t count, const std::vector<Edge>& edges) {
    beyond_blossom::structured::MatchingGraph graph(count, edges.size());
    for (size_t k = 0; k < edges.size(); k++) {
        const auto e = edges[k];
        if (e.v < 0)
            graph.add_boundary_edge(e.u, e.weight, {k});
        else
            graph.add_edge(e.u, size_t(e.v), e.weight, {k});
    }
    // Detector IDs 0,1,2,3 are a,b,c,d around a square, with a-b
    // horizontal and a-c vertical. All edges fit the unrotated stencil.
    const std::array<std::array<int64_t, 3>, 4> coordinates{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {2, 2, 0}}};
    beyond_blossom::StaticGridSpec spec{3, 3, false, {}, {}, beyond_blossom::GridSimd::scalar};
    spec.coordinates.assign(coordinates.begin(), coordinates.begin() + count);
    graph.configure_grid(spec);
    beyond_blossom::structured::GraphFlooder flooder(std::move(graph));
    flooder.sync_negative_weight_observables_and_detection_events();
    return Mwpm(std::move(flooder));
}

std::vector<uint64_t> syndrome(size_t bits, size_t count) {
    std::vector<uint64_t> events;
    for (size_t k = 0; k < count; k++)
        if ((bits >> k) & 1)
            events.push_back(k);
    return events;
}

struct Oracle {
    int64_t minimum = std::numeric_limits<int64_t>::max();
    std::set<uint64_t> minimizers;
};

// Directly enumerate correction subsets on the original signed-weight graph.
// This oracle does not use shortest paths, matching, predecoder state or its duals.
Oracle enumerate_corrections(const std::vector<Edge>& edges, size_t expected_parity) {
    Oracle result;
    for (uint64_t mask = 0; mask < (uint64_t{1} << edges.size()); mask++) {
        size_t parity = 0;
        int64_t weight = 0;
        for (size_t k = 0; k < edges.size(); k++) {
            if (!((mask >> k) & 1))
                continue;
            const auto e = edges[k];
            parity ^= size_t{1} << e.u;
            if (e.v >= 0)
                parity ^= size_t{1} << e.v;
            weight += e.weight;
        }
        if (parity != expected_parity || weight > result.minimum)
            continue;
        if (weight < result.minimum) {
            result.minimum = weight;
            result.minimizers.clear();
        }
        result.minimizers.insert(mask);
    }
    return result;
}

void assert_dual_and_matching(const Mwpm& decoder, const std::vector<Edge>& edges) {
    const auto& p = decoder.predecoder;
    std::vector<uint64_t> radius(decoder.flooder.graph.num_nodes);
    std::vector<bool> seen(radius.size());
    for (size_t k = 0; k < p.seed_view.storage.size(); k++) {
        const auto& s = p.seed_view.storage[k];
        ASSERT_LT(s.detector_id, radius.size());
        ASSERT_FALSE(seen[s.detector_id]);
        seen[s.detector_id] = true;
        radius[s.detector_id] = s.radius;
        EXPECT_EQ(s.radius & 1, 0U);
        const auto& node = decoder.flooder.graph.node(s.detector_id);
        // A run that has not yet imported seeds must leave formal state untouched.
        EXPECT_EQ(node.region_that_arrived, beyond_blossom::structured::NO_REGION);
        const auto& grid = *node.grid_component;
        if (s.mate == PredecodeSeed::UNMATCHED)
            continue;
        ASSERT_TRUE(node.has_edge(s.match_slot));
        if (s.mate == PredecodeSeed::BOUNDARY) {
            EXPECT_EQ(node.neighbor(s.match_slot), nullptr);
            EXPECT_EQ(s.radius, node.edge_weight(s.match_slot));
        } else {
            ASSERT_LT(s.mate, p.seed_view.storage.size());
            const auto& other = p.seed_view.storage[s.mate];
            EXPECT_EQ(other.mate, k);
            EXPECT_EQ(other.match_slot, s.match_slot ^ 1);
            EXPECT_EQ(node.neighbor(s.match_slot)->detector_id, other.detector_id);
            EXPECT_EQ(uint64_t(s.radius) + other.radius, grid.weights[s.cell][s.match_slot]);
        }
    }
    for (const auto e : edges) {
        if (e.v == int(e.u))  // Self-loops are removed by negative-weight preprocessing.
            continue;
        EXPECT_LE(radius[e.u] + (e.v < 0 ? 0 : radius[e.v]), uint64_t(std::abs(e.weight)));
    }
    for (const auto& component : decoder.flooder.graph.grid->components) {
        const auto& workspace = *component;
        for (size_t cell = 0; cell < workspace.predecode.active.size(); cell++) {
            if (!workspace.predecode.active[cell]) {
                EXPECT_EQ(workspace.predecode.radii[cell], 0U);
                continue;
            }
            const auto& s = p.seed_view.storage[workspace.predecode.active[cell] - 1];
            EXPECT_EQ(s.cell, cell);
            EXPECT_EQ(s.radius, workspace.predecode.radii[cell]);
        }
    }
    EXPECT_TRUE(decoder.flooder.queue.empty());
    if (p.complete) {
        EXPECT_EQ(p.result.weight, std::accumulate(radius.begin(), radius.end(), uint64_t{0}));
        for (const auto& s : p.seed_view.storage)
            EXPECT_NE(s.mate, PredecodeSeed::UNMATCHED);
    }
}

void verify_case(Mwpm& decoder, const std::vector<Edge>& edges, size_t bits) {
    const auto expected = enumerate_corrections(edges, bits);
    const auto events = syndrome(bits, decoder.flooder.graph.num_nodes);
    decoder.predecoder.prepare(decoder.flooder.graph, events, decoder.flooder.negative_weight_detection_events);
    decoder.predecoder.run();
    assert_dual_and_matching(decoder, edges);
    if (expected.minimizers.empty()) {
        EXPECT_THROW(
            beyond_blossom::structured::decode_detection_events_for_up_to_64_observables(decoder, events, false),
            std::invalid_argument);
    } else {
        const auto actual = beyond_blossom::structured::decode_detection_events_for_up_to_64_observables(decoder, events, false);
        EXPECT_EQ(actual.weight, expected.minimum);
        // Unique observable per edge reconstructs the entire correction, including
        // ties: accept exactly the independently enumerated minimum-cost subsets.
        EXPECT_TRUE(expected.minimizers.contains(actual.obs_mask)) << "correction=" << actual.obs_mask;
    }
    EXPECT_TRUE(decoder.flooder.queue.empty());
    EXPECT_EQ(decoder.node_arena.allocated.size(), decoder.node_arena.available.size());
    for (size_t k = 0; k < decoder.flooder.graph.num_nodes; k++) {
        const auto& node = decoder.flooder.graph.node(k);
        EXPECT_EQ(node.region_that_arrived, beyond_blossom::structured::NO_REGION);
        EXPECT_EQ(node.grid_component->collision_states[node.grid_cell].top_region_id, 0U);
    }
}

void run_imported_to_completion(Mwpm& decoder) {
    bool done = false;
    for (size_t k = 0; k < 1000; k++) {
        const auto event = decoder.flooder.run_until_next_mwpm_notification();
        if (event.event_type == beyond_blossom::structured::NO_EVENT) {
            done = true;
            break;
        }
        decoder.process_event(event);
    }
    ASSERT_TRUE(done);
    EXPECT_EQ(decoder.node_arena.allocated.size(), decoder.node_arena.available.size());
}
}  // namespace

namespace {
thread_local void (*observed_bound_kernel)(beyond_blossom::structured::OneHopBoundRange&) = nullptr;
thread_local size_t bound_kernel_calls = 0;
void counted_bound_kernel(beyond_blossom::structured::OneHopBoundRange& range) {
    ++bound_kernel_calls;
    observed_bound_kernel(range);
}
}  // namespace

TEST(OneHopPredecoder, BoundKernelPreservesSeedStateAcrossTiesAndInputOrders) {
    // Distinct fault masks on every edge make different equal-weight choices observable.
    for (int a : {0, 1, 2, 6}) {
        const std::vector<Edge> edges{{0, 1, a}, {1, 3, 2}, {0, 2, 2}, {2, 3, 4}, {0, -1, 3}, {3, -1, 2}};
        auto fast = decoder_for(4, edges);
        auto reference = decoder_for(4, edges);
        auto& grid = *fast.flooder.graph.grid->components.front();
#if BB_PREDECODE_SIMD && defined(__APPLE__) && defined(__aarch64__) && BB_STRUCTURED_STORAGE_DIRECTIONS == 12 && \
    !BB_PREDECODE_CAP_CACHE && !BB_STRUCTURED_PROFILE_COUNTERS && !BB_STRUCTURED_PROFILE_TIMERS && !BB_PREDECODE_DIAGNOSTICS
        ASSERT_NE(grid.predecode.run_bound_range, nullptr);
#endif
        if (!grid.predecode.run_bound_range)
            GTEST_SKIP() << "Construction-bound kernel unavailable in this build.";
        ASSERT_EQ(fast.single_predecode_grid(), &grid);
        observed_bound_kernel = grid.predecode.run_bound_range;
        grid.predecode.run_bound_range = counted_bound_kernel;
        for (unsigned bits = 0; bits < 16; ++bits) {
            auto events = syndrome(bits, 4);
            do {
                auto& f = fast.predecoder;
                auto& r = reference.predecoder;
                f.prepare(fast.flooder.graph, events, {});
                r.prepare(reference.flooder.graph, events, {});
                const auto before = bound_kernel_calls;
                f.run(&grid);
                EXPECT_EQ(bound_kernel_calls - before, events.empty() ? 0U : 1U);
                r.run();
                ASSERT_EQ(f.complete, r.complete);
                ASSERT_EQ(f.result.obs_mask, r.result.obs_mask);
                ASSERT_EQ(f.result.weight, r.result.weight);
                ASSERT_EQ(f.seed_view.storage.size(), r.seed_view.storage.size());
                for (size_t i = 0; i < f.seed_view.storage.size(); ++i) {
                    const auto& actual = f.seed_view.storage[i];
                    const auto& want = r.seed_view.storage[i];
                    EXPECT_EQ(actual.detector_id, want.detector_id);
                    EXPECT_EQ(actual.mate, want.mate);
                    EXPECT_EQ(actual.match_slot, want.match_slot);
                    EXPECT_EQ(actual.radius, want.radius);
                }
                EXPECT_EQ(grid.predecode.radii, reference.flooder.graph.grid->components.front()->predecode.radii);
            } while (std::next_permutation(events.begin(), events.end()));
        }
    }
}

TEST(OneHopPredecoder, ExhaustiveSquareWeightsSyndromesAndBoundaries) {
    size_t cases = 0;
    for (size_t encoded = 0; encoded < 81; encoded++) {
        size_t code = encoded;
        std::array<int, 4> weights;
        for (auto& w : weights) {
            w = std::array<int, 3>{0, 2, 6}[code % 3];
            code /= 3;
        }
        for (size_t boundary_mask : {size_t{0}, size_t{1}, size_t{9}, size_t{15}}) {
            std::vector<Edge> edges{{0, 1, weights[0]}, {0, 2, weights[1]}, {1, 3, weights[2]}, {2, 3, weights[3]}};
            for (size_t u = 0; u < 4; u++)
                if ((boundary_mask >> u) & 1)
                    edges.push_back({u, -1, int(2 * (u % 3))});
            auto decoder = decoder_for(4, edges);
            for (size_t bits = 0; bits < 16; bits++) {
                SCOPED_TRACE(
                    ::testing::Message() << "weights=" << encoded << " boundary=" << boundary_mask << " bits=" << bits);
                verify_case(decoder, edges, bits);
                cases++;
            }
        }
    }
    EXPECT_EQ(cases, 5184);
}

TEST(OneHopPredecoder, FusedSerialLeavesPreserveStableOrderAndRepeatedRuns) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    using State = std::tuple<uint32_t, beyond_blossom::weight_int, uint8_t>;
    const auto capture = [](const Predecoder& pre) {
        std::vector<State> result;
        for (const auto& seed : pre.seed_view.storage)
            result.emplace_back(seed.mate, seed.radius, seed.match_slot);
        return result;
    };
    for (size_t encoded = 0; encoded < 256; ++encoded) {
        size_t code = encoded;
        std::array<int, 4> weights;
        for (auto& weight : weights) {
            weight = std::array<int, 4>{0, 1, 2, 6}[code % 4];
            code /= 4;
        }
        for (size_t boundary_mask : {size_t{0}, size_t{1}, size_t{9}, size_t{15}}) {
            std::vector<Edge> edges{{0, 1, weights[0]}, {0, 2, weights[1]}, {1, 3, weights[2]}, {2, 3, weights[3]}};
            for (size_t node = 0; node < 4; ++node)
                if ((boundary_mask >> node) & 1)
                    edges.push_back({node, -1, int(node % 3)});
            auto decoder = decoder_for(4, edges);
            auto& pre = decoder.predecoder;
            // Exercise aligned fixed storage as well as the dynamic compatibility path.
            if (encoded & 2) pre.bind_serial_scratch(4);
            auto& grid = *decoder.flooder.graph.grid->components[0];
            const auto sparse_cap = grid.predecode.sparse_radius_limit;
            for (size_t bits = 0; bits < 16; ++bits) {
                SCOPED_TRACE(::testing::Message() << "weights=" << encoded << " boundary=" << boundary_mask
                                                 << " bits=" << bits);
                auto events = syndrome(bits, 4);
                // Exercise both source orders, since degree-one priority must
                // follow the prepared seed order rather than detector IDs.
                if (encoded & 1)
                    std::reverse(events.begin(), events.end());
                std::array<std::vector<State>, 2> expected_states;
                std::array<beyond_blossom::MatchingResult, 2> expected_results;
                std::array<bool, 2> expected_complete;
                // Keep the sorted reference on the original all-neighbor cap
                // even when the candidate uses its known syndrome mask.
                grid.predecode.sparse_radius_limit = [](const Predecoder::Grid& grid, size_t cell, uint16_t) {
                    return grid.predecode.radius_limit(grid, cell);
                };
                pre.prepare(decoder.flooder.graph, events, {});
                for (size_t repeat = 0; repeat < 2; ++repeat) {
                    pre.prepare_scratch();
                    Predecoder::PartialResult partial;
                    const auto order = pre.prepare_order(0, pre.seed_view.storage.size(), partial);
                    pre.run_order(order, partial);
                    pre.merge(partial);
                    pre.finish();
                    expected_states[repeat] = capture(pre);
                    expected_results[repeat] = pre.result;
                    expected_complete[repeat] = pre.complete;
                }
                grid.predecode.sparse_radius_limit = sparse_cap;
                pre.prepare(decoder.flooder.graph, events, {});
                for (size_t repeat = 0; repeat < 2; ++repeat) {
                    pre.run();
                    EXPECT_EQ(capture(pre), expected_states[repeat]);
                    EXPECT_EQ(pre.result, expected_results[repeat]);
                    EXPECT_EQ(pre.complete, expected_complete[repeat]);
                    EXPECT_EQ(pre.result_pending, expected_complete[repeat]);
                    assert_dual_and_matching(decoder, edges);
                }
            }
        }
    }
}

TEST(OneHopPredecoder, FusedPartitionPrefixesMatchExplicitSortWithoutTouchingHolesOrOtherScratch) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    using State = std::tuple<uint32_t, uint32_t, beyond_blossom::weight_int, uint8_t>;
    const auto capture = [](const Predecoder& pre) {
        std::vector<State> result;
        for (const auto& range : pre.seed_view.active_ranges())
            for (size_t i = range.begin; i < range.end; ++i) {
                const auto& seed = pre.seed_view.storage[i];
                result.emplace_back(seed.detector_id, seed.mate, seed.radius, seed.match_slot);
            }
        return result;
    };
    const auto check = [&]<size_t Parts>() {
        constexpr size_t Buckets = Parts + 1;
        std::array<size_t, Buckets + 1> bases{};
        for (size_t b = 0; b <= Buckets; ++b) bases[b] = 4 * b;
        for (size_t encoded = 0; encoded < 256; ++encoded) {
            size_t code = encoded;
            std::array<int, 4> weights{};
            for (auto& weight : weights) {
                weight = std::array<int, 4>{0, 1, 2, 6}[code % 4];
                code /= 4;
            }
            for (size_t boundary_mask : {size_t{0}, size_t{1}, size_t{9}, size_t{15}}) {
                std::vector<Edge> edges{{0,1,weights[0]}, {0,2,weights[1]}, {1,3,weights[2]}, {2,3,weights[3]}};
                for (size_t id = 0; id < 4; ++id)
                    if ((boundary_mask >> id) & 1) edges.push_back({id, -1, int(id % 3)});
                auto sorted = decoder_for(4, edges);
                auto fused = decoder_for(4, edges);
                sorted.predecoder.bind_fixed_scratch(bases);
                fused.predecoder.bind_fixed_scratch(bases);
                auto& reference_grid = *sorted.flooder.graph.grid->components[0];
                reference_grid.predecode.sparse_radius_limit = [](const Predecoder::Grid& grid, size_t cell, uint16_t) {
                    return grid.predecode.radius_limit(grid, cell);
                };
                std::array<PredecodeSeed, Buckets * 4> sorted_storage{}, fused_storage{};
                for (size_t bits = 0; bits < 16; ++bits) {
                    SCOPED_TRACE(::testing::Message() << "parts=" << Parts << " weights=" << encoded
                        << " boundary=" << boundary_mask << " bits=" << bits);
                    auto events = syndrome(bits, 4);
                    if (encoded & 1) std::reverse(events.begin(), events.end());
                    std::array<size_t, Buckets> counts{};
                    // Rotating the synthetic bucket assignment exercises both
                    // low and high physical offsets, including empty buckets.
                    const auto bucket_for = [&](uint32_t id) { return (id + encoded / 4) % Buckets; };
                    const auto stage = [&](Mwpm& decoder, auto& backing) {
                        auto& pre = decoder.predecoder;
                        pre.prepare(decoder.flooder.graph, events, {});
                        counts.fill(0);
                        for (auto& hole : backing) {
                            hole = {};
                            hole.grid = nullptr;
                            hole.detector_id = UINT32_MAX;
                        }
                        for (const auto& seed : pre.seed_view.storage) {
                            const auto b = bucket_for(seed.detector_id);
                            const auto slot = bases[b] + counts[b]++;
                            backing[slot] = seed;
                            seed.grid->predecode.active[seed.cell] = uint32_t(slot + 1);
                        }
                        pre.seed_view.bind_fixed(backing, bases);
                        pre.seed_view.set_counts(counts);
                        pre.activate_fixed_scratch();
                    };
                    stage(sorted, sorted_storage);
                    stage(fused, fused_storage);
                    const auto layout = fused.predecoder.scratch_layout();
                    for (const auto& range : layout) {
                        std::fill_n(reinterpret_cast<uint16_t*>(range.mask_address), 4, uint16_t{0xa5a5});
                        std::fill_n(reinterpret_cast<uint32_t*>(range.order_address), 4, uint32_t{0xdeadbeef});
                        std::fill_n(reinterpret_cast<uint8_t*>(range.degree_address), 4, uint8_t{0xa5});
                    }
                    for (size_t repeat = 0; repeat < 2; ++repeat) {
                        for (size_t b = 0; b < Buckets; ++b) {
                            Predecoder::PartialResult a, z;
                            const auto order = sorted.predecoder.prepare_partition_order(b, counts[b], a);
                            sorted.predecoder.run_order(order, a);
                            fused.predecoder.run_partition_fused(b, counts[b], z);
                            EXPECT_EQ(a.result, z.result);
                            EXPECT_EQ(capture(sorted.predecoder), capture(fused.predecoder));
                            sorted.predecoder.merge(a);
                            fused.predecoder.merge(z);
                            for (size_t other = 0; other < Buckets; ++other) {
                                const auto& range = layout[other];
                                const auto* mask = reinterpret_cast<const uint16_t*>(range.mask_address);
                                const auto* link = reinterpret_cast<const uint32_t*>(range.order_address);
                                const auto* degree = reinterpret_cast<const uint8_t*>(range.degree_address);
                                // Before its first visit a bucket's entire
                                // scratch must remain untouched by other owners.
                                const auto first = repeat == 0 && other > b ? 0 : counts[other];
                                for (size_t i = first; i < 4; ++i) {
                                    EXPECT_EQ(mask[i], 0xa5a5);
                                    EXPECT_EQ(link[i], 0xdeadbeef);
                                }
                                for (size_t i = 0; i < 4; ++i) EXPECT_EQ(degree[i], 0xa5);
                            }
                        }
                        sorted.predecoder.finish();
                        fused.predecoder.finish();
                        EXPECT_EQ(sorted.predecoder.result, fused.predecoder.result);
                        EXPECT_EQ(sorted.predecoder.complete, fused.predecoder.complete);
                        EXPECT_EQ(sorted.predecoder.result_pending, fused.predecoder.result_pending);
                    }
                    // All backing spans retire before the stack arrays expire.
                    sorted.reset();
                    fused.reset();
                }
            }
        }
    };
    check.template operator()<2>();
    check.template operator()<4>();
    check.template operator()<8>();
}

TEST(OneHopPredecoder, BoundPartitionPrefixesMatchExplicitSortWithoutTouchingHolesOrOtherScratch) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    using State = std::tuple<uint32_t, uint32_t, beyond_blossom::weight_int, uint8_t>;
    const auto capture = [](const Predecoder& pre) {
        std::vector<State> result;
        for (const auto& range : pre.seed_view.active_ranges())
            for (size_t i = range.begin; i < range.end; ++i) {
                const auto& seed = pre.seed_view.storage[i];
                result.emplace_back(seed.detector_id, seed.mate, seed.radius, seed.match_slot);
            }
        return result;
    };
    const auto check = [&]<size_t Parts>() {
        constexpr size_t Buckets = Parts + 1;
        std::array<size_t, Buckets + 1> bases{};
        for (size_t b = 0; b <= Buckets; ++b) bases[b] = 4 * b;
        for (size_t encoded = 0; encoded < 256; ++encoded) {
            size_t code = encoded;
            std::array<int, 4> weights{};
            for (auto& weight : weights) {
                weight = std::array<int, 4>{0, 1, 2, 6}[code % 4];
                code /= 4;
            }
            for (size_t boundary_mask : {size_t{0}, size_t{1}, size_t{9}, size_t{15}}) {
                std::vector<Edge> edges{{0,1,weights[0]}, {0,2,weights[1]}, {1,3,weights[2]}, {2,3,weights[3]}};
                for (size_t id = 0; id < 4; ++id)
                    if ((boundary_mask >> id) & 1) edges.push_back({id, -1, int(id % 3)});
                auto sorted = decoder_for(4, edges);
                auto fused = decoder_for(4, edges);
                auto* bound_grid = fused.single_predecode_grid();
                ASSERT_NE(bound_grid, nullptr);
#if BB_PREDECODE_SIMD && defined(__APPLE__) && defined(__aarch64__) && BB_STRUCTURED_STORAGE_DIRECTIONS == 12 && \
    !BB_PREDECODE_CAP_CACHE && !BB_STRUCTURED_PROFILE_COUNTERS && !BB_STRUCTURED_PROFILE_TIMERS && !BB_PREDECODE_DIAGNOSTICS
                ASSERT_NE(bound_grid->predecode.run_bound_range, nullptr);
#endif
                if (!bound_grid->predecode.run_bound_range)
                    GTEST_SKIP() << "Construction-bound kernel unavailable in this build.";
                sorted.predecoder.bind_fixed_scratch(bases);
                fused.predecoder.bind_fixed_scratch(bases);
                auto& reference_grid = *sorted.flooder.graph.grid->components[0];
                reference_grid.predecode.sparse_radius_limit = [](const Predecoder::Grid& grid, size_t cell, uint16_t) {
                    return grid.predecode.radius_limit(grid, cell);
                };
                std::array<PredecodeSeed, Buckets * 4> sorted_storage{}, fused_storage{};
                for (size_t bits = 0; bits < 16; ++bits) {
                    SCOPED_TRACE(::testing::Message() << "parts=" << Parts << " weights=" << encoded
                        << " boundary=" << boundary_mask << " bits=" << bits);
                    auto events = syndrome(bits, 4);
                    if (encoded & 1) std::reverse(events.begin(), events.end());
                    std::array<size_t, Buckets> counts{};
                    // Rotating the synthetic bucket assignment exercises both
                    // low and high physical offsets, including empty buckets.
                    const auto bucket_for = [&](uint32_t id) { return (id + encoded / 4) % Buckets; };
                    const auto stage = [&](Mwpm& decoder, auto& backing) {
                        auto& pre = decoder.predecoder;
                        pre.prepare(decoder.flooder.graph, events, {});
                        counts.fill(0);
                        for (auto& hole : backing) {
                            hole = {};
                            hole.grid = nullptr;
                            hole.detector_id = UINT32_MAX;
                        }
                        for (const auto& seed : pre.seed_view.storage) {
                            const auto b = bucket_for(seed.detector_id);
                            const auto slot = bases[b] + counts[b]++;
                            backing[slot] = seed;
                            seed.grid->predecode.active[seed.cell] = uint32_t(slot + 1);
                        }
                        pre.seed_view.bind_fixed(backing, bases);
                        pre.seed_view.set_counts(counts);
                        pre.activate_fixed_scratch();
                    };
                    stage(sorted, sorted_storage);
                    stage(fused, fused_storage);
                    const auto layout = fused.predecoder.scratch_layout();
                    for (const auto& range : layout) {
                        std::fill_n(reinterpret_cast<uint16_t*>(range.mask_address), 4, uint16_t{0xa5a5});
                        std::fill_n(reinterpret_cast<uint32_t*>(range.order_address), 4, uint32_t{0xdeadbeef});
                        std::fill_n(reinterpret_cast<uint8_t*>(range.degree_address), 4, uint8_t{0xa5});
                    }
                    for (size_t repeat = 0; repeat < 2; ++repeat) {
                        for (size_t b = 0; b < Buckets; ++b) {
                            Predecoder::PartialResult a, z;
                            const auto order = sorted.predecoder.prepare_partition_order(b, counts[b], a);
                            sorted.predecoder.run_order(order, a);
                            fused.predecoder.run_partition_fused(b, counts[b], z, bound_grid);
                            EXPECT_EQ(a.result, z.result);
                            EXPECT_EQ(capture(sorted.predecoder), capture(fused.predecoder));
                            sorted.predecoder.merge(a);
                            fused.predecoder.merge(z);
                            for (size_t other = 0; other < Buckets; ++other) {
                                const auto& range = layout[other];
                                const auto* mask = reinterpret_cast<const uint16_t*>(range.mask_address);
                                const auto* link = reinterpret_cast<const uint32_t*>(range.order_address);
                                const auto* degree = reinterpret_cast<const uint8_t*>(range.degree_address);
                                // Before its first visit a bucket's entire
                                // scratch must remain untouched by other owners.
                                const auto first = repeat == 0 && other > b ? 0 : counts[other];
                                for (size_t i = first; i < 4; ++i) {
                                    EXPECT_EQ(mask[i], 0xa5a5);
                                    EXPECT_EQ(link[i], 0xdeadbeef);
                                }
                                for (size_t i = 0; i < 4; ++i) EXPECT_EQ(degree[i], 0xa5);
                            }
                        }
                        sorted.predecoder.finish();
                        fused.predecoder.finish();
                        EXPECT_EQ(sorted.predecoder.result, fused.predecoder.result);
                        EXPECT_EQ(sorted.predecoder.complete, fused.predecoder.complete);
                        EXPECT_EQ(sorted.predecoder.result_pending, fused.predecoder.result_pending);
                    }
                    // All backing spans retire before the stack arrays expire.
                    sorted.reset();
                    fused.reset();
                }
            }
        }
    };
    check.template operator()<2>();
    check.template operator()<4>();
    check.template operator()<8>();
}

TEST(OneHopPredecoder, NonSyndromeNeighborsBoundGrowthAndEnableMultihopSolution) {
    // a-b direct costs 10; the path a-c-d-b costs 6. c,d are not syndrome.
    const std::vector<Edge> edges{{0, 1, 10}, {0, 2, 2}, {2, 3, 2}, {3, 1, 2}};
    auto decoder = decoder_for(4, edges);
    decoder.predecoder.prepare(decoder.flooder.graph, {0, 1}, {});
    decoder.predecoder.run();
    EXPECT_FALSE(decoder.predecoder.complete);
    for (const auto& seed : decoder.predecoder.seed_view.storage)
        EXPECT_LE(seed.radius, 2U);
    verify_case(decoder, edges, 3);
}

TEST(OneHopPredecoder, PartialTightPairRemainsReversible) {
    const std::vector<Edge> edges{{0, 1, 2}, {0, 2, 2}, {1, 3, 2}, {2, 3, 10}};
    auto decoder = decoder_for(4, edges);
    auto& graph = decoder.flooder.graph;
    const uint8_t slot = graph.node(0).index_of_neighbor(&graph.node(1));
    const std::vector<PredecodeSeed> seeds{
        {0, uint32_t(graph.node(0).grid_cell), 1, 2, slot},
        {1, uint32_t(graph.node(1).grid_cell), 0, 0, uint8_t(slot ^ 1)},
        {2, uint32_t(graph.node(2).grid_cell), PredecodeSeed::UNMATCHED, 0, 0},
        {3, uint32_t(graph.node(3).grid_cell), PredecodeSeed::UNMATCHED, 0, 0}};
    decoder.import_predecode_state(seeds);
    run_imported_to_completion(decoder);
    beyond_blossom::MatchingResult actual;
    for (size_t id : {0, 1, 2, 3}) {
        auto& node = graph.node(id);
        if (node.region_that_arrived)
            actual += decoder.shatter_blossom_and_extract_matches(node.top_region());
    }
    EXPECT_EQ(actual.weight, 4);
    EXPECT_EQ(actual.obs_mask, (uint64_t{1} << 1) | (uint64_t{1} << 2));
    verify_case(decoder, edges, 15);
}

TEST(OneHopPredecoder, DeferredSeedCleanupShattersBlossomAndImmediatelyClearsGrownNodes) {
    const auto fixture = [] {
        auto decoder = decoder_for(4, {{0, 1, 2}, {0, 2, 2}, {1, 3, 2}, {2, 3, 2}, {0, -1, 12}});
        auto& graph = decoder.flooder.graph;
        decoder.predecoder.prepare(graph, {0, 1, 2}, {});
        decoder.import_predecode_state(decoder.predecoder.seed_view.storage);
        std::array<beyond_blossom::structured::GraphFillRegion*, 3> leaves;
        for (size_t i = 0; i < leaves.size(); ++i) {
            leaves[i] = graph.node(i).top_region();
            // Complete the synthetic roots through the matcher so its live
            // forest state agrees with this extraction-only fixture.
            decoder.handle_tree_hitting_boundary({leaves[i]->region_id, {&graph.node(i), nullptr, 0}});
            leaves[i]->radius_value() = beyond_blossom::VaryingCT::from_base_and_growth(2, 0);
        }
        // An explicitly completed three-child blossom isolates final extraction
        // from event scheduling. Node 3 was grown into the parent shell; it is
        // not an imported seed and must still receive immediate full cleanup.
        std::vector<beyond_blossom::structured::RegionEdge> cycle{
            {leaves[0]->region_id, {&graph.node(0), &graph.node(1), 1}},
            {leaves[1]->region_id, {&graph.node(1), &graph.node(2), 2}},
            {leaves[2]->region_id, {&graph.node(2), &graph.node(0), 4}}};
        auto* blossom = decoder.flooder.create_blossom(cycle);
        blossom->radius_value() = beyond_blossom::VaryingCT::from_base_and_growth(4, 0);
        decoder.flooder.do_region_arriving_at_empty_detector_node(
            *blossom, graph.node(3), graph.node(1), graph.node(1).index_of_neighbor(&graph.node(3)));
        blossom->match = {beyond_blossom::structured::NO_REGION, {&graph.node(0), nullptr, 8}};
        decoder.flooder.queue.clear();
        return decoder;
    };
    auto immediate = fixture();
    const auto expected = beyond_blossom::structured::decode_prepared_detection_events_for_up_to_64_observables(
        immediate, false, true);
    EXPECT_EQ(expected, (beyond_blossom::MatchingResult{10, 10}));
    EXPECT_FALSE(immediate.has_pending_seed_cleanup());
    auto deferred = fixture();
    EXPECT_EQ(beyond_blossom::structured::solve_prepared_detection_events_deferring_seed_nodes(deferred), expected);
    EXPECT_TRUE(deferred.has_pending_seed_cleanup());
    EXPECT_TRUE(deferred.flooder.regions->empty());
    for (size_t i = 0; i < 4; ++i) {
        const auto& node = deferred.flooder.graph.node(i);
        EXPECT_EQ(node.region_that_arrived, beyond_blossom::structured::NO_REGION);
        EXPECT_EQ(node.top_region(), nullptr);  // No access to released pool IDs.
        EXPECT_EQ(node.local_radius(), beyond_blossom::VaryingCT{0});
        EXPECT_EQ(node.reached_from_source, i < 3 ? &node : nullptr);
    }
    Mwpm moved(std::move(deferred));
    EXPECT_FALSE(deferred.has_pending_seed_cleanup());
    EXPECT_TRUE(moved.has_pending_seed_cleanup());
    moved.clear_pending_seed_nodes();
    EXPECT_FALSE(moved.has_pending_seed_cleanup());
    for (size_t i = 0; i < 4; ++i) {
        const auto& a = moved.flooder.graph.node(i);
        const auto& b = immediate.flooder.graph.node(i);
        EXPECT_EQ(a.reached_from_source, b.reached_from_source);
        EXPECT_EQ(a.observables_crossed_from_source, b.observables_crossed_from_source);
        EXPECT_EQ(a.radius_of_arrival, b.radius_of_arrival);
        EXPECT_EQ(a.top_region_id(), b.top_region_id());
        EXPECT_EQ(a.grid_component->collision_states[a.grid_cell].wrapped_radius_cached,
                  b.grid_component->collision_states[b.grid_cell].wrapped_radius_cached);
        EXPECT_EQ(a.node_event_tracker.desired_time.value, b.node_event_tracker.desired_time.value);
        EXPECT_EQ(a.node_event_tracker.queued_time.value, b.node_event_tracker.queued_time.value);
        EXPECT_EQ(a.node_event_tracker.has_desired_time, b.node_event_tracker.has_desired_time);
        EXPECT_EQ(a.node_event_tracker.has_queued_time, b.node_event_tracker.has_queued_time);
    }
    // Model an exception after terminal extraction but before its successful
    // whole-shot pending commit. Hard recovery must not rely on that token.
    auto interrupted = fixture();
    EXPECT_THROW(([&] {
        interrupted.shatter_blossom_and_extract_matches_deferring_seed_nodes(
            interrupted.flooder.graph.node(0).top_region());
        throw std::runtime_error("injected extraction interruption");
    }()), std::runtime_error);
    EXPECT_FALSE(interrupted.has_pending_seed_cleanup());
    interrupted.reset();
    for (size_t i = 0; i < 4; ++i) {
        const auto& n = interrupted.flooder.graph.node(i);
        EXPECT_EQ(n.region_that_arrived, beyond_blossom::structured::NO_REGION);
        EXPECT_EQ(n.top_region(), nullptr);
        EXPECT_EQ(n.reached_from_source, nullptr);
        EXPECT_EQ(n.grid_component->collision_states[n.grid_cell].wrapped_radius_cached, 0);
        EXPECT_FALSE(n.node_event_tracker.has_queued_time);
    }
}

TEST(OneHopPredecoder, FrozenBoundaryMatchCanBeReplaced) {
    const std::vector<Edge> edges{{0, 1, 2}, {0, -1, 2}, {1, -1, 10}};
    auto decoder = decoder_for(2, edges);
    auto& graph = decoder.flooder.graph;
    const uint8_t slot = graph.node(0).index_of_neighbor(nullptr);
    const std::vector<PredecodeSeed> seeds{
        {0, uint32_t(graph.node(0).grid_cell), PredecodeSeed::BOUNDARY, 2, slot},
        {1, uint32_t(graph.node(1).grid_cell), PredecodeSeed::UNMATCHED, 0, 0}};
    decoder.import_predecode_state(seeds);
    run_imported_to_completion(decoder);
    beyond_blossom::MatchingResult actual;
    for (size_t id : {0, 1}) {
        auto& node = graph.node(id);
        if (node.region_that_arrived)
            actual += decoder.shatter_blossom_and_extract_matches(node.top_region());
    }
    EXPECT_EQ(actual.weight, 2);
    EXPECT_EQ(actual.obs_mask, 1U);
}

TEST(OneHopPredecoder, BoundaryPriorityAndZeroTightEdges) {
    for (const auto& edges : std::vector<std::vector<Edge>>{
             {{0, 1, 10}, {0, -1, 2}, {1, -1, 2}},
             {{0, 1, 0}, {0, -1, 0}, {1, -1, 0}},
             {{0, 1, 2}, {0, -1, 0}, {1, -1, 4}}}) {
        auto decoder = decoder_for(2, edges);
        for (size_t bits = 0; bits < 4; bits++)
            verify_case(decoder, edges, bits);
    }
}

TEST(OneHopPredecoder, NegativeWeightsAndSelfLoopsUseOriginalSignedObjective) {
    for (const auto& edges : std::vector<std::vector<Edge>>{
             {{0, 1, -2}, {0, -1, 4}, {1, -1, 6}},
             {{0, 1, 2}, {0, -1, -4}, {1, -1, 6}},
             {{0, 1, 2}, {0, -1, 4}, {1, -1, 6}, {0, 0, -8}},
             {{0, 1, -2}, {0, 2, -4}, {1, 3, 2}, {2, 3, 6}, {0, -1, -2}}}) {
        const size_t count = edges.size() == 5 ? 4 : 2;
        auto decoder = decoder_for(count, edges);
        for (size_t repeat = 0; repeat < 3; repeat++)
            for (size_t bits = 0; bits < (size_t{1} << count); bits++)
                verify_case(decoder, edges, bits);
    }
}

TEST(OneHopPredecoder, InvalidAndOddInputsDoNotContaminateNextShot) {
    const std::vector<Edge> edges{{0, 1, 2}, {0, 2, 4}};
    auto decoder = decoder_for(3, edges);
    for (size_t repeat = 0; repeat < 3; repeat++) {
        EXPECT_THROW(
            beyond_blossom::structured::decode_detection_events_for_up_to_64_observables(decoder, {0, 0}, false),
            std::invalid_argument);
        verify_case(decoder, edges, 3);
        EXPECT_THROW(
            beyond_blossom::structured::decode_detection_events_for_up_to_64_observables(decoder, {0, UINT64_MAX}, false),
            std::invalid_argument);
        verify_case(decoder, edges, 0);
        verify_case(decoder, edges, 7);  // Odd component with no boundary is infeasible.
        verify_case(decoder, edges, 6);
    }
}

#include <random>

#include "beyond_blossom/structured/predecode/one_hop_simd_kernels.h"

namespace {
template <beyond_blossom::structured::CircuitStencilKind Kind>
void compare_predecode_kernels(const std::array<size_t, 12>& directions) {
    using Box = beyond_blossom::StaticGridBox<5, 5, 3>;
    using Stencil = beyond_blossom::structured::CircuitStencil<Kind>;
    constexpr size_t source = 37;
    beyond_blossom::structured::OneHopPredecoder::Grid grid;
    grid.present.resize(Box::volume);
    grid.weights.resize(Box::volume);
    auto& w = grid;
    w.predecode.radii.resize(Box::volume);
    w.predecode.active.resize(Box::volume);
    std::mt19937_64 rng(20261004 + unsigned(Kind));
    // Enumerate every presence mask, including each single winner in the
    // first eight lanes and final four lanes; generate independent legal radii.
    for (uint16_t mask = 0; mask < 4096; mask++) {
        SCOPED_TRACE(::testing::Message() << "stencil=" << unsigned(Kind) << " mask=" << mask);
        grid.present[source] = mask;
        beyond_blossom::weight_int expected = UINT32_MAX;
        uint16_t expected_syndrome = 0;
        for (size_t slot = 0; slot < 12; slot++) {
            const auto delta = beyond_blossom::SURFACE_OFFSETS[directions[slot]];
            const size_t other = ptrdiff_t(source) + delta[0] + 5 * delta[1] + 25 * delta[2];
            const beyond_blossom::weight_int weight =
                std::array<beyond_blossom::weight_int, 5>{0, 2, 0x80000000U, UINT32_MAX, beyond_blossom::weight_int(rng())}[(mask + slot) % 5];
            grid.weights[source][slot] = weight;
            const auto radius = beyond_blossom::weight_int(rng() % (uint64_t(weight) + 1));
            // Absent lanes may contain values that would underflow; their mask
            // must remove them before the unsigned minimum reduction.
            w.predecode.radii[other] = (mask & (1 << slot)) ? radius : UINT32_MAX;
            w.predecode.active[other] = (rng() & 1) ? 17 : 0;
            if (mask & (1 << slot)) {
                expected = std::min(expected, beyond_blossom::weight_int(uint64_t(weight) - radius));
                if (w.predecode.active[other])
                    expected_syndrome |= uint16_t{1} << slot;
            }
        }
        EXPECT_EQ((beyond_blossom::structured::one_hop_radius_limit<Box, Stencil>(w, source)), expected);
        EXPECT_EQ((beyond_blossom::structured::one_hop_syndrome_mask<Box, Stencil>(w, source)), expected_syndrome);
#if BB_PREDECODE_AVX2_AVAILABLE && BB_STRUCTURED_STORAGE_DIRECTIONS == 12
        if (__builtin_cpu_supports("avx2"))
            EXPECT_EQ((beyond_blossom::structured::one_hop_radius_limit_avx2<Box, Stencil>(w, source)), expected);
#endif
    }
}
}  // namespace

TEST(OneHopPredecoder, AllMasksScalarAndAvx2AgreeWithIndependentRadiusFormula) {
#if BB_STRUCTURED_STORAGE_DIRECTIONS == 12
    // Explicit global directions keep the reference independent of the slot mapper.
    compare_predecode_kernels<beyond_blossom::structured::CircuitStencilKind::x>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13});
    compare_predecode_kernels<beyond_blossom::structured::CircuitStencilKind::z>({0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15});
    compare_predecode_kernels<beyond_blossom::structured::CircuitStencilKind::unrotated>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
#else
    GTEST_SKIP() << "The 8+4 SIMD kernel requires twelve-slot storage.";
#endif
}

TEST(OneHopPredecoder, UnmatchedFrontCrossesEmptyNodeBeforeReachingFrozenPair) {
    // a-b is initially frozen. c grows through empty d, reaches b, and the
    // augmenting path replaces a-b with c-d-b plus a-boundary.
    const std::vector<Edge> edges{{0, 1, 2}, {1, 3, 2}, {2, 3, 2}, {0, -1, 2}, {2, -1, 10}};
    auto decoder = decoder_for(4, edges);
    auto& graph = decoder.flooder.graph;
    const uint8_t slot = graph.node(0).index_of_neighbor(&graph.node(1));
    const std::vector<PredecodeSeed> seeds{
        {0, uint32_t(graph.node(0).grid_cell), 1, 2, slot},
        {1, uint32_t(graph.node(1).grid_cell), 0, 0, uint8_t(slot ^ 1)},
        {2, uint32_t(graph.node(2).grid_cell), PredecodeSeed::UNMATCHED, 0, 0}};
    decoder.import_predecode_state(seeds);
    EXPECT_EQ(graph.node(3).region_that_arrived, beyond_blossom::structured::NO_REGION);
    run_imported_to_completion(decoder);
    beyond_blossom::MatchingResult actual;
    for (size_t id : {0, 1, 2}) {
        auto& node = graph.node(id);
        if (node.region_that_arrived)
            actual += decoder.shatter_blossom_and_extract_matches(node.top_region());
    }
    EXPECT_EQ(actual.weight, 6);
    EXPECT_EQ(actual.obs_mask, 14U);
    verify_case(decoder, edges, 7);
}

TEST(OneHopPredecoder, FiniteCapCacheIncludesZeroAndRebuildsEveryShot) {
    static_assert(sizeof(PredecodeSeed) == 32);
    // The first pair a-b leaves c and d constrained by zero-weight edges.
    // Repeated attempts to grow c or d must reuse a finite radius of zero.
    const std::vector<Edge> edges{{0, 1, 0}, {0, 2, 0}, {1, 3, 0}, {2, 3, 10}};
    auto decoder = decoder_for(4, edges);
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto& p = decoder.predecoder;
        p.prepare(decoder.flooder.graph, {0, 1, 2, 3}, {});
        p.run();
        ASSERT_FALSE(p.complete);
        assert_dual_and_matching(decoder, edges);
        EXPECT_EQ(p.seed_view.storage[2].radius, 0U);
        EXPECT_EQ(p.seed_view.storage[3].radius, 0U);
#if BB_PREDECODE_CAP_CACHE
        EXPECT_TRUE(p.seed_view.storage[2].cap_saturated);
        EXPECT_TRUE(p.seed_view.storage[3].cap_saturated);
#endif
#if BB_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(p.stats.cap_requests, 5U);
        EXPECT_EQ(p.stats.cap_executions, BB_PREDECODE_CAP_CACHE ? 3U : 5U);
        EXPECT_EQ(p.stats.cap_repeat_finite_requests, 2U);
        EXPECT_EQ(p.stats.cap_radius_increases, 0U);
        EXPECT_EQ(p.stats.cap_unchanged, 5U);
#endif
        // A new shot creates fresh seed records, including their cache flags.
        // This singleton still has the zero-weight constraint.
        p.prepare(decoder.flooder.graph, {2}, {});
        p.run();
        ASSERT_EQ(p.seed_view.storage.size(), 1U);
        EXPECT_EQ(p.seed_view.storage[0].radius, 0U);
#if BB_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(p.stats.cap_requests, 1U);
        EXPECT_EQ(p.stats.cap_executions, 1U);
        EXPECT_EQ(p.stats.cap_repeat_finite_requests, 0U);
#endif
        verify_case(decoder, edges, 15);
    }
}

TEST(OneHopPredecoder, MaximumCapSentinelCanBecomeFiniteWithinShot) {
    auto decoder = decoder_for(2, {{0, 1, 2}, {1, -1, 2}});
    auto& graph = decoder.flooder.graph;
    auto& a = graph.node(0);
    auto& b = graph.node(1);
    // Inject a low-level uint32 edge beyond UserGraph's ordinary quantization
    // domain. Only predecode is exercised; the formal time lattice is separate.
    const auto ab = a.index_of_neighbor(&b);
    const auto ba = b.index_of_neighbor(&a);
    a.grid_component->weights[a.grid_cell][ab] = UINT32_MAX;
    b.grid_component->weights[b.grid_cell][ba] = UINT32_MAX;
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto& p = decoder.predecoder;
        p.prepare(graph, {0, 1}, {});
        p.run();
        ASSERT_EQ(p.seed_view.storage.size(), 2U);
        EXPECT_FALSE(p.complete);
        // a's first cap is MAX. After b reaches its boundary cap of 2, a
        // must scan again: floor_even(MAX-2) is MAX-3.
        EXPECT_EQ(p.seed_view.storage[0].radius, UINT32_MAX - 3U);
        EXPECT_EQ(p.seed_view.storage[1].radius, 2U);
        EXPECT_EQ(p.seed_view.storage[0].mate, PredecodeSeed::UNMATCHED);
        EXPECT_EQ(p.seed_view.storage[1].mate, PredecodeSeed::BOUNDARY);
        EXPECT_LE(uint64_t(p.seed_view.storage[0].radius) + p.seed_view.storage[1].radius, uint64_t(UINT32_MAX));
#if BB_PREDECODE_CAP_CACHE
        EXPECT_TRUE(p.seed_view.storage[0].cap_saturated);
        EXPECT_TRUE(p.seed_view.storage[1].cap_saturated);
#endif
#if BB_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(p.stats.cap_requests, 4U);
        EXPECT_EQ(p.stats.cap_executions, BB_PREDECODE_CAP_CACHE ? 3U : 4U);
        EXPECT_EQ(p.stats.cap_finite_executions, BB_PREDECODE_CAP_CACHE ? 2U : 3U);
        EXPECT_EQ(p.stats.cap_radius_increases, 2U);
        EXPECT_EQ(p.stats.cap_unchanged, 2U);
#endif
    }
}

TEST(OneHopPredecoder, IsolatedInfiniteCapRemainsUncached) {
    auto decoder = decoder_for(1, {});
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto& p = decoder.predecoder;
        p.prepare(decoder.flooder.graph, {0}, {});
        p.run();
        ASSERT_EQ(p.seed_view.storage.size(), 1U);
        EXPECT_FALSE(p.complete);
        EXPECT_EQ(p.seed_view.storage[0].radius, 0U);
#if BB_PREDECODE_CAP_CACHE
        EXPECT_FALSE(p.seed_view.storage[0].cap_saturated);
#endif
#if BB_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(p.stats.cap_executions, 1U);
        EXPECT_EQ(p.stats.cap_finite_executions, 0U);
#endif
    }
}

TEST(OneHopPredecoder, CachedSeedIsFreshAfterActiveSetAndNegativeNormalizationChange) {
    const std::vector<Edge> edges{{0, 1, 2}, {0, 2, 2}, {1, 3, 2}, {2, 3, 10}};
    auto decoder = decoder_for(4, edges);
    auto& p = decoder.predecoder;
    for (int repeat = 0; repeat < 3; ++repeat) {
        p.prepare(decoder.flooder.graph, {0, 1, 2, 3}, {});
        p.run();
        ASSERT_EQ(p.seed_view.storage.size(), 4U);
        EXPECT_EQ(p.seed_view.storage[2].radius, 0U);
        p.prepare(decoder.flooder.graph, {2}, {});
        p.run();
        ASSERT_EQ(p.seed_view.storage.size(), 1U);
        EXPECT_EQ(p.seed_view.storage[0].detector_id, 2U);
        EXPECT_EQ(p.seed_view.storage[0].radius, 2U);
        // Cancellation compacts the array and insertion creates another seed;
        // both retained and inserted seeds must start this shot uncached.
        p.prepare(decoder.flooder.graph, {0, 2}, {0, 3});
        p.run();
        ASSERT_EQ(p.seed_view.storage.size(), 2U);
        EXPECT_EQ(p.seed_view.storage[0].detector_id, 2U);
        EXPECT_EQ(p.seed_view.storage[1].detector_id, 3U);
        EXPECT_EQ(p.seed_view.storage[0].radius, 2U);
        EXPECT_EQ(p.seed_view.storage[1].radius, 2U);
#if BB_STRUCTURED_PROFILE_COUNTERS
        EXPECT_EQ(p.stats.cap_requests, 4U);
        EXPECT_EQ(p.stats.cap_executions, BB_PREDECODE_CAP_CACHE ? 2U : 4U);
#endif
        verify_case(decoder, edges, 15);
    }
}

TEST(OneHopPredecoder, SharedRangesCanGrowAndPairTargetsOutsideTheSourceRange) {
    auto decoder = decoder_for(4, {{0, 1, 2}, {2, 3, 4}});
    auto& predecode = decoder.predecoder;
    predecode.prepare(decoder.flooder.graph, {0, 1, 2, 3}, {});
    ASSERT_EQ(decoder.flooder.graph.grid->components.size(), 2);
    predecode.prepare_scratch();
    beyond_blossom::structured::OneHopPredecoder::PartialResult a, b;
    const auto first = predecode.prepare_order(0, 1, a);
    const auto rest = predecode.prepare_order(1, 4, b);
    predecode.run_order(first, a);
    EXPECT_EQ(predecode.seed_view.storage[0].mate, 1);
    EXPECT_EQ(predecode.seed_view.storage[1].mate, 0);
    predecode.run_order(rest, b);
    predecode.merge(a);
    predecode.merge(b);
    predecode.finish();
    EXPECT_TRUE(predecode.complete);
    const auto result = beyond_blossom::structured::decode_prepared_detection_events_for_up_to_64_observables(decoder, false);
    EXPECT_EQ(result.weight, 6);
    EXPECT_EQ(decoder.flooder.queue.size(), 0);
    auto moved = std::move(decoder);
    EXPECT_EQ(beyond_blossom::structured::decode_detection_events_for_up_to_64_observables(moved, {0, 1}, false).weight, 2);
    EXPECT_THROW(moved.predecoder.prepare(moved.flooder.graph, {2, UINT64_MAX}, {}), std::invalid_argument);
    EXPECT_EQ(beyond_blossom::structured::decode_detection_events_for_up_to_64_observables(moved, {2, 3}, false).weight, 4);
}

TEST(OneHopPredecoder, BulkImportBuildsMateEndpointsBeforeTheirPartnerObjects) {
    const std::vector<Edge> edges{{0, 1, 2}, {0, 2, 2}, {1, 3, 2}, {2, 3, 10}};
    auto decoder = decoder_for(4, edges);
    auto& graph = decoder.flooder.graph;
    const uint8_t slot = graph.node(0).index_of_neighbor(&graph.node(1));
    const std::vector<PredecodeSeed> seeds{
        {0, uint32_t(graph.node(0).grid_cell), 1, 2, slot},
        {1, uint32_t(graph.node(1).grid_cell), 0, 0, uint8_t(slot ^ 1)},
        {2, uint32_t(graph.node(2).grid_cell), PredecodeSeed::UNMATCHED, 0, 0},
        {3, uint32_t(graph.node(3).grid_cell), PredecodeSeed::UNMATCHED, 0, 0}};
    const auto first = decoder.prepare_predecode_import(seeds);
    decoder.import_predecode_range(seeds, first, 1, 2);
    EXPECT_EQ(graph.node(0).region_that_arrived, beyond_blossom::structured::NO_REGION);
    EXPECT_EQ(graph.node(1).owner_region()->match.region, first);
    EXPECT_EQ(graph.node(1).owner_region()->match.edge.loc_to, &graph.node(0));
    EXPECT_TRUE(decoder.flooder.queue.empty());
    EXPECT_TRUE(decoder.node_arena.allocated.empty());
    decoder.import_predecode_range(seeds, first, 3, 4);
    decoder.import_predecode_range(seeds, first, 0, 1);
    decoder.import_predecode_range(seeds, first, 2, 3);
    EXPECT_TRUE(decoder.flooder.queue.empty());
    decoder.finish_predecode_import(seeds, first);
    run_imported_to_completion(decoder);
    beyond_blossom::MatchingResult actual;
    for (size_t id : {0, 1, 2, 3}) {
        auto& node = graph.node(id);
        if (node.region_that_arrived)
            actual += decoder.shatter_blossom_and_extract_matches(node.top_region());
    }
    EXPECT_EQ(actual.weight, 4);
    EXPECT_EQ(actual.obs_mask, (uint64_t{1} << 1) | (uint64_t{1} << 2));
    EXPECT_TRUE(decoder.flooder.regions->empty());
}

TEST(OneHopPredecoder, BulkImportResetRecoversConstructedAndReservedSlots) {
    const std::vector<Edge> edges{{0, 1, 2}, {0, -1, 2}, {1, -1, 10}};
    auto decoder = decoder_for(2, edges);
    auto& graph = decoder.flooder.graph;
    const uint8_t slot = graph.node(0).index_of_neighbor(nullptr);
    const std::vector<PredecodeSeed> seeds{
        {0, uint32_t(graph.node(0).grid_cell), PredecodeSeed::BOUNDARY, 2, slot},
        {1, uint32_t(graph.node(1).grid_cell), PredecodeSeed::UNMATCHED, 0, 0}};
    const auto first = decoder.prepare_predecode_import(seeds);
    decoder.import_predecode_range(seeds, first, 0, 1);
    EXPECT_EQ(graph.node(0).owner_region()->match.region, beyond_blossom::structured::NO_REGION);
    EXPECT_EQ(graph.node(0).owner_region()->match.edge.loc_to, nullptr);
    EXPECT_EQ(graph.node(1).region_that_arrived, beyond_blossom::structured::NO_REGION);
    const auto generation = decoder.flooder.regions->generation(first);
    // The coordinator's recovery after a worker failure: all workers have joined,
    // one reservation was constructed, and another has no object lifetime yet.
    decoder.reset();
    EXPECT_TRUE(decoder.flooder.regions->empty());
    EXPECT_EQ(graph.node(0).region_that_arrived, beyond_blossom::structured::NO_REGION);
    EXPECT_FALSE(decoder.flooder.regions->is_live(first, generation));
    decoder.import_predecode_state(seeds);
    EXPECT_GT(decoder.flooder.regions->generation(first), generation);
    run_imported_to_completion(decoder);
    beyond_blossom::MatchingResult actual;
    for (size_t id : {0, 1}) {
        auto& node = graph.node(id);
        if (node.region_that_arrived)
            actual += decoder.shatter_blossom_and_extract_matches(node.top_region());
    }
    EXPECT_EQ(actual.weight, 2);
    EXPECT_EQ(actual.obs_mask, 1U);
    decoder.import_predecode_state({});
    EXPECT_TRUE(decoder.flooder.queue.empty());
    EXPECT_TRUE(decoder.flooder.regions->empty());
}


TEST(OneHopPredecoder, PartitionScratchSeparatesCacheLinesAndPreservesStableGlobalOrder) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    Predecoder::Grid grid;
    grid.predecode.active.resize(258);
    grid.predecode.radii.resize(258);
    grid.boundary_mask.resize(258);
    grid.predecode.neighborhood = [](const Predecoder::Grid&, size_t cell, uint16_t) {
        const uint32_t degree = (cell * 7 + 3) % 13;
        return (degree << 16) | ((uint32_t{1} << degree) - 1);
    };
    Predecoder padded, compact;
    std::vector<PredecodeSeed> seeds;
    for (size_t n : {0, 1, 31, 59, 64, 65, 127, 3, 257, 0}) {
        seeds.resize(n);
        for (size_t k = 0; k < n; ++k) {
            seeds[k] = {uint32_t(k), uint32_t(k)};
            seeds[k].grid = &grid;
        }
        padded.bind_seeds(seeds, false);
        compact.bind_seeds(seeds, false);
        // P4 plus conflict, including two empty interior ranges.
        const std::array<size_t, 6> offsets{0, n / 4, n / 4, n / 2, n / 2, n};
        padded.prepare_partition_scratch(offsets);
        compact.prepare_scratch();
        const auto layout = padded.scratch_layout();
        ASSERT_EQ(layout.size(), 5);
        std::vector<std::pair<uintptr_t, uintptr_t>> lines;
        for (size_t b = 0; b < layout.size(); ++b) {
            const auto& range = layout[b];
            ASSERT_EQ(range.seed_begin, offsets[b]);
            ASSERT_EQ(range.seed_end, offsets[b + 1]);
            const auto size = range.seed_end - range.seed_begin;
            for (const auto [address, element_size] : {
                     std::pair{range.mask_address, sizeof(uint16_t)},
                     std::pair{range.degree_address, sizeof(uint8_t)},
                     std::pair{range.order_address, sizeof(uint32_t)}}) {
                EXPECT_EQ(address % 64, 0);
                if (size)
                    lines.emplace_back(address / 64, (address + size * element_size - 1) / 64);
            }
            Predecoder::PartialResult actual_stats, reference_stats;
            const auto actual = padded.prepare_partition_order(b, actual_stats);
            const auto reference = compact.prepare_order(offsets[b], offsets[b + 1], reference_stats);
            EXPECT_EQ(std::vector<uint32_t>(actual.sources.begin(), actual.sources.end()),
                      std::vector<uint32_t>(reference.sources.begin(), reference.sources.end()));
            for (uint32_t k : actual.sources) {
                ASSERT_GE(k, offsets[b]);
                ASSERT_LT(k, offsets[b + 1]);
                EXPECT_EQ(actual.masks_by_seed[k], reference.masks_by_seed[k]);
            }
        }
        std::sort(lines.begin(), lines.end());
        for (size_t k = 1; k < lines.size(); ++k)
            EXPECT_LT(lines[k - 1].second, lines[k].first);
        const auto address = layout.front().mask_address;
        Predecoder moved(std::move(padded));
        EXPECT_TRUE(padded.scratch_layout().empty());
        ASSERT_EQ(moved.scratch_layout().size(), 5);
        EXPECT_EQ(moved.scratch_layout().front().mask_address, address);
        padded = std::move(moved);
        padded.reset();
        compact.reset();
        EXPECT_TRUE(padded.scratch_layout().empty());
    }
}

TEST(OneHopPredecoder, CellClearRetainsResultsAndSurvivesMoveAndRebind) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    Predecoder::Grid grid;
    grid.predecode.active.resize(4);
    grid.predecode.radii.resize(4);
    std::vector<beyond_blossom::structured::PredecodeSeed> seeds(2);
    for (size_t i = 0; i < seeds.size(); ++i) {
        seeds[i].detector_id = uint32_t(i);
        seeds[i].cell = uint32_t(i + 1);
        seeds[i].grid = &grid;
        seeds[i].radius = 2;
        seeds[i].mate = uint32_t(1 - i);
    }
    Predecoder pre;
    pre.bind_seeds(seeds);
    grid.predecode.radii[1] = grid.predecode.radii[2] = 2;
    pre.complete = true;
    pre.result_pending = false;
    pre.result = {3, 4};
    pre.stats.prepare_ns = 17;
    pre.stats.handoff_ns = 19;
    const auto result = pre.result;
    pre.clear_cell_state();
    EXPECT_EQ(pre.seed_view.storage.data(), seeds.data());
    EXPECT_EQ(pre.seed_view.storage.size(), 2);
    EXPECT_EQ(pre.result, result);
    EXPECT_TRUE(pre.complete);
    EXPECT_FALSE(pre.result_pending);
    EXPECT_EQ(pre.stats.prepare_ns, 17);
    EXPECT_EQ(pre.stats.handoff_ns, 19);
    for (const auto& seed : pre.seed_view.storage) {
        EXPECT_EQ(seed.radius, 2);
        EXPECT_EQ(seed.mate, 1 - seed.detector_id);
        EXPECT_EQ(grid.predecode.active[seed.cell], 0);
        EXPECT_EQ(grid.predecode.radii[seed.cell], 0);
    }
    Predecoder moved(std::move(pre));
    pre.reset();
    EXPECT_EQ(moved.seed_view.storage.data(), seeds.data());
    EXPECT_EQ(moved.result, result);
    moved.rebuild_cell_index();
    grid.predecode.radii[2] = 5;
    moved.reset();
    EXPECT_TRUE(moved.seed_view.storage.empty());
    EXPECT_EQ(grid.predecode.active[1], 0);
    EXPECT_EQ(grid.predecode.active[2], 0);
    EXPECT_EQ(grid.predecode.radii[2], 0);
    moved.bind_seeds(seeds, false);
    grid.predecode.radii[1] = 7;
    moved.reset();
    EXPECT_EQ(grid.predecode.radii[1], 0);
}
TEST(OneHopPredecoder, RetiredViewDoesNotClearNewOwnerCells) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    Predecoder::Grid grid;
    grid.predecode.active.resize(1);
    grid.predecode.radii.resize(1);
    std::vector<beyond_blossom::structured::PredecodeSeed> seeds(1);
    seeds.front().grid = &grid;
    Predecoder retired, current;
    retired.bind_seeds(seeds);
    retired.clear_cell_state();
    current.bind_seeds(seeds);
    grid.predecode.radii[0] = 9;
    // A second clear and the following reset must not traverse a retired view.
    retired.clear_cell_state();
    retired.reset();
    EXPECT_EQ(grid.predecode.active[0], 1);
    EXPECT_EQ(grid.predecode.radii[0], 9);
    current.reset();
    EXPECT_EQ(grid.predecode.active[0], 0);
    EXPECT_EQ(grid.predecode.radii[0], 0);
}

TEST(OneHopPredecoder, LiveExternalCellOwnershipTransfersOnMoveAndAssignment) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    Predecoder::Grid source_grid, old_target_grid;
    source_grid.predecode.active.resize(1);
    source_grid.predecode.radii.resize(1);
    old_target_grid.predecode.active.resize(1);
    old_target_grid.predecode.radii.resize(1);
    std::vector<beyond_blossom::structured::PredecodeSeed> source_seeds(1), old_target_seeds(1);
    source_seeds.front().grid = &source_grid;
    old_target_seeds.front().grid = &old_target_grid;
    Predecoder source, target;
    source.bind_seeds(source_seeds);
    source_grid.predecode.radii[0] = 7;
    source.result = {3, 4};
    source.complete = true;
    source.result_pending = true;
    source.stats.prepare_ns = 23;
    target.bind_seeds(old_target_seeds);
    old_target_grid.predecode.radii[0] = 11;
    Predecoder moved(std::move(source));
    EXPECT_TRUE(source.seed_view.storage.empty());
    source.reset();
    EXPECT_EQ(source_grid.predecode.active[0], 1);
    EXPECT_EQ(source_grid.predecode.radii[0], 7);
    target = std::move(moved);
    EXPECT_TRUE(moved.seed_view.storage.empty());
    moved.reset();
    EXPECT_EQ(old_target_grid.predecode.active[0], 0);
    EXPECT_EQ(old_target_grid.predecode.radii[0], 0);
    EXPECT_EQ(source_grid.predecode.active[0], 1);
    EXPECT_EQ(source_grid.predecode.radii[0], 7);
    EXPECT_EQ(target.seed_view.storage.data(), source_seeds.data());
    EXPECT_EQ(target.result.obs_mask, 3);
    EXPECT_EQ(target.result.weight, 4);
    EXPECT_TRUE(target.complete);
    EXPECT_TRUE(target.result_pending);
    EXPECT_EQ(target.stats.prepare_ns, 23);
    target = std::move(target);
    EXPECT_EQ(source_grid.predecode.radii[0], 7);
    target.reset();
    EXPECT_EQ(source_grid.predecode.active[0], 0);
    EXPECT_EQ(source_grid.predecode.radii[0], 0);
}

TEST(OneHopPredecoder, LiveOwnedSeedAllocationTransfersWithoutMovedFromCleanup) {
    using Predecoder = beyond_blossom::structured::OneHopPredecoder;
    auto decoder = decoder_for(2, {{0, 1, 2}});
    auto& source = decoder.predecoder;
    source.prepare(decoder.flooder.graph, {0, 1}, {});
    ASSERT_EQ(source.seed_view.storage.size(), 2);
    ASSERT_GE(source.owned_seed_capacity(), 2);
    const auto* allocation = source.seed_view.storage.data();
    auto* grid = source.seed_view.storage.front().grid;
    const auto first = source.seed_view.storage.front().cell;
    const auto second = source.seed_view.storage.back().cell;
    grid->predecode.radii[first] = 5;
    Predecoder moved(std::move(source));
    EXPECT_TRUE(source.seed_view.storage.empty());
    source.reset();
    EXPECT_EQ(moved.seed_view.storage.data(), allocation);
    EXPECT_EQ(grid->predecode.active[first], 1);
    EXPECT_EQ(grid->predecode.active[second], 2);
    Predecoder target;
    target = std::move(moved);
    moved.reset();
    EXPECT_EQ(target.seed_view.storage.data(), allocation);
    EXPECT_EQ(grid->predecode.radii[first], 5);
    EXPECT_GE(target.owned_seed_capacity(), 2);
    target.reset();
    EXPECT_EQ(grid->predecode.active[first], 0);
    EXPECT_EQ(grid->predecode.active[second], 0);
    EXPECT_EQ(grid->predecode.radii[first], 0);
    // The moved-from member can own a fresh shot on the same graph again.
    source.prepare(decoder.flooder.graph, {1}, {});
    EXPECT_EQ(source.seed_view.storage.size(), 1);
    source.reset();
    EXPECT_EQ(grid->predecode.active[second], 0);
}

TEST(OneHopPredecoder, FixedSeedViewRankSlicesSkipEmptyCapacityAndHoles) {
    using beyond_blossom::structured::SeedView;
    std::vector<beyond_blossom::structured::PredecodeSeed> storage(7);
    for (size_t parts : {2, 4}) {
        const std::array<size_t, 6> two{0, 0, 4, 7, 0, 0}, four{0, 0, 2, 2, 4, 7};
        const auto& bases = parts == 2 ? two : four;
        for (size_t occupancy = 0; occupancy < 256; ++occupancy) {
            SeedView view;
            view.bind_fixed(storage, std::span(bases).first(parts + 2));
            std::array<size_t, 5> counts{};
            size_t value = occupancy, expected = 0;
            for (size_t b = 0; b <= parts; ++b) {
                const auto capacity = bases[b + 1] - bases[b];
                counts[b] = value % (capacity + 1); value /= capacity + 1;
                expected += counts[b];
            }
            view.set_counts(std::span(counts).first(parts + 1));
            EXPECT_EQ(view.active_count(), expected);
            std::vector<size_t> slots;
            for (const auto& r : view.active_ranges()) for (size_t slot = r.begin; slot < r.end; ++slot) {
                EXPECT_EQ(view.rank_of(slot), slots.size());
                if (parts == 2) EXPECT_EQ(view.rank_of<2>(slot), slots.size());
                else EXPECT_EQ(view.rank_of<4>(slot), slots.size());
                slots.push_back(slot);
            }
            for (size_t begin = 0; begin <= expected; ++begin) for (size_t end = begin; end <= expected; ++end) {
                std::vector<size_t> projected;
                view.for_rank_range(begin, end, [&](size_t lo, size_t hi, size_t rank) {
                    EXPECT_EQ(rank, begin + projected.size());
                    for (size_t slot = lo; slot < hi; ++slot) projected.push_back(slot);
                });
                EXPECT_EQ(projected, std::vector<size_t>(slots.begin() + begin, slots.begin() + end));
            }
            for (size_t slot = 0; slot < storage.size(); ++slot)
                EXPECT_EQ(view.contains(slot), std::find(slots.begin(), slots.end(), slot) != slots.end());
        }
    }
}

TEST(OneHopPredecoder, FixedSeedViewCompleteEdgesAndResetIgnoreHoleRecords) {
    auto decoder = decoder_for(2, {{0, 1, 2}});
    auto& graph = decoder.flooder.graph;
    std::vector<PredecodeSeed> backing(8);
    uint8_t slot = 0;
    while (!graph.node(0).has_edge(slot) || graph.node(0).neighbor(slot) != &graph.node(1)) ++slot;
    backing[0] = {0, uint32_t(graph.node(0).grid_cell), 5, 2, slot};
    backing[5] = {1, uint32_t(graph.node(1).grid_cell), 0, 0, uint8_t(slot ^ 1)};
    backing[0].grid = graph.node(0).grid_component;
    backing[5].grid = graph.node(1).grid_component;
    auto& pre = decoder.predecoder;
    const std::array<size_t, 4> bases{0, 2, 5, 8};
    const std::array<size_t, 3> counts{1, 0, 1};
    pre.seed_view.bind_fixed(backing, bases);
    pre.seed_view.set_counts(counts);
    pre.publish_cell_state();
    pre.rebuild_cell_index();
    pre.finish();
    ASSERT_TRUE(pre.complete);  // All holes still carry UNMATCHED and null grid.
    beyond_blossom::structured::decode_prepared_detection_events_to_normalized_match_edges(decoder, false, false);
    ASSERT_EQ(decoder.flooder.match_edges.size(), 1U);
    EXPECT_EQ(decoder.flooder.match_edges[0].loc_from, &graph.node(0));
    EXPECT_EQ(decoder.flooder.match_edges[0].loc_to, &graph.node(1));
    pre.reset();
    for (size_t id : {0, 1}) {
        const auto& node = graph.node(id);
        EXPECT_EQ(node.grid_component->predecode.active[node.grid_cell], 0U);
        EXPECT_EQ(node.grid_component->predecode.radii[node.grid_cell], 0U);
    }
}

#if BB_TAILRESET_PREDECODE_MODE == 3
TEST(OneHopPredecoder, SimpleEqualShareCapsAndEvenRounding) {
    for (const int weight : {0, 4, 6, 7, 8}) {
        const std::vector<Edge> edges{{0, 1, weight}};
        auto decoder = decoder_for(2, edges);
        auto& pre = decoder.predecoder;
        pre.prepare(decoder.flooder.graph, {0, 1}, {});
        pre.run();
        assert_dual_and_matching(decoder, edges);
        const auto half = (weight / 2) & ~1;
        EXPECT_EQ(pre.seed_view.storage[0].radius, half);
        EXPECT_EQ(pre.seed_view.storage[1].radius, half);
        EXPECT_EQ(pre.complete, 2 * half == weight);
        // Public graph construction quantizes weights onto the even lattice.
        // Raw odd edges above test only predecoder dual feasibility and refusal
        // to accept a non-tight match; the existing full timeline requires even
        // weights. The 2-mod-4 case must still recover its exact optimum.
        if (weight % 2 == 0) {
            verify_case(decoder, edges, 3);
            verify_case(decoder, edges, 0);
        }
    }
}

TEST(OneHopPredecoder, SimpleEqualShareRespectsInactiveAndBoundaryConstraints) {
    {
        const std::vector<Edge> edges{{0, 1, 8}, {0, 2, 2}, {1, -1, 20}, {2, -1, 20}};
        auto decoder = decoder_for(3, edges);
        auto& pre = decoder.predecoder;
        pre.prepare(decoder.flooder.graph, {0, 1}, {});
        pre.run();
        assert_dual_and_matching(decoder, edges);
        EXPECT_EQ(pre.seed_view.storage[0].radius, 2U);
        EXPECT_EQ(pre.seed_view.storage[1].radius, 4U);
        EXPECT_FALSE(pre.complete);
        EXPECT_EQ(pre.seed_view.storage[0].mate, PredecodeSeed::UNMATCHED);
        EXPECT_EQ(pre.seed_view.storage[1].mate, PredecodeSeed::UNMATCHED);
        verify_case(decoder, edges, 3);
    }
    {
        const std::vector<Edge> edges{{0, 1, 8}, {0, -1, 2}, {1, -1, 20}};
        auto decoder = decoder_for(2, edges);
        auto& pre = decoder.predecoder;
        pre.prepare(decoder.flooder.graph, {0, 1}, {});
        pre.run();
        assert_dual_and_matching(decoder, edges);
        EXPECT_EQ(pre.seed_view.storage[0].radius, 2U);
        EXPECT_EQ(pre.seed_view.storage[0].mate, PredecodeSeed::BOUNDARY);
        EXPECT_EQ(pre.seed_view.storage[1].radius, 4U);
        EXPECT_EQ(pre.seed_view.storage[1].mate, PredecodeSeed::UNMATCHED);
        verify_case(decoder, edges, 3);
    }
    auto boundary = decoder_for(1, {{0, -1, 6}});
    boundary.predecoder.prepare(boundary.flooder.graph, {0}, {});
    boundary.predecoder.run();
    EXPECT_TRUE(boundary.predecoder.complete);
    EXPECT_EQ(boundary.predecoder.seed_view.storage[0].radius, 6U);
}

TEST(OneHopPredecoder, SimpleEqualShareExhaustiveCorrectionOracle) {
    // Competing local pairs, unequal caps, boundaries, 2-mod-4 weights, empty
    // shots, and multiple observable bits on the public even-weight lattice.
    // The oracle enumerates original edge subsets independently of the decoder.
    for (const int weight : {0, 2, 4, 6, 12}) {
        const std::vector<Edge> edges{{0, 1, weight}, {0, 2, 4}, {1, 3, 8}, {2, 3, 6},
                                      {0, -1, 2}, {3, -1, 10}};
        auto decoder = decoder_for(4, edges);
        for (size_t bits = 0; bits < 16; ++bits) verify_case(decoder, edges, bits);
        for (size_t bits = 16; bits-- > 0;) verify_case(decoder, edges, bits);
    }
}
#endif
