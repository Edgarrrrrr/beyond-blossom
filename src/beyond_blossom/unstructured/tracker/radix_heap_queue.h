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

#ifndef BEYOND_BLOSSOM2_BUCKET_QUEUE_H
#define BEYOND_BLOSSOM2_BUCKET_QUEUE_H

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <queue>
#include <vector>

#include "beyond_blossom/core/ints.h"
#if BB_ENABLE_PROFILE
#include "beyond_blossom/profile/profile.h"
#endif
#include "beyond_blossom/core/tracker/cyclic.h"
#include "beyond_blossom/unstructured/tracker/flood_check_event.h"

namespace beyond_blossom {

#if BB_ENABLE_PROFILE
inline beyond_blossom::profile::QueueKind profile_queue_kind_for_event(FloodCheckEventType event_type) {
    switch (event_type) {
        case LOOK_AT_NODE:
            return beyond_blossom::profile::QueueKind::node;
        case LOOK_AT_SHRINKING_REGION:
            return beyond_blossom::profile::QueueKind::region;
        case LOOK_AT_SEARCH_NODE:
            return beyond_blossom::profile::QueueKind::search;
        case NO_FLOOD_CHECK_EVENT:
            return beyond_blossom::profile::QueueKind::none;
    }
    return beyond_blossom::profile::QueueKind::none;
}
#endif

/// A monotonic priority queue for TentativeEvents.
///
/// The priority queue assumes that times increase monotonically. The caller must not enqueue a
/// time that is cycle-before the time of the last popped event. Time t1 is "cycle-before" time t2
/// iff it takes more increments to get from t1 to t2 than it takes to get from t2 to t1.
///
/// The priority queue works by categorizing upcoming events into groups based on the most
/// significant bit that differs between the event's time and the current time. For example, here
/// are the times that are stored in each bucket assuming the current time is 17:
///
///     (current time = 17)
///     bucket 0: event time in [17]
///     bucket 1: event time in [] (empty because 17 & (1 << 0) is non-zero)
///     bucket 2: event time in [18, 19]
///     bucket 3: event time in [20, 21, 22, 23]
///     bucket 4: event time in [24, 25, 26, 27, 28, 29, 30, 31]
///     bucket 5: event time in [] (empty because 17 & (1 << 4) is non-zero)
///     bucket 6: event time in [32, ..., 63]
///     bucket 7: event time in [64, ..., 127]
///     bucket 8: event time in [128, ..., 255]
///     bucket 9: event time in [256, ..., 511]
///     ...
///     bucket K: event time in [1<<(K-1), (1<<K)-1]
///     ...
///     bucket 31: event time in [1<<30, (1<<31)-1]
///     bucket 32: [invalid for an event to go into this bucket]
///
/// Dequeueing always happens from bucket 0. Whenever bucket 0 is empty, it's refilled by
/// redistributing events from the smallest non-empty bucket (after advancing the time to the
/// minimum time of any event in that bucket).
///
/// The experience of a single event going through the queue is, roughly speaking:
/// - Initially end up in one of the higher buckets.
/// - Sit there for awhile as the lower buckets are drained.
/// - Get redistributed to a slightly lower bucket.
/// - Repeatedly wait and get redistributed to lower and lower buckets until in bucket 0.
/// - Get dequeued out of bucket 0 and yielded as a result.
template <bool use_validation>
struct radix_heap_queue {
    std::array<std::vector<FloodCheckEvent>, sizeof(beyond_blossom::cyclic_time_int) * 8 + 1> bit_buckets;
    beyond_blossom::cumulative_time_int cur_time;
    size_t _num_enqueued;
    // Tracks nonzero buckets, including wraparound. Bucket zero is accessed
    // directly, so ordinary pops need no bitmap update or empty-bucket branch.
    uint64_t nonempty_high_buckets = 0;

    radix_heap_queue() : cur_time{0}, _num_enqueued(0) {
    }

    size_t size() const {
        return _num_enqueued;
    }

    bool empty() const {
        return _num_enqueued == 0;
    }

    /// Determines which bucket an event with the given time should go into.
    inline size_t cur_bit_bucket_for(cyclic_time_int time) const {
        return std::bit_width((uint64_t)(time.value ^ cyclic_time_int{cur_time}.value));
    }

    /// Adds an event to the priority queue.
    ///
    /// The event MUST NOT be cycle-before the current time.
    void enqueue(FloodCheckEvent event) {
#if BB_ENABLE_PROFILE
        bool trace_queue = beyond_blossom::profile::trace_enabled(beyond_blossom::profile::TraceLevel::queue);
        auto kind = beyond_blossom::profile::stats_enabled() || trace_queue
                        ? profile_queue_kind_for_event(event.tentative_event_type)
                        : beyond_blossom::profile::QueueKind::none;
        auto range = beyond_blossom::profile::queue_scoped_range(kind, "enq", beyond_blossom::profile::Color::Purple);
#endif
        if (use_validation) {
            if (event.time < cyclic_time_int{cur_time}) {
                std::stringstream ss;
                ss << "Attempted to schedule an event cycle-before the present.\n";
                ss << "    current time: " << cur_time << "\n";
                ss << "    tentative event: " << event << "\n";
                throw std::invalid_argument(ss.str());
            }
        }
        auto target_bucket = cur_bit_bucket_for(event.time);
        bit_buckets[target_bucket].push_back(event);
        if (target_bucket)
            nonempty_high_buckets |= uint64_t{1} << target_bucket;
        _num_enqueued++;
#if BB_ENABLE_PROFILE
        beyond_blossom::profile::record_queue_enqueue(kind, target_bucket);
#endif
    }

    /// Checks if all events are in the correct bucket.
    bool satisfies_invariants() const {
        uint64_t actual_nonempty = 0;
        size_t actual_count = 0;
        for (size_t b = 0; b < bit_buckets.size(); ++b) {
            if (b && !bit_buckets[b].empty())
                actual_nonempty |= uint64_t{1} << b;
            actual_count += bit_buckets[b].size();
        }
        if (actual_nonempty != nonempty_high_buckets || actual_count != _num_enqueued)
            return false;
        for (size_t b = 0; b < bit_buckets.size(); b++) {
            for (size_t k = 0; k < bit_buckets[b].size(); k++) {
                if (cur_bit_bucket_for(bit_buckets[b][k].time) != b) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Dequeues the next event.
    ///
    /// If the queue is empty, a tentative event with type NO_TENTATIVE_EVENT is returned.
    FloodCheckEvent dequeue() {
#if BB_ENABLE_PROFILE
        beyond_blossom::profile::ScopedRange range(beyond_blossom::profile::TraceLevel::queue, "q.dequeue", beyond_blossom::profile::Color::Purple);
#endif
        if (_num_enqueued == 0)
            return FloodCheckEvent(cyclic_time_int{0});
        if (bit_buckets[0].empty()) {
            // Need to refill bucket 0, so we can dequeue from it.

            // Find first non-empty bucket. It has the soonest event.
            const size_t b = std::countr_zero(nonempty_high_buckets);
            nonempty_high_buckets &= ~(uint64_t{1} << b);
#if BB_ENABLE_PROFILE
            beyond_blossom::profile::record_queue_redistribution(b, bit_buckets[b].size());
            beyond_blossom::profile::ScopedRange redistribution_range(
                beyond_blossom::profile::TraceLevel::queue, "q.redistribute", beyond_blossom::profile::Color::Brown);
#endif

            if (b == 1) {
                // Special case:
                std::swap(bit_buckets[0], bit_buckets[1]);
                cur_time++;
            } else {
                auto &source_bucket = bit_buckets[b];

                // Advance time to the minimum time in the bucket.
                decltype(cyclic_time_int::value) min_time = source_bucket[0].time.value;
                for (size_t k = 1; k < source_bucket.size(); k++) {
                    min_time = std::min(min_time, source_bucket[k].time.value);
                }
                cur_time = cyclic_time_int{min_time}.widen_from_nearby_reference(cur_time);

                // Redistribute the contents of the bucket.
                for (auto &e : source_bucket) {
                    auto target_bucket = cur_bit_bucket_for(e.time);
                    bit_buckets[target_bucket].push_back(e);
                    if (target_bucket)
                        nonempty_high_buckets |= uint64_t{1} << target_bucket;
                }
                source_bucket.clear();
            }
        }

        _num_enqueued--;
        FloodCheckEvent result = bit_buckets[0].back();
        bit_buckets[0].pop_back();
#if BB_ENABLE_PROFILE
        bool trace_queue = beyond_blossom::profile::trace_enabled(beyond_blossom::profile::TraceLevel::queue);
        auto kind = beyond_blossom::profile::stats_enabled() || trace_queue
                        ? profile_queue_kind_for_event(result.tentative_event_type)
                        : beyond_blossom::profile::QueueKind::none;
        auto event_range = beyond_blossom::profile::queue_scoped_range(kind, "deq", beyond_blossom::profile::Color::Purple);
        beyond_blossom::profile::record_queue_dequeue(kind);
#endif
        return result;
    }

    /// Lists the sorted events in the queue.
    ///
    /// This method mostly exacts to facilitate testing. It doesn't really make sense to use it
    /// during normal operation.
    std::vector<FloodCheckEvent> to_vector() const {
        std::vector<FloodCheckEvent> result;
        for (size_t b = 0; b < bit_buckets.size(); b++) {
            result.insert(result.begin(), bit_buckets[b].begin(), bit_buckets[b].end());
        }
        std::sort(result.begin(), result.end(), [](const FloodCheckEvent &e1, const FloodCheckEvent &e2) {
            return e1.time < e2.time;
        });
        return result;
    }

    std::string str() const;

    /// Clear all remaining events from the queue
    void clear();

    /// Clear the queue and set cur_time = 0
    void reset();
};

template <bool use_validation>
std::ostream &operator<<(std::ostream &out, radix_heap_queue<use_validation> q) {
    out << "bit_bucket_queue {\n";
    out << "    cur_time=" << q.cur_time << "\n";
    for (size_t b = 0; b < q.bit_buckets.size(); b++) {
        auto copy = q.bit_buckets[b];
        if (!copy.empty()) {
            out << "    bucket[" << b << "] {\n";
            std::sort(copy.begin(), copy.end(), [](const FloodCheckEvent &e1, const FloodCheckEvent &e2) {
                return e1.time < e2.time;
            });
            for (auto &e : copy) {
                out << "        " << e << ",\n";
            }
            out << "    }\n";
        }
    }
    out << "}";
    return out;
}

template <bool use_validation>
std::string radix_heap_queue<use_validation>::str() const {
    std::stringstream ss;
    ss << *this;
    return ss.str();
}

template <bool use_validation>
void radix_heap_queue<use_validation>::clear() {
    const auto clear_bucket = [&](size_t b) {
        auto& bucket = bit_buckets[b];
#if BB_ENABLE_PROFILE
        if (beyond_blossom::profile::stats_enabled()) {
            for (const auto &event : bucket) {
                beyond_blossom::profile::record_queue_remove(profile_queue_kind_for_event(event.tentative_event_type));
            }
        }
#endif
        _num_enqueued -= bucket.size();
        bucket.clear();
    };
    clear_bucket(0);
    while (nonempty_high_buckets) {
        const size_t b = std::countr_zero(nonempty_high_buckets);
        nonempty_high_buckets &= nonempty_high_buckets - 1;
        clear_bucket(b);
    }
}

template <bool use_validation>
void radix_heap_queue<use_validation>::reset() {
    clear();
    cur_time = 0;
}

}  // namespace beyond_blossom

#endif  // BEYOND_BLOSSOM2_BUCKET_QUEUE_H
