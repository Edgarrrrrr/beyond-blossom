// Copyright 2022 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "beyond_blossom/structured/flooder/graph_fill_region.h"

#include "beyond_blossom/core/interop/varying.h"
#include "beyond_blossom/structured/flooder/graph.h"

namespace beyond_blossom::structured {

namespace {
size_t checked_region_capacity(size_t capacity) {
    if (capacity >= std::numeric_limits<RegionId>::max())
        throw std::length_error("Too many regions for the object pool.");
    return capacity;
}
}  // namespace

RegionPool::RegionPool(size_t capacity)
    : capacity_(checked_region_capacity(capacity)),
      states_(capacity_ + 1, 0),
      generations_(capacity_ + 1, 0),
      seed_extraction_(capacity_ + 1),
      seed_endpoints_(capacity_ + 1),
      seed_node_flags_(capacity_ + 1, 0),
      radii(capacity_) {
    available_.reserve(capacity);
    regions_ = allocator_.allocate(capacity + 1);
}
RegionPool::~RegionPool() {
    for (RegionId id = 1; id < lifetime_end_; ++id)
        if (has_object(states_[id]))
            regions_[id].~GraphFillRegion();
    allocator_.deallocate(regions_, capacity_ + 1);
}
RegionId RegionPool::reserve_one() {
    RegionId id;
    if (!available_.empty()) {
        id = available_.back();
        // Never wrap: an old event must never acquire a new object's identity.
        if (generations_[id] == std::numeric_limits<RegionGeneration>::max())
            throw std::overflow_error("Region generation exhausted.");
        available_.pop_back();
    } else {
        if (next_ > capacity_)
            throw std::length_error("Region object pool exhausted.");
        id = next_;
        if (generations_[id] == std::numeric_limits<RegionGeneration>::max())
            throw std::overflow_error("Region generation exhausted.");
        ++next_;
    }
    assert(states_[id] == free_raw || states_[id] == free_neutral || states_[id] == free_seed_match || states_[id] == free_lazy_raw);
    neutral_reset_end_ = 0;
    lifetime_end_ = std::max(lifetime_end_, static_cast<RegionId>(id + 1));
    ++generations_[id];
    seed_node_flags_[id] = 0;
    states_[id] = states_[id] == free_neutral ? reserved_neutral :
                      states_[id] == free_seed_match ? reserved_seed_match : reserved_raw;
    return id;
}
std::vector<RegionId> RegionPool::reserve_ids(size_t count) {
    if (count > available_.size() + capacity_ + 1 - next_)
        throw std::length_error("Region object pool exhausted.");
    std::vector<RegionId> ids;
    ids.reserve(count);
    try {
        for (size_t i = 0; i < count; ++i)
            ids.push_back(reserve_one());
    } catch (...) {
        for (auto id : ids)
            release(id);
        throw;
    }
    return ids;
}
RegionId RegionPool::reserve_contiguous_ids(size_t count) {
    if (!empty())
        throw std::logic_error("Contiguous reservation requires an idle region pool.");
    if (count > capacity_)
        throw std::length_error("Region object pool exhausted.");
    for (size_t id = 1; id <= count; ++id)
        if (generations_[id] == std::numeric_limits<RegionGeneration>::max())
            throw std::overflow_error("Region generation exhausted.");
    // Every old slot is free. A subsequent allocation can advance from count+1
    // and retains the old generation even when reusing an earlier high-water slot.
    available_.clear();
    neutral_reset_end_ = 0;
    contiguous_shot_ = count != 0;
    next_ = static_cast<RegionId>(count + 1);
    lifetime_end_ = std::max(lifetime_end_, next_);
    for (RegionId id = 1; id < next_; ++id) {
        assert(states_[id] == free_raw || states_[id] == free_neutral || states_[id] == free_seed_match || states_[id] == free_lazy_raw);
        ++generations_[id];
        seed_node_flags_[id] = 0;
        states_[id] = states_[id] == free_neutral ? reserved_neutral :
                      states_[id] == free_seed_match ? reserved_seed_match : reserved_raw;
    }
    return 1;
}
GraphFillRegion &RegionPool::construct_reserved(RegionId id) noexcept {
    assert(id && id < next_ && (states_[id] == reserved_raw || states_[id] == reserved_neutral || states_[id] == reserved_seed_match));
    seed_node_flags_[id] = 0;
    auto *region = &regions_[id];
    if (states_[id] == reserved_seed_match) {
        region->~GraphFillRegion();
        region = new (region) GraphFillRegion(*this, id);
    } else if (states_[id] == reserved_raw)
        region = new (region) GraphFillRegion(*this, id);
    radii.data()[id] = VaryingCT{1};
    states_[id] = live;
    return *region;
}
GraphFillRegion *RegionPool::allocate() {
    return &construct_reserved(reserve_one());
}
void RegionPool::bind_compact_seed_nodes(DetectorNode* nodes, size_t count) noexcept {
    assert(nodes && count && count <= UINT32_MAX);
    assert(compact_node_base_ == nullptr || (compact_node_base_ == nodes && compact_node_count_ == count));
    compact_node_base_ = nodes;
    compact_node_count_ = count;
}
DetectorNode* RegionPool::seed_node(RegionId id) const noexcept {
    assert(id && id < next_);
    if (seed_node_flags_[id] & borrowed_endpoints)
        return compact_node_base_ + borrowed_seed(id).detector_id;
    if (seed_node_flags_[id] & compact_endpoints)
        return compact_node_base_ + seed_endpoints_[id].detectors.node;
    return seed_endpoints_[id].pointers.node;
}
DetectorNode* RegionPool::seed_mate(RegionId id) const noexcept {
    assert(id && id < next_);
    if (seed_node_flags_[id] & borrowed_endpoints) {
        const auto mate = borrowed_seed(id).mate;
        return mate == PredecodeSeed::BOUNDARY ? nullptr :
            compact_node_base_ + borrowed_seeds_.storage[mate].detector_id;
    }
    if (seed_node_flags_[id] & compact_endpoints) {
        const auto detector = seed_endpoints_[id].detectors.mate;
        return detector == UINT32_MAX ? nullptr : compact_node_base_ + detector;
    }
    return seed_endpoints_[id].pointers.mate;
}
void RegionPool::borrow_seeds(const SeedView& seeds) {
    assert(compact_node_base_ && !has_borrowed_seeds());
    assert(seeds.active_count() + 1 == next_);
    changed_initial_seeds_.clear();
    // At most one entry per initial root or first materialized matched seed.
    // All later appends, including const materialization, are allocation-free.
    changed_initial_seeds_.reserve(seeds.active_count());
    borrowed_seeds_ = seeds;
}
const RegionPool::SeedExtraction& RegionPool::seed_extraction(RegionId id) const noexcept {
    assert(is_unchanged_seed(id));
    if (seed_node_flags_[id] & borrowed_endpoints) {
        const auto& seed = borrowed_seed(id);
        const auto* grid = seed.grid ? seed.grid : compact_node_base_[seed.detector_id].grid_component;
        const auto mate = seed.mate == PredecodeSeed::BOUNDARY ? NO_REGION :
            static_cast<RegionId>(1 + borrowed_seeds_.rank_of(seed.mate));
        seed_extraction_[id] = {grid->observables[seed.cell][seed.match_slot], seed.radius, mate};
    }
    return seed_extraction_[id];
}
GraphFillRegion& RegionPool::materialize_seed(RegionId id) const noexcept {
    assert(is_lazy_seed(id));
    const auto state = states_[id];
    const auto& seed = seed_extraction(id);
    auto* node = seed_node(id);
    auto* mate = seed_mate(id);
    assert(node && ((seed.mate != NO_REGION) == (mate != nullptr)));
    // Cache complete endpoints only when the full object is actually requested.
    // Untouched extraction resolves IDs without writing this pointer payload.
    seed_endpoints_[id].pointers = {node, mate};
    if (seed_node_flags_[id] & borrowed_endpoints) {
        assert(changed_initial_seeds_.size() < changed_initial_seeds_.capacity());
        changed_initial_seeds_.push_back(id);
    }
    seed_node_flags_[id] &= ~(compact_endpoints | borrowed_endpoints);
    if (seed_node_flags_[id] & node_deferred) {
        assert(node->region_that_arrived == NO_REGION && node->reached_from_source == nullptr);
        assert(node->radius_of_arrival == 0 && node->observables_crossed_from_source == 0);
        assert(!node->node_event_tracker.has_queued_time && !node->node_event_tracker.has_desired_time);
        node->region_that_arrived = id;
        node->reached_from_source = node;
        seed_node_flags_[id] &= ~node_deferred;
    }
    const Match match{seed.mate, {node, mate, seed.obs_mask}};
    auto* region = &regions_[id];
    if (state == lazy_seed_match) {
        region->match = match;
        region->shell_area.replace_unchanged_singleton(node);
    } else if (state == lazy_neutral) {
        region->match = match;
        region->shell_area.set_singleton_from_neutral(node);
    } else {
        // Only separately allocated storage and mutable representation metadata
        // change. The pool pointer is retained for later nonconst operations.
        region = new (region) GraphFillRegion(const_cast<RegionPool&>(*this), id, node, match);
    }
    states_[id] = live_seed_match;
    return *region;
}
void RegionPool::release(RegionId id) noexcept {
    assert(id && id < next_ &&
           (states_[id] == reserved_raw || states_[id] == live || states_[id] == live_seed_match ||
            states_[id] == reserved_neutral || states_[id] == reserved_seed_match || is_lazy_state(states_[id])));
    const auto state = states_[id];
    if (has_object(state))
        regions_[id].~GraphFillRegion();
    states_[id] = state == lazy_raw ? free_lazy_raw : free_raw;
    radii.data()[id] = VaryingCT{0};
    available_.push_back(id);
}
void RegionPool::reset() noexcept {
    cancel_neutral_reset();
    borrowed_seeds_.retire();
    changed_initial_seeds_.clear();
    for (RegionId id = 1; id < lifetime_end_; ++id) {
        const auto state = states_[id];
        seed_node_flags_[id] = 0;
        if (has_object(state))
            regions_[id].~GraphFillRegion();
        if (state == live || state == live_seed_match || is_lazy_state(state))
            radii.data()[id] = VaryingCT{0};
        if (state != free_raw)
            states_[id] = free_raw;
    }
    lifetime_end_ = 1;
    available_.clear();
    for (RegionId id = 1; id < next_; ++id)
        available_.push_back(id);
}
void RegionPool::mark_completed_shot() noexcept {
    assert(empty());
    // A completed contiguous import published every initial slot. Later
    // ordinary MWPM allocations can only reuse these IDs or extend the range.
    // Ordinary-only decoding after an abort can have unused holes; it keeps
    // the existing construction path instead of rebuilding those empty slots.
    if (contiguous_shot_) {
        neutral_reset_end_ = next_;
        contiguous_shot_ = false;
    }
}
RegionId RegionPool::take_neutral_reset_end() noexcept {
    const auto end = neutral_reset_end_;
    neutral_reset_end_ = 0;
    return end;
}
void RegionPool::prepare_neutral_range(RegionId begin, RegionId end) noexcept {
    assert(empty() && begin && begin <= end && end <= next_ && end <= lifetime_end_);
    for (RegionId id = begin; id < end; ++id) {
        assert(states_[id] == free_raw || states_[id] == free_neutral || states_[id] == free_seed_match || states_[id] == free_lazy_raw);
        if (states_[id] == free_raw) {
            new (&regions_[id]) GraphFillRegion(*this, id);
            states_[id] = free_neutral;
        }
    }
}
bool RegionPool::empty() const noexcept {
    return available_.size() == next_ - 1;
}

GraphFillRegion::GraphFillRegion(RegionPool &pool, RegionId id) noexcept
    : blossom_parent(NO_REGION),
      blossom_parent_top(id),
      alt_tree_node(nullptr),
      shrink_event_tracker(),
      match{NO_REGION, {nullptr, nullptr, 0}},
      region_id(id),
      pool(&pool) {
}
GraphFillRegion::~GraphFillRegion() = default;
bool GraphFillRegion::tree_equal(const GraphFillRegion &other) const {
    if (alt_tree_node != other.alt_tree_node || radius_value() != other.radius_value() ||
        blossom_children.size() != other.blossom_children.size() || shell_area != other.shell_area) {
        return false;
    }
    if (blossom_children.empty())
        return true;
    for (size_t i = 0; i < blossom_children.size(); i++) {
        if (blossom_children[i].edge != other.blossom_children[i].edge)
            return false;
        if (!(*pool)[blossom_children[i].region].tree_equal((*other.pool)[other.blossom_children[i].region]))
            return false;
    }
    return true;
}

bool GraphFillRegion::operator==(const GraphFillRegion &rhs) const {
    return tree_equal(rhs);
}

bool GraphFillRegion::operator!=(const GraphFillRegion &rhs) const {
    return !(rhs == *this);
}

void GraphFillRegion::add_match(GraphFillRegion *region, const CompressedEdge &edge) {
    assert(pool == region->pool);
    pool->invalidate_seed_match(region_id);
    pool->invalidate_seed_match(region->region_id);
    match = Match{region->region_id, edge};
    region->match = Match{region_id, edge.reversed()};
}

void GraphFillRegion::cleanup_shell_area() {
    for (auto &detector_node : shell_area) {
        detector_node->reset();
    }
}

void GraphFillRegion::cleanup_shell_area_deferring_seed_nodes() {
    for (auto* node : shell_area) {
        // The predecoder's index stays live throughout MWPM and extraction.
        // Only original imported seeds defer cleanup; grown nodes retire now.
        if (node->grid_component->predecode.active[node->grid_cell]) {
            node->region_that_arrived = NO_REGION;
            node->set_top_region(nullptr);
        } else {
            node->reset();
        }
    }
}

void GraphFillRegion::clear_blossom_parent() {
    blossom_parent = NO_REGION;
    do_op_for_each_descendant_and_self([&](GraphFillRegion *descendant) {
        pool->invalidate_seed_match(descendant->region_id);
        descendant->blossom_parent_top = region_id;
        for (DetectorNode *n : descendant->shell_area) {
            n->set_top_region(this);
            n->set_wrapped_radius(n->compute_wrapped_radius());
        }
    });
}

void GraphFillRegion::clear_blossom_parent_ignoring_wrapped_radius() {
    blossom_parent = NO_REGION;
    do_op_for_each_descendant_and_self([&](GraphFillRegion *descendant) {
        pool->invalidate_seed_match(descendant->region_id);
        descendant->blossom_parent_top = region_id;
        for (DetectorNode *n : descendant->shell_area) {
            n->set_top_region(this);
        }
    });
}

void GraphFillRegion::wrap_into_blossom(GraphFillRegion *new_blossom_parent_and_top) {
    assert(pool == new_blossom_parent_and_top->pool);
    blossom_parent = new_blossom_parent_and_top->region_id;
    do_op_for_each_descendant_and_self([&](GraphFillRegion *descendant) {
        pool->invalidate_seed_match(descendant->region_id);
        descendant->blossom_parent_top = new_blossom_parent_and_top->region_id;
        for (DetectorNode *n : descendant->shell_area) {
            n->set_top_region(new_blossom_parent_and_top);
            n->set_wrapped_radius(n->compute_wrapped_radius());
        }
    });
}

bool GraphFillRegion::operator<=(const GraphFillRegion &rhs) const {
    const GraphFillRegion *r = this;
    while (r != nullptr && r != &rhs) {
        r = r->parent_region();
    }
    return r == &rhs;
}

bool GraphFillRegion::operator<(const GraphFillRegion &rhs) const {
    return this != &rhs && (*this <= rhs);
}

bool GraphFillRegion::operator>(const GraphFillRegion &rhs) const {
    return rhs < *this;
}

bool GraphFillRegion::operator>=(const GraphFillRegion &rhs) const {
    return rhs <= *this;
}

}  // namespace beyond_blossom::structured
