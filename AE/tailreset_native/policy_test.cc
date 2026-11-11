#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include "pymatching/sparse_blossom/structured/driver/mwpm_decoding.h"

namespace {
struct Edge { size_t u; int v, weight; };
const std::array<Edge, 8> edges{{{0,1,4},{0,2,6},{1,3,6},{2,3,4},
                              {0,-1,8},{1,-1,8},{2,-1,8},{3,-1,8}}};
auto make_decoder() {
    pm::structured::MatchingGraph graph(4, edges.size());
    for (size_t k = 0; k < edges.size(); ++k) {
        const auto e = edges[k];
        if (e.v < 0) graph.add_boundary_edge(e.u, e.weight, {k});
        else graph.add_edge(e.u, size_t(e.v), e.weight, {k});
    }
    pm::StaticGridSpec spec{3,3,false,{{0,0,0},{2,0,0},{0,2,0},{2,2,0}}, {}, pm::GridSimd::scalar};
    graph.configure_grid(spec);
    pm::structured::GraphFlooder flooder(std::move(graph));
    flooder.sync_negative_weight_observables_and_detection_events();
    return pm::structured::Mwpm(std::move(flooder));
}
void need(bool value) { if (!value) throw std::runtime_error("AE predecode policy regression"); }
}
int main() {
    try {
        auto decoder = make_decoder();
        for (size_t bits = 0; bits < 16; ++bits) {
            int optimum = std::numeric_limits<int>::max();
            std::set<uint64_t> observables;
            for (size_t correction = 0; correction < 256; ++correction) {
                size_t parity = 0; int weight = 0;
                for (size_t k = 0; k < edges.size(); ++k) if (correction & (1U << k)) {
                    parity ^= 1U << edges[k].u;
                    if (edges[k].v >= 0) parity ^= 1U << edges[k].v;
                    weight += edges[k].weight;
                }
                if (parity != bits || weight > optimum) continue;
                if (weight < optimum) { optimum = weight; observables.clear(); }
                observables.insert(correction);
            }
            std::vector<uint64_t> events;
            for (size_t k = 0; k < 4; ++k) if (bits & (1U << k)) events.push_back(k);
            for (size_t pass = 0; pass < 3; ++pass) {
                std::reverse(events.begin(), events.end());
                auto prefix = make_decoder();
                auto& pre = prefix.predecoder;
                pre.prepare(prefix.flooder.graph, events, prefix.flooder.negative_weight_detection_events);
#if PM_TAILRESET_PREDECODE_MODE == 2
                pre.prepare_scratch();
                pm::structured::OneHopPredecoder::PartialResult partial;
                const auto order = pre.prepare_order(0, pre.seed_view.storage.size(), partial);
                need(std::is_sorted(order.sources.begin(), order.sources.end(), [&](auto a, auto b) {
                    return pre.seed_view.storage[a].cell < pre.seed_view.storage[b].cell;
                }));
#endif
                pre.run(prefix.single_predecode_grid());
                size_t residual = 0, pairs = 0, boundaries = 0;
                for (size_t k = 0; k < pre.seed_view.storage.size(); ++k) {
                    const auto& seed = pre.seed_view.storage[k];
                    if (seed.mate == pm::structured::PredecodeSeed::UNMATCHED) ++residual;
                    else if (seed.mate == pm::structured::PredecodeSeed::BOUNDARY) ++boundaries;
                    else if (k < seed.mate) ++pairs;
#if PM_TAILRESET_PREDECODE_MODE == 1
                    need(seed.radius == 0 && seed.mate == pm::structured::PredecodeSeed::UNMATCHED);
#endif
                }
                need(events.size() == residual + 2*pairs + boundaries);
                const auto value = pm::structured::decode_detection_events_for_up_to_64_observables(decoder, events, false);
                need(value.weight == optimum && observables.contains(value.obs_mask));
            }
        }
        std::cout << "48 exhaustive syndrome/order/reuse cases passed; mode=" << PM_TAILRESET_PREDECODE_MODE << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
