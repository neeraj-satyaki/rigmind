// Zone based inference: when a camera has an enabled zone, only objects whose centre lies inside the zone count for the alarm
// facts, the person tracking and the PPE check. The platform always counts: it sets the picture scale and the ROI.
// Header-only, no OpenCV. Zone points are percent of the frame (0-100, x to the right, y downward), as the zone editor saves them.
#pragma once

#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace rigmind {

struct Zone {
    bool enabled = false;
    std::vector<std::pair<float, float>> points; // percent of the frame
};

// Ray casting: is (x, y) inside the polygon? Points on an edge count as inside.
inline bool point_in_polygon(float x, float y, const std::vector<std::pair<float, float>>& poly) {
    const std::size_t n = poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = poly[i].first, yi = poly[i].second, xj = poly[j].first, yj = poly[j].second;
        // on the edge
        const float cross = (x - xi) * (yj - yi) - (y - yi) * (xj - xi);
        if (std::fabs(cross) < 1e-4f && x >= std::fmin(xi, xj) - 1e-4f && x <= std::fmax(xi, xj) + 1e-4f && y >= std::fmin(yi, yj) - 1e-4f &&
            y <= std::fmax(yi, yj) + 1e-4f)
            return true;
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
    }
    return inside;
}

// {"enabled":bool,"points":[[x,y],...]} (extra keys ignored). A zone with fewer than 3 valid points is never enabled.
inline Zone parse_zone(const nlohmann::json& j) {
    Zone z;
    if (!j.is_object() || !j.contains("points") || !j["points"].is_array()) return z;
    for (const auto& p : j["points"]) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) return Zone{};
        const float x = p[0].get<float>(), y = p[1].get<float>();
        if (!std::isfinite(x) || !std::isfinite(y) || x < -1 || x > 101 || y < -1 || y > 101) return Zone{};
        z.points.emplace_back(x, y);
    }
    z.enabled = j.value("enabled", false) && z.points.size() >= 3;
    return z;
}

// True when a box (x1,y1,x2,y2 in pixels of a w x h frame) counts: no zone, or its centre is inside the zone.
inline bool box_in_zone(const Zone& zone, float x1, float y1, float x2, float y2, int w, int h) {
    if (!zone.enabled || w <= 0 || h <= 0) return true;
    const float cx = (x1 + x2) / 2 * 100.0f / static_cast<float>(w), cy = (y1 + y2) / 2 * 100.0f / static_cast<float>(h);
    return point_in_polygon(cx, cy, zone.points);
}

// Per-camera zones, set from the host and read by the detection workers.
class ZoneBook {
public:
    void set(const std::string& cam, Zone zone) {
        std::lock_guard<std::mutex> lock(mutex_);
        zones_[cam] = std::move(zone);
    }
    Zone get(const std::string& cam) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = zones_.find(cam);
        return it == zones_.end() ? Zone{} : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Zone> zones_;
};

} // namespace rigmind
