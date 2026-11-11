// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_REGION_SHELL_H
#define PYMATCHING_STRUCTURED_REGION_SHELL_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace pm::structured {
class DetectorNode;

// A contiguous, non-owning pointer sequence with one inline element. Singleton
// regions need no shell allocation, and larger shells retain vector ordering.
// The three-word layout keeps the enclosing Region's direct-index stride fixed.
class RegionShell {
    union Storage {
        DetectorNode* inline_node = nullptr;
        DetectorNode** heap;
    } storage_;
    size_t size_ = 0;
    size_t capacity_ = 1;

    void grow_and_push(DetectorNode* node) {
        constexpr size_t max_capacity = std::numeric_limits<size_t>::max() / sizeof(DetectorNode*);
        if (capacity_ == max_capacity)
            throw std::length_error("Region shell exhausted.");
        const size_t capacity = capacity_ > max_capacity / 2 ? max_capacity : capacity_ * 2;
        auto* replacement = new DetectorNode*[capacity];
        std::copy(begin(), end(), replacement);
        replacement[size_] = node;
        // Pointer copies cannot throw. Commit only after allocation succeeds.
        if (capacity_ > 1)
            delete[] storage_.heap;
        storage_.heap = replacement;
        capacity_ = capacity;
        ++size_;
    }

   public:
    RegionShell() noexcept = default;
    explicit RegionShell(DetectorNode* node) noexcept : storage_{node}, size_(1) {
    }
    ~RegionShell() {
        if (capacity_ > 1)
            delete[] storage_.heap;
    }
    RegionShell(const RegionShell&) = delete;
    RegionShell& operator=(const RegionShell&) = delete;
    RegionShell(RegionShell&&) = delete;
    RegionShell& operator=(RegionShell&&) = delete;
    size_t size() const noexcept {
        return size_;
    }
    bool empty() const noexcept {
        return size_ == 0;
    }
    DetectorNode** begin() noexcept {
        return capacity_ == 1 ? &storage_.inline_node : storage_.heap;
    }
    DetectorNode* const* begin() const noexcept {
        return capacity_ == 1 ? &storage_.inline_node : storage_.heap;
    }
    DetectorNode** end() noexcept {
        return begin() + size_;
    }
    DetectorNode* const* end() const noexcept {
        return begin() + size_;
    }
    DetectorNode*& operator[](size_t index) noexcept {
        assert(index < size_);
        return begin()[index];
    }
    DetectorNode* const& operator[](size_t index) const noexcept {
        assert(index < size_);
        return begin()[index];
    }
    DetectorNode* back() const noexcept {
        assert(size_ != 0);
        return begin()[size_ - 1];
    }
    void push_back(DetectorNode* node) {
        if (size_ == capacity_) {
            grow_and_push(node);
        } else {
            begin()[size_++] = node;
        }
    }
    void set_singleton_from_neutral(DetectorNode* node) noexcept {
        assert(size_ == 0 && capacity_ == 1);
        storage_.inline_node = node;
        size_ = 1;
    }
    void replace_unchanged_singleton(DetectorNode* node) noexcept {
        assert(size_ == 1 && capacity_ == 1);
        storage_.inline_node = node;
    }
    void pop_back() noexcept {
        assert(size_ != 0);
        --size_;
    }
    bool operator==(const RegionShell& other) const noexcept {
        return size_ == other.size_ && std::equal(begin(), end(), other.begin());
    }
};
static_assert(sizeof(RegionShell) == 3 * sizeof(void*));
}  // namespace pm::structured
#endif
