// Copyright 2022 PyMatching Contributors
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

#include "search_flooder.h"

#include <limits>

namespace pm::structured {

SearchFlooder::SearchFlooder() : target_type(NO_TARGET) {
}

SearchFlooder::SearchFlooder(SearchGraph graph) : graph(std::move(graph)), target_type(NO_TARGET) {
}

std::pair<size_t, pm::cumulative_time_int> SearchFlooder::find_next_event_at_node_returning_neighbor_index_and_time(
    const SearchDetectorNode &detector_node) const {
    return detector_node.grid_component->search_next(
        *detector_node.grid_component, detector_node, queue.cur_time, target_type == BOUNDARY);
}

void SearchFlooder::reschedule_events_at_search_detector_node(SearchDetectorNode &detector_node) {
    auto x = find_next_event_at_node_returning_neighbor_index_and_time(detector_node);
    if (x.first == SIZE_MAX) {
        detector_node.node_event_tracker.set_no_desired_event();
    } else {
        detector_node.node_event_tracker.set_desired_event({&detector_node, cyclic_time_int{x.second}}, queue);
    }
}

void SearchFlooder::do_search_starting_at_empty_search_detector_node(SearchDetectorNode *src) {
    src->reached_from_source = src;
    src->index_of_predecessor = SIZE_MAX;
    src->distance_from_source = 0;
    reached_nodes.push_back(src);
    reschedule_events_at_search_detector_node(*src);
}

void SearchFlooder::do_search_exploring_empty_detector_node(
    SearchDetectorNode &empty_node, size_t empty_to_from_index) {
    auto from_node = empty_node.neighbor(empty_to_from_index);
    empty_node.reached_from_source = from_node->reached_from_source;
    empty_node.index_of_predecessor = empty_to_from_index;
    empty_node.distance_from_source = empty_node.edge_weight(empty_to_from_index) + from_node->distance_from_source;
    reached_nodes.push_back(&empty_node);
    reschedule_events_at_search_detector_node(empty_node);
}

SearchGraphEdge SearchFlooder::do_look_at_node_event(SearchDetectorNode &node) {
    auto next = find_next_event_at_node_returning_neighbor_index_and_time(node);
    if (next.second == queue.cur_time) {
        auto dst = node.neighbor(next.first);
        if (!dst) {
            // Found boundary (and must have target_type == BOUNDARY). Return collision edge for search to terminate.
            return {&node, next.first};
        } else if (node.reached_from_source && !dst->reached_from_source) {
            do_search_exploring_empty_detector_node(*dst, next.first ^ 1);
            // Need to revisit node immediately, since the search hasn't finished and other events may need to be
            // handled along other adjacent edges.
            node.node_event_tracker.set_desired_event(
                {
                    &node,
                    cyclic_time_int{queue.cur_time},
                },
                queue);
            return {nullptr, SIZE_MAX};
        } else {
            return {&node, next.first};
        }
    } else if (next.first != SIZE_MAX) {
        // Need to revisit this node, but at a later time
        node.node_event_tracker.set_desired_event(
            {
                &node,
                cyclic_time_int{next.second},
            },
            queue);
    }
    return {nullptr, SIZE_MAX};
}

SearchGraphEdge SearchFlooder::run_until_collision(SearchDetectorNode *src, SearchDetectorNode *dst) {
    if (!dst) {
        target_type = BOUNDARY;
    } else {
        target_type = DETECTOR_NODE;
        do_search_starting_at_empty_search_detector_node(dst);
    }
    do_search_starting_at_empty_search_detector_node(src);

    while (!queue.empty()) {
        FloodCheckEvent ev = queue.dequeue();
        if (ev.data_look_at_search_node->node_event_tracker.dequeue_decision(ev, queue)) {
            auto collision_edge = do_look_at_node_event(*ev.data_look_at_search_node);
            if (collision_edge.detector_node) {
                return collision_edge;
            }
        }
    }
    return {nullptr, SIZE_MAX};
}

void SearchFlooder::reset_graph() {
    for (auto &detector_node : reached_nodes)
        detector_node->reset();
    reached_nodes.clear();
}

void SearchFlooder::reset() {
    reset_graph();
    queue.reset();
}

SearchFlooder::SearchFlooder(SearchFlooder &&other) noexcept
    : graph(std::move(other.graph)),
      queue(std::move(other.queue)),
      reached_nodes(std::move(other.reached_nodes)),
      target_type(other.target_type) {
}

}  // namespace pm::structured
