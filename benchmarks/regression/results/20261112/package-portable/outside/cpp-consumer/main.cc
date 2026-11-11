
#include <beyond_blossom/structured/decoder.h>
#include <stim/dem/detector_error_model.h>
#include <array>
#include <iostream>
#include <vector>
int main() {
    stim::DetectorErrorModel model(R"(
        error(0.11) D0 D1 L0
        error(0.03) D0
        error(0.02) D1
        detector(0, 0, 0) D0
        detector(2, 0, 0) D1
    )");
    beyond_blossom::structured::Options options;
    options.distance = 3; options.rounds = 3; options.rotated = false;
    options.simd = beyond_blossom::GridSimd::scalar;
    options.include_search_graph = true;
    beyond_blossom::structured::Decoder decoder(model, options);
    const std::array<std::vector<uint64_t>, 4> shots{{{}, {0}, {1}, {0, 1}}};
    const auto results = decoder.decode_batch(shots);
    if (results.size() != 4 || results[0].obs_mask != 0 || results[0].weight != 0 || results[3].obs_mask != 1)
        return 1;
    const auto again = decoder.decode(shots[3]);
    if (again != results[3]) return 2;
    std::vector<int64_t> edges;
    decoder.decode_to_edges(shots[3], edges);
    if (edges.empty() || edges.size() % 2) return 3;
    std::cout << "installed CMake StructuredDecoder: passed\n";
}
