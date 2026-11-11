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

#include "beyond_blossom/structured/diagram/mwpm_diagram.h"

#include <fstream>

namespace beyond_blossom::structured {

std::pair<std::vector<std::pair<float, float>>, std::vector<std::pair<float, float>>> pick_coords_for_drawing_from_dem(
    const stim::DetectorErrorModel &dem, float pixels_per_unit_length) {
    size_t n = dem.count_detectors();
    if (n == 0) {
        return {{}, {}};
    }
    std::set<uint64_t> all_dets;
    for (uint64_t k = 0; k < n; k++) {
        all_dets.insert(k);
    }
    auto coords_from_dem = dem.get_detector_coordinates(all_dets);
    std::vector<std::pair<float, float>> coords;
    for (uint64_t k = 0; k < n; k++) {
        auto p = coords_from_dem.find(k);
        if (p == coords_from_dem.end() || p->second.empty()) {
            coords.push_back({k, 0});
            continue;
        }
        auto &cs = p->second;
        if (cs.size() == 1) {
            coords.push_back({cs[0], 0});
        } else {
            coords.push_back({cs[0], cs[1]});
        }

        // Do an arbitrary orthographic projection of the other axes.
        double s = 1;
        for (size_t d = 2; d < cs.size(); d++) {
            s *= 0.66;
            coords.back().first += cs[d] * s;
            coords.back().second += cs[d] * s / (d + 1);
        }
    }

    float avg_x = 0;
    float avg_y = 0;
    for (const auto &c : coords) {
        avg_x += c.first;
        avg_y += c.second;
    }
    avg_x /= n;
    avg_y /= n;
    std::vector<std::pair<float, float>> boundary_coords;
    for (const auto &c : coords) {
        auto dx = c.first - avg_x;
        auto dy = c.second - avg_y;
        auto d = sqrtf(dx * dx + dy * dy);
        if (d < 1e-5) {
            dx = 11;
            dy = 7;
            d = sqrtf(dx * dx + dy * dy);
        }
        dx /= d;
        dy /= d;
        dx *= 5;
        dy *= 5;
        boundary_coords.push_back({c.first + dx, c.second + dy});
    }
    float min_x = INFINITY;
    float min_y = INFINITY;
    for (size_t k = 0; k < n; k++) {
        min_x = std::min(min_x, std::min(coords[k].first, boundary_coords[k].first));
        min_y = std::min(min_y, std::min(coords[k].second, boundary_coords[k].second));
    }
    for (size_t k = 0; k < n; k++) {
        coords[k].first -= min_x;
        boundary_coords[k].first -= min_x;
        coords[k].second -= min_y;
        boundary_coords[k].second -= min_y;
    }
    for (size_t k = 0; k < n; k++) {
        coords[k].first *= pixels_per_unit_length;
        coords[k].second *= pixels_per_unit_length;
        boundary_coords[k].first *= pixels_per_unit_length;
        boundary_coords[k].second *= pixels_per_unit_length;

        coords[k].first += 4;
        coords[k].second += 4;
        boundary_coords[k].first += 4;
        boundary_coords[k].second += 4;
    }

    return {coords, boundary_coords};
}

/// Helper class for drawing frames.
struct StateHelper {
    const Mwpm &mwpm;
    const cumulative_time_int &t;
    const std::vector<std::pair<float, float>> &coords;
    const std::vector<std::pair<float, float>> &boundary_coords;
    std::ostream &out;

    std::pair<float, float> neighbor_coords(const DetectorNode &src, const DetectorNode *dst) {
        size_t k = mwpm.flooder.graph.node_id(&src);
        if (dst != nullptr) {
            size_t k2 = mwpm.flooder.graph.node_id(dst);
            return coords[k2];
        } else {
            return {boundary_coords[k].first, boundary_coords[k].second};
        }
    };

    std::pair<float, float> lerp_pos(const DetectorNode &src, size_t local_neighbor_index, float ratio) {
        if (ratio > 1) {
            ratio = 1;
        }
        auto c = coords[mwpm.flooder.graph.node_id(&src)];
        auto nc = neighbor_coords(src, src.neighbor(local_neighbor_index));
        return {nc.first * ratio + c.first * (1 - ratio), nc.second * ratio + c.second * (1 - ratio)};
    }

    std::vector<std::vector<std::pair<float, float>>> approximate_region_polygons(GraphFillRegion *region) {
        std::map<DetectorNode *, std::set<std::pair<float, float>>> points_by_source;
        region->do_op_for_each_node_in_total_area([&](const DetectorNode *n) {
            auto &perimeter = points_by_source[n->reached_from_source];
            if (n->compute_local_radius_at_time_bounded_by_region(t, *region) == 0) {
                perimeter.insert(coords[mwpm.flooder.graph.node_id(n)]);
                return;
            }

            for (size_t nk = 0; nk < n->edge_slots(); nk++) {
                if (!n->has_edge(nk))
                    continue;
                auto w = n->edge_weight(nk);
                auto r = n->compute_stitch_radius_at_time_bounded_by_region_towards_neighbor(t, *region, nk);
                if (!r.has_value()) {
                    continue;
                }
                float ratio = w == 0 ? 0 : r.value() / w;
                perimeter.insert(lerp_pos(*n, nk, ratio));
            }
        });

        std::vector<std::vector<std::pair<float, float>>> result;
        for (const auto &b : points_by_source) {
            if (b.second.empty()) {
                continue;
            }
            std::vector<std::pair<float, float>> p;
            p.insert(p.end(), b.second.begin(), b.second.end());
            float cx = 0;
            float cy = 0;
            for (const auto &e : p) {
                cx += e.first;
                cy += e.second;
            }
            cx /= p.size();
            cy /= p.size();
            std::sort(p.begin(), p.end(), [&](std::pair<float, float> a, std::pair<float, float> b) -> bool {
                return atan2f(a.second - cy, a.first - cx) < atan2f(b.second - cy, b.first - cx);
            });
            result.push_back(p);
        }
        return result;
    }

    void fill_region(GraphFillRegion *region, const std::string &fill, const char *stroke) {
        for (const auto &poly : approximate_region_polygons(region)) {
            out << " <path d=\"";
            for (size_t k = 0; k < poly.size(); k++) {
                out << (k == 0 ? 'M' : 'L');
                out << poly[k].first << " " << poly[k].second << " ";
            }
            out << " z\" stroke=\"" << stroke << "\" fill=\"" << fill << "\"/>\n";
        }
    }

    void draw_detector_graph_edges() {
        for (size_t k = 0; k < mwpm.flooder.graph.num_nodes; k++) {
            const auto &n = mwpm.flooder.graph.node(k);
            for (size_t h = 0; h < n.edge_slots(); h++) {
                if (!n.has_edge(h))
                    continue;
                const auto *n2_ptr = n.neighbor(h);
                auto nc = neighbor_coords(n, n2_ptr);
                if (n2_ptr != nullptr) {
                    size_t k2 = mwpm.flooder.graph.node_id(n2_ptr);
                    if (k2 > k) {
                        out << " <line x1=\"" << coords[k].first << "\" x2=\"" << nc.first << "\" y1=\""
                            << coords[k].second << "\" y2=\"" << nc.second << "\" stroke=\"#CCC\"/>\n";
                    }
                } else {
                    out << " <line x1=\"" << coords[k].first << "\" x2=\"" << nc.first << "\" y1=\"" << coords[k].second
                        << "\" y2=\"" << nc.second << "\" stroke-dasharray=\"4 2"
                        << "\" stroke=\"#FAA\"/>\n";
                }
            }
        }
    }
    void draw_unexcited_detector_nodes() {
        for (size_t k = 0; k < mwpm.flooder.graph.num_nodes; k++) {
            const auto &n = mwpm.flooder.graph.node(k);
            if (n.reached_from_source != &n) {
                out << " <circle cx=\"" << coords[k].first << "\" cy=\"" << coords[k].second << "\" r=\"" << 3
                    << "\" stroke=\"none"
                    << "\" fill=\"#CCC\"/>\n";
            }
        }
    }

    std::set<GraphFillRegion *> find_all_regions() {
        std::set<GraphFillRegion *> regions;
        for (size_t k = 0; k < mwpm.flooder.graph.num_nodes; k++) {
            const auto &n = mwpm.flooder.graph.node(k);
            GraphFillRegion *r = n.owner_region();
            while (r != nullptr) {
                regions.insert(r);
                r = r->parent_region();
            }
        }
        return regions;
    }

    std::set<AltTreeNode *> find_all_alt_tree_nodes() {
        std::set<AltTreeNode *> tree_nodes;
        for (auto *r : find_all_regions()) {
            if (r->alt_tree_node != nullptr) {
                tree_nodes.insert(r->alt_tree_node);
            }
        }
        return tree_nodes;
    }

    void draw_detection_events() {
        for (size_t k = 0; k < mwpm.flooder.graph.num_nodes; k++) {
            const auto &n = mwpm.flooder.graph.node(k);
            if (n.reached_from_source == &n) {
                out << " <circle cx=\"" << coords[k].first << "\" cy=\"" << coords[k].second << "\" r=\"" << 3
                    << "\" stroke=\"none"
                    << "\" fill=\"red\"/>\n";
            }
        }
    }

    void draw_pending_collisions() {
        for (size_t k = 0; k < mwpm.flooder.graph.num_nodes; k++) {
            const auto &n = mwpm.flooder.graph.node(k);
            if (n.top_region() == nullptr) {
                continue;
            }
            auto ev = mwpm.flooder.find_next_event_at_node_returning_neighbor_index_and_time(n);
            if (ev.first != SIZE_MAX && ev.second == t) {
                auto r1 = n.local_radius();
                auto m = n.neighbor(ev.first);
                auto r2 = m == nullptr ? VaryingCT{0} : m->local_radius();
                if (!r1.colliding_with(r2)) {
                    continue;
                }
                if (r1.get_distance_at_time(t) + r2.get_distance_at_time(t) != n.edge_weight(ev.first)) {
                    continue;
                }
                if (m != nullptr && (m->top_region() == nullptr ||
                                     m->top_region() == n.top_region())) {
                    continue;
                }
                auto col =
                    lerp_pos(n, ev.first, r1.get_distance_at_time(t) / std::max(1.0f, (float)n.edge_weight(ev.first)));
                out << " <circle cx=\"" << col.first << "\" cy=\"" << col.second << "\" r=\"" << 3
                    << "\" stroke=\"black"
                    << "\" fill=\"orange\"/>\n";
            }
        }
    }

    void draw_match_edges() {
        for (auto *r : find_all_regions()) {
            if (r->blossom_parent_top == r->region_id && r->radius_value().is_frozen()) {
                size_t k1 = mwpm.flooder.graph.node_id(r->match.edge.loc_from);
                if (r->match.edge.loc_to == nullptr) {
                    out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << boundary_coords[k1].first << "\" y1=\""
                        << coords[k1].second << "\" y2=\"" << boundary_coords[k1].second << "\" stroke-width=\"" << 4
                        << "\" stroke=\"#000\"/>\n";
                } else {
                    size_t k2 = mwpm.flooder.graph.node_id(r->match.edge.loc_to);
                    out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << coords[k2].first << "\" y1=\""
                        << coords[k1].second << "\" y2=\"" << coords[k2].second << "\" stroke-width=\"" << 4
                        << "\" stroke=\"#000\"/>\n";
                }
            }
        }
    }

    void draw_alt_tree_edges() {
        for (auto *tn : find_all_alt_tree_nodes()) {
            if (tn->inner_region != NO_REGION) {
                size_t k1 = mwpm.flooder.graph.node_id(tn->inner_to_outer_edge.loc_from);
                size_t k2 = mwpm.flooder.graph.node_id(tn->inner_to_outer_edge.loc_to);
                out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << coords[k2].first << "\" y1=\""
                    << coords[k1].second << "\" y2=\"" << coords[k2].second << "\" stroke=\"#000\"/>\n";
            }
            for (size_t k = 0; k < tn->children.size(); k++) {
                size_t k1 = mwpm.flooder.graph.node_id(tn->children[k].edge.loc_from);
                size_t k2 = mwpm.flooder.graph.node_id(tn->children[k].edge.loc_to);
                out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << coords[k2].first << "\" y1=\""
                    << coords[k1].second << "\" y2=\"" << coords[k2].second << "\" stroke-dasharray=\"4 2"
                    << "\" stroke=\"#000\"/>\n";
            }
        }
    }

    void draw_blossom_edges() {
        for (auto *r : find_all_regions()) {
            for (const auto &e : r->blossom_children) {
                size_t k1 = mwpm.flooder.graph.node_id(e.edge.loc_from);
                size_t k2 = mwpm.flooder.graph.node_id(e.edge.loc_to);
                out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << coords[k2].first << "\" y1=\""
                    << coords[k1].second << "\" y2=\"" << coords[k2].second << "\" stroke-width=\"" << 4
                    << "\" stroke=\"#22F\"/>\n";
            }
        }
    }

    std::pair<size_t, size_t> span() const {
        float width = 0;
        float height = 0;
        for (const auto &c : coords) {
            width = std::max(width, c.first);
            height = std::max(height, c.second);
        }
        for (const auto &c : boundary_coords) {
            width = std::max(width, c.first);
            height = std::max(height, c.second);
        }
        return {(size_t)ceil(width / 4) * 4 + 4, (size_t)ceil(height / 4) * 4 + 4};
    }
};

void write_decoder_state_as_svg(
    const std::vector<std::pair<float, float>> &coords,
    const std::vector<std::pair<float, float>> &boundary_coords,
    const Mwpm &mwpm,
    MwpmEvent focused_event,
    std::ostream &out) {
    // Rendering inspects physical node/source fields, unlike the matching
    // kernel. Resolve lazy singleton representations only on this cold path.
    for (size_t id = 0; id < mwpm.flooder.graph.num_nodes; ++id)
        mwpm.flooder.graph.node(id).materialize_seed_state();
    StateHelper s{
        mwpm,
        mwpm.flooder.queue.cur_time,
        coords,
        boundary_coords,
        out,
    };
    auto span = s.span();
    out << "<svg width=\"" << span.first << "\" height=\"" << span.second << "\" version=\""
        << "1.1"
        << "\" xmlns=\""
        << "http://www.w3.org/2000/svg"
        << "\">\n";

    out << " <path d=\"M 0 0 L " << span.first << " 0 L " << span.first << " " << span.second << " L 0 " << span.second
        << " Z\" fill=\"white\" stroke=\"#CCC\"/>\n";

    s.draw_detector_graph_edges();
    s.draw_unexcited_detector_nodes();

    std::set<GraphFillRegion *> regions = s.find_all_regions();

    for (auto *r : regions) {
        std::string color;
        if (mwpm.flooder.regions->get(r->blossom_parent_top)->radius_value().is_growing()) {
            color = "#FF0000";
        } else if (mwpm.flooder.regions->get(r->blossom_parent_top)->radius_value().is_frozen()) {
            if (mwpm.flooder.regions->get(r->blossom_parent_top)->match.edge.loc_to == nullptr) {
                color = "#002000";
            } else {
                color = "#00FF00";
            }
        } else {
            color = "#0000FF";
        }
        if (r->blossom_parent == NO_REGION) {
            color += "80";
        } else {
            color += "80";
        }
        s.fill_region(r, color, "none");
    }

    s.draw_detection_events();
    s.draw_pending_collisions();

    s.draw_alt_tree_edges();
    s.draw_blossom_edges();
    s.draw_match_edges();

    if (focused_event.event_type == REGION_HIT_REGION) {
        auto &e = focused_event.region_hit_region_event_data.edge;
        size_t k1 = mwpm.flooder.graph.node_id(e.loc_from);
        size_t k2 = mwpm.flooder.graph.node_id(e.loc_to);
        out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << coords[k2].first << "\" y1=\"" << coords[k1].second
            << "\" y2=\"" << coords[k2].second << "\" stroke-width=\"" << 15 << "\" stroke=\"#000\"/>\n";
        out << " <line x1=\"" << coords[k1].first << "\" x2=\"" << coords[k2].first << "\" y1=\"" << coords[k1].second
            << "\" y2=\"" << coords[k2].second << "\" stroke-width=\"" << 12 << "\" stroke=\"#F80\"/>\n";
    } else if (focused_event.event_type == REGION_HIT_BOUNDARY) {
        s.fill_region(mwpm.region_for(focused_event.region_hit_boundary_event_data.region), "#80800080", "#000");
    } else if (focused_event.event_type == BLOSSOM_SHATTER) {
        s.fill_region(mwpm.region_for(focused_event.blossom_shatter_event_data.blossom_region), "#FF800080", "#FF8000");
    }

    out << "</svg>\n";
}

void write_animated_decoding_svg_frames(
    Mwpm &mwpm,
    const std::vector<std::pair<float, float>> &coords,
    const std::vector<std::pair<float, float>> &boundary_coords,
    const std::vector<uint64_t> detection_events,
    const std::string &file_path_prefix,
    bool print_progress,
    size_t frames_per_mwpm_event,
    size_t hold_frames_at_start,
    size_t hold_frames_at_end,
    size_t max_growth_between_frames) {
    // Set up the decoder.
    mwpm.flooder.queue.cur_time = 0;
    for (auto &detection : detection_events) {
        mwpm.create_detection_event(&mwpm.flooder.graph.node(detection));
    }
    auto check_if_done = [&]() {
        for (auto &detection : detection_events) {
            if (!mwpm.flooder.graph.node(detection).top_region()->radius_value().is_frozen()) {
                return false;
            }
        }
        return true;
    };

    // Set up frame writing functionality.
    size_t frame_number = 0;
    auto output_frame = [&](MwpmEvent ev) {
        std::string frame_num_as_str = std::to_string(frame_number);
        while (frame_num_as_str.size() < 5) {
            frame_num_as_str.insert(frame_num_as_str.begin(), '0');
        }
        std::string frame_path = file_path_prefix;
        frame_path += "frame_";
        frame_path += frame_num_as_str;
        frame_path += ".svg";

        std::ofstream out_file;
        out_file.open(frame_path);
        if (out_file.fail()) {
            throw std::invalid_argument("Failed to open " + frame_path + " to write.");
        }
        write_decoder_state_as_svg(coords, boundary_coords, mwpm, ev, out_file);
        out_file.close();
        if (print_progress) {
            std::cerr << "wrote " << frame_path << "\n";
        }
        frame_number++;
    };

    // Show initial problem.
    for (size_t k = 0; k < hold_frames_at_start; k++) {
        output_frame(MwpmEvent::no_event());
    }

    // Run decoding.
    auto next_draw_time = (beyond_blossom::cumulative_time_int)max_growth_between_frames;
    bool just_drew_interesting_event = false;
    while (!check_if_done()) {
        // Advance time.
        auto flood_event = mwpm.flooder.queue.dequeue();

        // Show intermediate growth frames.
        while ((just_drew_interesting_event || max_growth_between_frames != 0) &&
               mwpm.flooder.queue.cur_time > next_draw_time) {
            auto t = mwpm.flooder.queue.cur_time;
            mwpm.flooder.queue.cur_time = next_draw_time;
            output_frame(MwpmEvent::no_event());
            mwpm.flooder.queue.cur_time = t;
            next_draw_time += (beyond_blossom::cumulative_time_int)max_growth_between_frames;
            just_drew_interesting_event = false;
        }

        // Check if something interesting happened.
        if (!mwpm.flooder.dequeue_decision(flood_event)) {
            continue;
        }
        auto mwpm_event = mwpm.flooder.process_tentative_event_returning_mwpm_event(flood_event);
        if (mwpm_event.event_type == NO_EVENT) {
            continue;
        }

        // Draw the interesting thing about to happen.
        for (size_t k = 0; k < frames_per_mwpm_event; k++) {
            output_frame(mwpm_event);
            next_draw_time = mwpm.flooder.queue.cur_time;
            just_drew_interesting_event = true;
        }

        // Move on to the state after the interesting thing.
        mwpm.process_event(mwpm_event);
    }

    // Show the solution.
    for (size_t k = 0; k < hold_frames_at_end; k++) {
        output_frame(MwpmEvent::no_event());
    }
}

}  // namespace beyond_blossom::structured
