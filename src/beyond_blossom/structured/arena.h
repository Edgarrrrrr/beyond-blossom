// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_ARENA_H
#define BB_STRUCTURED_ARENA_H

#include <cassert>
#include <cstdlib>
#include <new>
#include <span>
#include "beyond_blossom/core/arena.h"

namespace beyond_blossom::structured {

// Structured import reserves disjoint root slots before dispatch. The generic
// arena and its allocation/destruction algorithm remain the upstream types.
template <typename T>
struct StructuredArena : beyond_blossom::Arena<T> {
    std::span<T*> borrow_available(size_t count) {
        auto& allocated = this->allocated;
        auto& available = this->available;
        if (available.size() < count) {
            const size_t missing = count - available.size();
            allocated.reserve(allocated.size() + missing);
            available.reserve(count);
            for (size_t i = 0; i < missing; ++i) {
                auto* p = static_cast<T*>(std::malloc(sizeof(T)));
                if (p == nullptr) throw std::bad_alloc();
                allocated.push_back(p);
                available.push_back(p);
            }
        }
        return std::span<T*>(available).last(count);
    }

    void commit_borrowed_available(size_t count) noexcept {
        assert(count <= this->available.size());
        this->available.resize(this->available.size() - count);
    }
};

}  // namespace beyond_blossom::structured
#endif
