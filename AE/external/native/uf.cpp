#ifdef BENCH_LDPC
#include "uf_decode_core.hpp"
#else
#include "Decoder.hpp"
#include "boundary_lattice.hpp"
#endif
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Edge { int u, v; double w; uint64_t obs; int64_t q; };
static uint64_t ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();
}
int main(int argc, char** argv) { try {
    if (argc != 3) throw std::runtime_error("usage: uf INPUT.json OUTPUT_PREFIX");
    std::ifstream input(argv[1]); Json j; input >> j;
    const size_t m=j.at("num_detectors"), repeats=j.at("repeats");
    const auto samples=j.at("samples").get<std::vector<std::vector<int>>>();
    const auto warm=j.at("warmup").get<std::vector<std::vector<int>>>();
    const double warm_seconds=j.at("warmup_seconds");
    std::vector<Edge> edges;
    std::vector<std::pair<int,int>> endpoints;
    for (auto e:j.at("edges")) {
        edges.push_back({e[0],e[1],e[2],e[3],e[4]});
        endpoints.emplace_back(e[0],e[1]);
    }
#ifdef BENCH_LDPC
    ldpc::bp::BpSparse pcm(m,edges.size());
    for (size_t i=0;i<edges.size();++i) {
        pcm.insert_entry(edges[i].u,i);
        if(edges[i].v>=0) pcm.insert_entry(edges[i].v,i);
    }
    ldpc::uf::PreparedUfDecoder decoder(pcm);
    std::vector<uint8_t> syndrome(m,0);
#else
    UnionFindCPP::Decoder<BoundaryLattice> decoder(m,endpoints);
    std::vector<uint32_t> syndrome(decoder.num_vertices(),0);
#endif
    std::vector<size_t> correction;
    auto decode = [&](const std::vector<int>& defects) {
        std::fill(syndrome.begin(),syndrome.end(),0);
        for(auto v:defects) syndrome.at(v)=1;
        correction.clear();
#ifdef BENCH_LDPC
        decoder.prepare_shot(syndrome);
        auto t0=Clock::now();
        const auto& result=decoder.decode_prepared();
        auto t1=Clock::now();
        for(size_t e=0;e<result.size();++e) if(result[e]) correction.push_back(e);
        decoder.finish_shot();
#else
        auto t0=Clock::now();
        auto result=decoder.decode(syndrome);
        auto t1=Clock::now();
        for(auto edge:result) correction.push_back(decoder.edge_idx(edge));
        decoder.clear();
#endif
        return ns(t0,t1);
    };
    auto check = [&](const std::vector<int>& defects) {
        std::vector<uint8_t> parity(m,0);
        double weight=0; int64_t integer_weight=0; uint64_t observable=0;
        for(auto i:correction) {
            const auto& e=edges.at(i);
            parity[e.u]^=1; if(e.v>=0) parity[e.v]^=1;
            weight+=e.w; integer_weight+=e.q; observable^=e.obs;
        }
        for(auto v:defects) parity.at(v)^=1;
        for(auto p:parity) if(p) throw std::runtime_error("Hc != syndrome");
        return std::tuple<double,int64_t,uint64_t>(weight,integer_weight,observable);
    };
    if(warm.empty()||samples.empty()||repeats==0) throw std::runtime_error("Empty workload");
    auto start=Clock::now(); size_t warmed=0;
    do { for(const auto& s:warm) { decode(s); check(s); ++warmed; } }
    while(std::chrono::duration<double>(Clock::now()-start).count()<warm_seconds);
    const double warm_elapsed=std::chrono::duration<double>(Clock::now()-start).count();
    std::ofstream output(std::string(argv[2])+".csv");
    std::ofstream chains(std::string(argv[2])+".corrections.jsonl");
    output << "repeat,shot,decode_ns,weight,integer_weight,observable\n" << std::setprecision(17);
    // All formatting, correction extraction, weight calculations, parity checks
    // and writes take place after the native function's timer has stopped.
    for(size_t r=0;r<repeats;++r) for(size_t i=0;i<samples.size();++i) {
        auto elapsed=decode(samples[i]);
        auto [weight,q,obs]=check(samples[i]);
        output<<r<<','<<i<<','<<elapsed<<','<<weight<<','<<q<<','<<obs<<'\n';
        if(r==0) chains << Json(correction).dump() << '\n';
    }
    output.close(); chains.close();
    if(!output||!chains) throw std::runtime_error("Output write failed");
    std::cout << Json({{"completed",true},{"warmup_calls",warmed},
                      {"warmup_wall_seconds",warm_elapsed}}).dump() << '\n';
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
