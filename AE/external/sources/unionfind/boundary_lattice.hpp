#pragma once
#include "LatticeFromParity.hpp"
#include <stdexcept>
#include <set>

// Reuse upstream graph construction and its robin-map edge/adjacency ordering.
// Every open edge gets a private degree-one virtual vertex; there is no
// shared boundary hub that could introduce a path between distant detectors.
class BoundaryLattice : public UnionFindCPP::LatticeFromParity {
    struct CSR {
        uint32_t real, vertices, edges;
        std::vector<int> indices, indptr;
        std::vector<uint32_t> boundaries;
    };
    uint32_t real_;
    std::vector<uint32_t> boundaries_;

    static CSR make_csr(uint32_t real, const std::vector<std::pair<int, int>>& edges) {
        if (!real || edges.empty()) throw std::invalid_argument("Empty graph");
        CSR csr{real, real, static_cast<uint32_t>(edges.size()), {}, {}, {}};
        std::vector<std::vector<int>> rows(real);
        std::set<std::pair<int, int>> unique;
        for (int i = 0; i < static_cast<int>(edges.size()); ++i) {
            auto [u, v] = edges[i];
            if (u < 0 || u >= static_cast<int>(real) || v < -1 ||
                v >= static_cast<int>(real) || u == v)
                throw std::invalid_argument("Invalid detector endpoints");
            if (!unique.insert(std::minmax(u, v)).second)
                throw std::invalid_argument("Parallel edges must be merged before construction");
            if (v == -1) {
                v = csr.vertices++;
                csr.boundaries.push_back(v);
                rows.emplace_back();
            }
            rows[u].push_back(i);
            rows[v].push_back(i);
        }
        csr.indptr.push_back(0);
        for (auto& row : rows) {
            csr.indices.insert(csr.indices.end(), row.begin(), row.end());
            csr.indptr.push_back(static_cast<int>(csr.indices.size()));
        }
        return csr;
    }
    explicit BoundaryLattice(CSR csr)
        : LatticeFromParity(csr.vertices, csr.edges, csr.indices.data(), csr.indptr.data()),
          real_(csr.real), boundaries_(std::move(csr.boundaries)) {}
public:
    BoundaryLattice(uint32_t real, const std::vector<std::pair<int, int>>& edges)
        : BoundaryLattice(make_csr(real, edges)) {}
    bool is_boundary(uint32_t v) const { return v >= real_; }
    const std::vector<uint32_t>& boundary_vertices() const { return boundaries_; }
};
