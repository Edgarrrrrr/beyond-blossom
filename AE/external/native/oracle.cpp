// Independent integer-graph oracle, built only against the pinned official source.
// Graph conversion, both decoder calls and all validation are outside timing.
#include "pymatching/sparse_blossom/driver/mwpm_decoding.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Json = nlohmann::json;

namespace {
struct Edge {
    int64_t u;
    int64_t v;
    double weight;
    uint64_t observable;
    int64_t integer_weight;
};

uint64_t unsigned_integer(const Json& value, const char* name) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    if (value.is_number_unsigned())
        return value.get<uint64_t>();
    const auto signed_value = value.get<int64_t>();
    if (signed_value < 0)
        throw std::invalid_argument(std::string(name) + " must be nonnegative");
    return static_cast<uint64_t>(signed_value);
}

int64_t signed_integer(const Json& value, const char* name) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    if (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX))
        throw std::invalid_argument(std::string(name) + " exceeds signed 64-bit range");
    return value.get<int64_t>();
}

std::pair<int64_t, int64_t> canonical(int64_t u, int64_t v) {
    if (u == -1 || (v >= 0 && u > v))
        std::swap(u, v);
    return {u, v};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::invalid_argument("usage: oracle INPUT.json OUTPUT.jsonl");
        if (std::filesystem::exists(argv[2]))
            throw std::invalid_argument("Oracle output already exists; use a new output path");
        std::ifstream input(argv[1]);
        if (!input)
            throw std::runtime_error("Cannot open native input");
        Json payload;
        input >> payload;
        input >> std::ws;
        if (!input.eof())
            throw std::invalid_argument("Trailing data after native JSON input");

        const auto detector_count = unsigned_integer(payload.at("num_detectors"), "num_detectors");
        const auto observable_count = unsigned_integer(payload.at("num_observables"), "num_observables");
        if (detector_count == 0 || detector_count > static_cast<uint64_t>(INT64_MAX) ||
            detector_count > std::numeric_limits<size_t>::max() || observable_count > 64)
            throw std::invalid_argument("Invalid detector or observable count");
        const size_t m = static_cast<size_t>(detector_count);
        const auto& raw_edges = payload.at("edges");
        const auto& samples = payload.at("samples");
        if (!raw_edges.is_array() || raw_edges.empty() || !samples.is_array() || samples.empty())
            throw std::invalid_argument("Edges and samples must be nonempty arrays");

        pm::MatchingGraph graph(m, static_cast<size_t>(observable_count), 1.0);
        pm::SearchGraph search(m);
        std::vector<Edge> edges;
        std::map<std::pair<int64_t, int64_t>, size_t> edge_indices;
        for (const auto& raw : raw_edges) {
            if (!raw.is_array() || raw.size() != 5 || !raw[2].is_number())
                throw std::invalid_argument("Each edge needs u, v, float weight, observable mask, integer weight");
            const int64_t u = signed_integer(raw[0], "edge u");
            const int64_t v = signed_integer(raw[1], "edge v");
            const double weight = raw[2].get<double>();
            const uint64_t mask = unsigned_integer(raw[3], "observable mask");
            const int64_t q = signed_integer(raw[4], "integer edge weight");
            if (u < 0 || static_cast<uint64_t>(u) >= detector_count || v < -1 ||
                (v >= 0 && static_cast<uint64_t>(v) >= detector_count) || u == v)
                throw std::invalid_argument("Invalid edge endpoint or self loop");
            if (!std::isfinite(weight) || weight <= 0 || q <= 0 || q % 2 != 0 ||
                q > std::numeric_limits<pm::signed_weight_int>::max())
                throw std::invalid_argument("Edge weights must be finite positive; integer weights must be even and in range");
            if (observable_count < 64 && (mask >> observable_count) != 0)
                throw std::invalid_argument("Observable mask exceeds num_observables");
            if (!edge_indices.emplace(canonical(u, v), edges.size()).second)
                throw std::invalid_argument("Duplicate edge endpoints");
            edges.push_back({u, v, weight, mask, q});
            std::vector<size_t> observables;
            for (size_t bit = 0; bit < observable_count; ++bit)
                if ((mask >> bit) & 1)
                    observables.push_back(bit);
            // These q values are already the shared external graph's even weights.
            // Construct the internal graph directly; do not renormalize through a DEM.
            const auto integer_weight = static_cast<pm::signed_weight_int>(q);
            if (v < 0) {
                graph.add_boundary_edge(static_cast<size_t>(u), integer_weight, observables);
                search.add_boundary_edge(static_cast<size_t>(u), integer_weight, observables);
            } else {
                graph.add_edge(static_cast<size_t>(u), static_cast<size_t>(v), integer_weight, observables);
                search.add_edge(static_cast<size_t>(u), static_cast<size_t>(v), integer_weight, observables);
            }
        }
        pm::Mwpm decoder{pm::GraphFlooder(std::move(graph)), pm::SearchFlooder(std::move(search))};
        std::ofstream output(argv[2]);
        if (!output)
            throw std::runtime_error("Cannot create oracle output");
        for (size_t shot = 0; shot < samples.size(); ++shot) {
            const auto& raw_sample = samples[shot];
            if (!raw_sample.is_array())
                throw std::invalid_argument("Each sample must be an array of detector IDs");
            std::vector<uint64_t> syndrome;
            syndrome.reserve(raw_sample.size());
            for (const auto& raw_detector : raw_sample) {
                const auto detector = unsigned_integer(raw_detector, "detector ID");
                if (detector >= detector_count || (!syndrome.empty() && detector <= syndrome.back()))
                    throw std::invalid_argument("Sample detector IDs must be bounded, sorted and unique");
                syndrome.push_back(detector);
            }

            std::vector<int64_t> endpoints;
            pm::decode_detection_events_to_edges(decoder, syndrome, endpoints);
            if (endpoints.size() % 2 != 0)
                throw std::runtime_error("Official decoder returned an incomplete edge");
            std::vector<size_t> correction;
            std::vector<bool> seen(edges.size(), false);
            std::vector<uint8_t> parity(m, 0);
            uint64_t observable = 0;
            int64_t integer_weight = 0;
            long double float_weight = 0;
            for (size_t pair = 0; pair < endpoints.size(); pair += 2) {
                const auto found = edge_indices.find(canonical(endpoints[pair], endpoints[pair + 1]));
                if (found == edge_indices.end() || seen[found->second])
                    throw std::runtime_error("Official decoder returned an unknown or repeated edge");
                const size_t index = found->second;
                seen[index] = true;
                const auto& edge = edges[index];
                correction.push_back(index);
                parity[static_cast<size_t>(edge.u)] ^= 1;
                if (edge.v >= 0)
                    parity[static_cast<size_t>(edge.v)] ^= 1;
                observable ^= edge.observable;
                if (integer_weight > INT64_MAX - edge.integer_weight)
                    throw std::overflow_error("Correction integer weight overflow");
                integer_weight += edge.integer_weight;
                float_weight += edge.weight;
            }
            for (auto detector : syndrome)
                parity[static_cast<size_t>(detector)] ^= 1;
            if (std::any_of(parity.begin(), parity.end(), [](uint8_t value) { return value != 0; }))
                throw std::runtime_error("Official correction fails Hc = syndrome");
            const auto matching_result = pm::decode_detection_events_for_up_to_64_observables(decoder, syndrome, false);
            if (matching_result.weight != integer_weight)
                throw std::runtime_error("Official matching weight and reconstructed correction weight differ");
            const double recorded_weight = static_cast<double>(float_weight);
            if (!std::isfinite(recorded_weight))
                throw std::overflow_error("Correction float weight overflow");
            std::sort(correction.begin(), correction.end());
            output << Json({{"shot", shot}, {"integer_weight", integer_weight},
                            {"weight", recorded_weight}, {"observable", observable},
                            {"edge_ids", correction}}).dump() << '\n';
        }
        output.close();
        if (!output)
            throw std::runtime_error("Oracle output write failed");
        std::cout << Json({{"completed", true}, {"shots", samples.size()},
                          {"quantization", "input edges[4], no additional normalization"}}).dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
