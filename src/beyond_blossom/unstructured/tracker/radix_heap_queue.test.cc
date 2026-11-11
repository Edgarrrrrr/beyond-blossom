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

#include "beyond_blossom/unstructured/tracker/radix_heap_queue.h"

#include <gtest/gtest.h>
#include <random>

using namespace beyond_blossom;

TEST(radix_heap_queue, basic_usage) {
    radix_heap_queue<true> q;
    ASSERT_EQ(q.size(), 0);
    ASSERT_EQ(q.cur_time, 0);

    ASSERT_EQ(q.dequeue(), FloodCheckEvent(cyclic_time_int{0}));

    q.enqueue(FloodCheckEvent(cyclic_time_int{9}));
    q.enqueue(FloodCheckEvent(cyclic_time_int{3}));
    ASSERT_EQ(q.size(), 2);
    ASSERT_EQ(q.cur_time, 0);

    ASSERT_EQ(q.dequeue(), FloodCheckEvent(cyclic_time_int{3}));
    ASSERT_EQ(q.dequeue(), FloodCheckEvent(cyclic_time_int{9}));
    ASSERT_EQ(q.size(), 0);
    ASSERT_EQ(q.cur_time, 9);

    ASSERT_EQ(q.dequeue(), FloodCheckEvent(cyclic_time_int{0}));
}

TEST(radix_heap_queue, sorts_fuzz) {
    radix_heap_queue<true> q;
    std::mt19937 rng(0);  // NOLINT(cert-msc51-cpp)

    std::vector<cyclic_time_int> s;
    for (size_t k = 0; k < 1000; k++) {
        auto v = cyclic_time_int{rng() & 0x00007FFF};
        s.push_back(v);
        q.enqueue(FloodCheckEvent(v));
    }
    std::sort(s.begin(), s.end());
    ASSERT_TRUE(q.satisfies_invariants());

    for (size_t k = 0; k < s.size(); k++) {
        auto t = q.dequeue().time;
        ASSERT_EQ(t, s[k]) << k;
    }

    ASSERT_EQ(q.dequeue(), FloodCheckEvent(cyclic_time_int{0}));
}

TEST(radix_heap_queue, bucket_for) {
    radix_heap_queue<true> q;
    q.cur_time = 17;
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{17}), 0);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{18}), 2);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{19}), 2);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{20}), 3);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{21}), 3);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{22}), 3);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{23}), 3);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{24}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{25}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{26}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{27}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{28}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{29}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{30}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{31}), 4);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{32}), 6);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{33}), 6);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{63}), 6);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{64}), 7);
    ASSERT_EQ(q.cur_bit_bucket_for(cyclic_time_int{65}), 7);
}

TEST(radix_heap_queue, wraparound_all_the_way_around) {
    std::priority_queue<int64_t> reference_queue;
    radix_heap_queue<true> actual_queue;
    for (size_t k = 0; k < 100; k++) {
        actual_queue.enqueue(FloodCheckEvent{cyclic_time_int{k}});
        reference_queue.push(-(int64_t)k);
    }

    std::mt19937 rng(0);  // NOLINT(cert-msc51-cpp)
    size_t n = 0;
    while (!reference_queue.empty()) {
        auto actual = (size_t)-reference_queue.top();
        auto t = (size_t)actual_queue.dequeue().time.widen_from_nearby_reference(actual_queue.cur_time);
        ASSERT_EQ(t, actual) << n;
        reference_queue.pop();
        n++;
        if (n < (1 << 17)) {
            t += rng() % 1000;
            actual_queue.enqueue(FloodCheckEvent{cyclic_time_int{t}});
            reference_queue.push(-(int64_t)t);
        }
    }
}

TEST(radix_heap_queue, clear) {
    radix_heap_queue<true> q;
    ASSERT_EQ(q.size(), 0);
    ASSERT_EQ(q.cur_time, 0);

    q.enqueue(FloodCheckEvent(cyclic_time_int{9}));
    q.enqueue(FloodCheckEvent(cyclic_time_int{3}));
    q.enqueue(FloodCheckEvent(cyclic_time_int{1000}));
    ASSERT_EQ(q.size(), 3);
    ASSERT_EQ(q.cur_time, 0);

    ASSERT_EQ(q.dequeue(), FloodCheckEvent(cyclic_time_int{3}));
    ASSERT_EQ(q.size(), 2);
    ASSERT_EQ(q.cur_time, 3);

    q.clear();
    ASSERT_EQ(q.size(), 0);
    ASSERT_EQ(q.cur_time, 3);

    for (size_t i = 0; i < q.bit_buckets.size() - 1; i++)
        ASSERT_EQ(q.bit_buckets[i].size(), 0);

    q.reset();
    ASSERT_EQ(q.cur_time, 0);
}

TEST(radix_heap_queue, sparse_bucket_bitmap_survives_wraparound_and_clear) {
    radix_heap_queue<true> q;
    q.cur_time = (int64_t{1} << 32) - 4;
    for (uint64_t delta : {uint64_t{0}, uint64_t{1}, uint64_t{8}, uint64_t{65536}})
        q.enqueue(FloodCheckEvent(cyclic_time_int{uint64_t(q.cur_time) + delta}));
    ASSERT_TRUE(q.satisfies_invariants());
    EXPECT_EQ(q.to_vector().size(), 4);
    EXPECT_NE(q.str().find("bucket[32]"), std::string::npos);
    for (size_t index = 0; index < 3; ++index) {
        q.dequeue();
        ASSERT_TRUE(q.satisfies_invariants());
    }
    q.clear();
    EXPECT_EQ(q.nonempty_high_buckets, 0);
    EXPECT_TRUE(q.satisfies_invariants());
    q.enqueue(FloodCheckEvent(cyclic_time_int{q.cur_time + 4096}));
    q.dequeue();
    EXPECT_EQ(q.nonempty_high_buckets, 0);
    EXPECT_TRUE(q.satisfies_invariants());
}
