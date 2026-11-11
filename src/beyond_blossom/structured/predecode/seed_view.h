// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_SEED_VIEW_H
#define BB_STRUCTURED_SEED_VIEW_H

#include <algorithm>
#include <array>
#include <cassert>
#include <span>
#include "beyond_blossom/structured/predecode/predecode_state.h"

namespace beyond_blossom::structured {

// Physical slots are stable within a shot. Only these prefixes contain seeds;
// the remainder of storage is constructed backing, with no seed identity.
class SeedView {
   public:
    struct Range {
        size_t begin = 0, end = 0, rank_begin = 0;
        size_t count() const noexcept { return end - begin; }
    };
    std::span<PredecodeSeed> storage;

    void bind_dense(std::span<PredecodeSeed> seeds) noexcept {
        storage = seeds;
        ranges[0] = {0, seeds.size(), 0};
        range_count = 1;
        count = seeds.size();
    }
    void bind_fixed(std::span<PredecodeSeed> backing, std::span<const size_t> bases) noexcept {
        assert(bases.size() >= 2 && bases.size() <= ranges.size() + 1);
        assert(bases.front() == 0 && bases.back() == backing.size());
        storage = backing;
        range_count = bases.size() - 1;
        count = 0;
        for (size_t b = 0; b < range_count; ++b)
            ranges[b] = {bases[b], bases[b], 0};
    }
    void set_counts(std::span<const size_t> counts) noexcept {
        assert(counts.size() == range_count);
        count = 0;
        for (size_t b = 0; b < range_count; ++b) {
            const size_t capacity_end = b + 1 == range_count ? storage.size() : ranges[b + 1].begin;
            assert(counts[b] <= capacity_end - ranges[b].begin);
            ranges[b].end = ranges[b].begin + counts[b];
            ranges[b].rank_begin = count;
            count += counts[b];
        }
    }
    void retire() noexcept { storage = {}; range_count = count = 0; }
    size_t active_count() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    std::span<const Range> active_ranges() const noexcept { return std::span(ranges).first(range_count); }
    bool contains(size_t slot) const noexcept {
        for (const auto& r : active_ranges())
            if (slot >= r.begin && slot < r.end) return true;
        return false;
    }
    // Compile-time bucket count keeps the hot mate conversion a few compares.
    // Last-base selection also handles repeated bases from empty capacities.
    template <size_t Partitions>
    uint32_t rank_of(uint32_t slot) const noexcept {
        static_assert(Partitions == 2 || Partitions == 4 || Partitions == 8);
        assert(range_count == Partitions + 1 && contains(slot));
        size_t b = 0;
        for (size_t k = 1; k <= Partitions; ++k)
            if (slot >= ranges[k].begin) b = k;
        return uint32_t(ranges[b].rank_begin + slot - ranges[b].begin);
    }
    uint32_t rank_of(uint32_t slot) const noexcept {
        assert(contains(slot));
        for (const auto& r : active_ranges())
            if (slot >= r.begin && slot < r.end)
                return uint32_t(r.rank_begin + slot - r.begin);
        return UINT32_MAX;
    }
    uint32_t slot_of(size_t rank) const noexcept {
        assert(rank < count);
        for (const auto& r : active_ranges())
            if (rank >= r.rank_begin && rank < r.rank_begin + r.count())
                return uint32_t(r.begin + rank - r.rank_begin);
        return UINT32_MAX;
    }
    template <typename F>
    void for_rank_range(size_t begin, size_t end, F&& f) const {
        assert(begin <= end && end <= count);
        for (const auto& r : active_ranges()) {
            const auto lo = std::max(begin, r.rank_begin);
            const auto hi = std::min(end, r.rank_begin + r.count());
            if (lo < hi) f(r.begin + lo - r.rank_begin, r.begin + hi - r.rank_begin, lo);
        }
    }

   private:
    std::array<Range, 9> ranges{};
    size_t range_count = 0, count = 0;
};

}  // namespace beyond_blossom::structured
#endif
