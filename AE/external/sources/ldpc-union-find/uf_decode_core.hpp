#ifndef LDPC_UF_DECODE_CORE_HPP
#define LDPC_UF_DECODE_CORE_HPP

#include "union_find.hpp"
#include <stdexcept>

namespace ldpc::uf {

// Separate per-shot preparation and resource cleanup from the existing
// unweighted growth/merge/spanning-tree/peeling algorithm. The fixed upstream
// Cluster implementation is used without changing its growth or tie rules.
class PreparedUfDecoder {
    ldpc::bp::BpSparse &pcm_;
    UfDecoder graph_info_;
    std::vector<Cluster *> global_bit_membership_;
    std::vector<Cluster *> global_check_membership_;
    std::vector<Cluster *> clusters_;
    std::vector<Cluster *> invalid_clusters_;
    const std::vector<uint8_t> *syndrome_ = nullptr;
    bool ready_ = false;

public:
    explicit PreparedUfDecoder(ldpc::bp::BpSparse &pcm)
        : pcm_(pcm), graph_info_(pcm), global_bit_membership_(pcm.n, nullptr),
          global_check_membership_(pcm.m, nullptr) {
        if (!graph_info_.pcm_max_bit_degree_2)
            throw std::runtime_error("Prepared peeling requires column degrees 1 or 2");
        clusters_.reserve(pcm.m);
        invalid_clusters_.reserve(pcm.m);
    }

    PreparedUfDecoder(const PreparedUfDecoder &) = delete;
    PreparedUfDecoder &operator=(const PreparedUfDecoder &) = delete;
    ~PreparedUfDecoder() { finish_shot(); }

    // Outside the core timer: validate the input, clear prior state and output,
    // reset membership arrays, and create the initial syndrome clusters.
    void prepare_shot(const std::vector<uint8_t> &syndrome) {
        if (syndrome.size() != static_cast<size_t>(pcm_.m))
            throw std::invalid_argument("Incorrect syndrome length");
        finish_shot();
        std::fill(graph_info_.decoding.begin(), graph_info_.decoding.end(), 0);
        std::fill(global_bit_membership_.begin(), global_bit_membership_.end(), nullptr);
        std::fill(global_check_membership_.begin(), global_check_membership_.end(), nullptr);
        for (int i = 0; i < pcm_.m; ++i) {
            if (syndrome[i] == 1) {
                auto cluster = std::make_unique<Cluster>(pcm_, i,
                    global_check_membership_.data(), global_bit_membership_.data(),
                    graph_info_.planar_code_boundary_bits);
                clusters_.push_back(cluster.get());
                invalid_clusters_.push_back(cluster.get());
                cluster.release();
            } else if (syndrome[i] != 0) {
                throw std::invalid_argument("Syndrome is not binary");
            }
        }
        syndrome_ = &syndrome;
        ready_ = true;
    }

    // Timed function. Initial clusters already exist, but every growth, merge,
    // validity update, sort, spanning-tree construction, peeling operation and
    // write of the correction is still performed here. No future-shot work is
    // prepared here, and no decoded answer is computed in prepare_shot().
    const std::vector<uint8_t> &decode_prepared() {
        if (!ready_) throw std::logic_error("Call prepare_shot before each decode");
        ready_ = false;
        while (!invalid_clusters_.empty()) {
            for (auto cluster : invalid_clusters_) {
                if (cluster->active) cluster->grow_cluster(EMPTY_DOUBLE_VECTOR, 1);
            }
            invalid_clusters_.clear();
            for (auto cluster : clusters_) {
                if (cluster->active && cluster->parity() == 1 && !cluster->contains_boundary_bits)
                    invalid_clusters_.push_back(cluster);
            }
            std::sort(invalid_clusters_.begin(), invalid_clusters_.end(),
                [](const Cluster *lhs, const Cluster *rhs) {
                    return lhs->bit_nodes.size() < rhs->bit_nodes.size();
                });
        }
        for (auto cluster : clusters_) {
            if (cluster->active) {
                auto erasure = cluster->peel_decode(*syndrome_);
                for (int bit : erasure) graph_info_.decoding[bit] = 1;
            }
        }
        return graph_info_.decoding;
    }

    // Outside the core timer. The correction remains valid until the next
    // prepare_shot() or decoder destruction. All allocated clusters are owned
    // by clusters_, including those made inactive by a merge.
    void finish_shot() noexcept {
        for (auto cluster : clusters_) delete cluster;
        clusters_.clear();
        invalid_clusters_.clear();
        syndrome_ = nullptr;
        ready_ = false;
    }
};

}
#endif
