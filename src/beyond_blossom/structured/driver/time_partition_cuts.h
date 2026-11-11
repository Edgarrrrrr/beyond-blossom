// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef PY_MATCHING_STRUCTURED_TIME_PARTITION_CUTS_H
#define PY_MATCHING_STRUCTURED_TIME_PARTITION_CUTS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace beyond_blossom::structured {

// Time planes use consecutive cell IDs. All geometry and guard arithmetic is
// bound once, before shots; classification only compares those cell IDs.
class TimePartitionCuts {
    struct Cut {
        size_t cell = 0, lower = 0, upper = 0;
    };
    std::array<Cut, 7> cuts_{};
    size_t cut_count_ = 0;
    // Only the two-axis geometry owns this constructor-built table. A full
    // cell table avoids division/modulo in the per-shot count/reorder passes.
    std::vector<uint8_t> ty_buckets_;

   public:
    enum class Geometry : uint8_t { symmetric_width, three_planes, ty_2x2 };
    TimePartitionCuts() = default;
    TimePartitionCuts(size_t plane, size_t nt, size_t partitions, size_t width,
                      Geometry geometry = Geometry::symmetric_width) {
        const bool valid_guard = geometry == Geometry::symmetric_width ? width >= 2
            : geometry == Geometry::three_planes && width == 0;
        if (!plane || nt < 2 || (partitions != 1 && partitions != 2 && partitions != 4 && partitions != 8) || !valid_guard)
            throw std::invalid_argument("Invalid shared time partition geometry.");
        if (plane > std::numeric_limits<size_t>::max() / nt)
            throw std::overflow_error("Shared time partition cell count overflows size_t.");
        cut_count_ = partitions - 1;
        const size_t extent = nt - 2;
        for (size_t k = 1; k < partitions; ++k) {
            // This equals 1 + extent*k/partitions without overflowing the product.
            const size_t cut = 1 + (extent / partitions) * k + (extent % partitions) * k / partitions;
            // Legacy guards use [cut-width, cut+width); three_planes uses
            // [cut-1, cut+2). Both clip safely at either grid endpoint.
            const size_t left = geometry == Geometry::three_planes ? 1 : width;
            const size_t right = geometry == Geometry::three_planes ? 2 : width;
            const size_t lower = left >= cut ? 0 : cut - left;
            const size_t upper = right >= nt - cut ? nt : cut + right;
            cuts_[k - 1] = {cut * plane, lower * plane, upper * plane};
        }
    }

    TimePartitionCuts(size_t nx, size_t ny, size_t nt, size_t partitions, size_t width, Geometry geometry) {
        if (!nx || ny < 2 || nt < 2)
            throw std::invalid_argument("Invalid shared grid partition geometry.");
        if (nx > std::numeric_limits<size_t>::max() / ny)
            throw std::overflow_error("Shared grid partition plane overflows size_t.");
        const size_t plane = nx * ny;
        if (geometry != Geometry::ty_2x2) {
            *this = TimePartitionCuts(plane, nt, partitions, width, geometry);
            return;
        }
        if (partitions != 4 || width != 0)
            throw std::invalid_argument("Two-axis t-y geometry requires P4 and width0.");
        if (plane > std::numeric_limits<size_t>::max() / nt)
            throw std::overflow_error("Shared grid partition cell count overflows size_t.");
        const size_t volume = plane * nt;
        if (volume > ty_buckets_.max_size())
            throw std::length_error("Two-axis partition table exceeds its allocation range.");
        ty_buckets_.resize(volume);
        const size_t t_cut = 1 + (nt - 2) / 2;
        const size_t y_cut = 1 + (ny - 2) / 2;
        // Half-open [cut-1,cut+2), expressed without overflowing cut+2.
        const auto conflict = [](size_t coordinate, size_t cut) {
            return coordinate < cut ? cut - coordinate <= 1 : coordinate - cut < 2;
        };
        for (size_t t = 0; t < nt; ++t) {
            const bool t_conflict = conflict(t, t_cut);
            for (size_t y = 0; y < ny; ++y) {
                const uint8_t bucket = t_conflict || conflict(y, y_cut)
                    ? uint8_t{4} : uint8_t(2 * (t >= t_cut) + (y >= y_cut));
                const size_t row = (t * ny + y) * nx;
                std::fill_n(ty_buckets_.data() + row, nx, bucket);
            }
        }
    }

    // cell is a valid cell ID in the grid used at construction. The last
    // bucket is the shared conflict bucket; preceding buckets are interiors.
    size_t bucket(size_t cell) const noexcept {
        if (!ty_buckets_.empty())
            return ty_buckets_[cell];
        size_t owner = 0;
        for (size_t k = 0; k < cut_count_; ++k) {
            const auto& cut = cuts_[k];
            if (cell >= cut.lower && cell < cut.upper)
                return cut_count_ + 1;
            owner += cell >= cut.cell;
        }
        return owner;
    }
};

}  // namespace beyond_blossom::structured
#endif
