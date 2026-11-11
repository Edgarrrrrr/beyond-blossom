// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

#ifndef BB_STRUCTURED_REGION_RADIUS_STORE_H
#define BB_STRUCTURED_REGION_RADIUS_STORE_H

#include <limits>
#include <stdexcept>
#include <vector>

#include "beyond_blossom/core/interop/varying.h"

namespace beyond_blossom::structured {

// Fixed storage keeps references valid throughout region allocation and recycling.
class RegionRadiusStore {
    std::vector<VaryingCT> values_;

   public:
    explicit RegionRadiusStore(size_t capacity) {
        if (capacity >= std::numeric_limits<uint32_t>::max())
            throw std::length_error("Too many regions for the grid radius store.");
        values_.resize(capacity + 1, VaryingCT{0});
    }
    RegionRadiusStore(const RegionRadiusStore&) = delete;
    RegionRadiusStore& operator=(const RegionRadiusStore&) = delete;
    VaryingCT& at(uint32_t id) {
        if (id == 0 || id >= values_.size())
            throw std::out_of_range("Invalid mutable region id.");
        return values_[id];
    }
    VaryingCT* data() noexcept {
        return values_.data();
    }
    const VaryingCT* data() const noexcept {
        return values_.data();
    }
};
}  // namespace beyond_blossom::structured
#endif
