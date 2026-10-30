// Scene memory for the plain-language summaries: for every detected object (both models, every class) the latest box,
// centroid, mask contour and a short series of centroid points over time. Header-only, no OpenCV.
//
// The detection workers call update() after each frame; the pipeline reads snapshot() about once a second and sends it to the
// host as the "scene" event, where the summary prompt is built from it (slm/scene_text.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace rigmind {

struct SceneObs {
    std::string label;                          // class name
    int track_id = 0;                           // tracker id, 0 when the model has no tracker for this class
    float box[4] = {0, 0, 0, 0};                // x1, y1, x2, y2 in frame pixels
    std::vector<std::pair<int, int>> contour;   // mask outline, empty for box-only classes
};

class SceneTrail {
public:
    static constexpr std::int64_t kSampleMs = 400;   // minimum spacing of centroid samples
    static constexpr std::int64_t kForgetMs = 3000;  // a trail not seen for this long is dropped
    static constexpr std::int64_t kFreshMs = 1500;   // only trails seen this recently are reported
    static constexpr std::size_t kKeepPoints = 16;
    static constexpr float kMatchPx = 300.0f;        // nearest-centroid match limit for objects without a tracker id

    void update(const std::string& model, std::int64_t ts_ms, const std::vector<SceneObs>& observations) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<bool> taken(trails_.size(), false);
        for (const SceneObs& o : observations) {
            const float cx = (o.box[0] + o.box[2]) / 2, cy = (o.box[1] + o.box[3]) / 2;
            int best = -1;
            float best_d = kMatchPx;
            for (std::size_t i = 0; i < trails_.size(); ++i) {
                const Trail& t = trails_[i];
                if (taken[i] || t.model != model || t.label != o.label) continue;
                if (o.track_id > 0 || t.tracker) {
                    if (o.track_id > 0 && t.tracker && t.id == o.track_id) { best = static_cast<int>(i); break; }
                    continue;
                }
                const float d = std::hypot(t.cx - cx, t.cy - cy);
                if (d < best_d) { best_d = d; best = static_cast<int>(i); }
            }
            if (best < 0) {
                Trail t;
                t.model = model;
                t.label = o.label;
                t.tracker = o.track_id > 0;
                t.id = t.tracker ? o.track_id : ++next_id_[model + "/" + o.label];
                trails_.push_back(std::move(t));
                taken.push_back(true);
                best = static_cast<int>(trails_.size()) - 1;
            }
            taken[static_cast<std::size_t>(best)] = true;
            Trail& t = trails_[static_cast<std::size_t>(best)];
            for (int k = 0; k < 4; ++k) t.box[k] = o.box[k];
            t.cx = cx;
            t.cy = cy;
            t.contour = o.contour;
            t.seen_ms = ts_ms;
            if (t.points.empty() || ts_ms - t.points.back().t >= kSampleMs) {
                t.points.push_back({cx, cy, ts_ms});
                if (t.points.size() > kKeepPoints) t.points.erase(t.points.begin());
            }
        }
        trails_.erase(std::remove_if(trails_.begin(), trails_.end(),
                                     [&](const Trail& t) { return ts_ms - t.seen_ms > kForgetMs; }),
                      trails_.end());
    }

    // {"ev":"scene","cam","ts","w","h","objects":[{model,label,id,box,centroid,contour,trail}]}
    // contour: up to `contour_points` evenly spaced outline points; trail: up to `trail_points` centroid samples,
    // oldest first, each [x, y, ms_ago].
    nlohmann::json snapshot(const std::string& cam, std::int64_t now_ms, int w, int h, std::size_t contour_points = 16,
                            std::size_t trail_points = 8) const {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json objects = nlohmann::json::array();
        for (const Trail& t : trails_) {
            if (now_ms - t.seen_ms > kFreshMs) continue;
            nlohmann::json o = {{"model", t.model}, {"label", t.label}, {"id", t.id},
                                {"box", {std::lround(t.box[0]), std::lround(t.box[1]), std::lround(t.box[2]), std::lround(t.box[3])}},
                                {"centroid", {std::lround(t.cx), std::lround(t.cy)}}};
            nlohmann::json contour = nlohmann::json::array();
            for (std::size_t i : pick(t.contour.size(), contour_points)) contour.push_back({t.contour[i].first, t.contour[i].second});
            o["contour"] = contour;
            nlohmann::json trail = nlohmann::json::array();
            for (std::size_t i : pick(t.points.size(), trail_points)) {
                const Sample& s = t.points[i];
                trail.push_back({std::lround(s.x), std::lround(s.y), now_ms - s.t});
            }
            o["trail"] = trail;
            objects.push_back(std::move(o));
        }
        return {{"ev", "scene"}, {"cam", cam}, {"ts", now_ms}, {"w", w}, {"h", h}, {"objects", objects}};
    }

private:
    struct Sample { float x, y; std::int64_t t; };
    struct Trail {
        std::string model, label;
        int id = 0;
        bool tracker = false;
        float box[4] = {0, 0, 0, 0};
        float cx = 0, cy = 0;
        std::int64_t seen_ms = 0;
        std::vector<std::pair<int, int>> contour;
        std::vector<Sample> points;
    };

    // Up to `limit` indexes out of `n`, evenly spaced and always including the first and the last.
    static std::vector<std::size_t> pick(std::size_t n, std::size_t limit) {
        std::vector<std::size_t> out;
        if (n == 0 || limit == 0) return out;
        if (n <= limit) {
            for (std::size_t i = 0; i < n; ++i) out.push_back(i);
            return out;
        }
        if (limit == 1) return {n - 1};
        for (std::size_t k = 0; k < limit; ++k) out.push_back(k * (n - 1) / (limit - 1));
        return out;
    }

    mutable std::mutex mutex_;
    std::vector<Trail> trails_;
    std::map<std::string, int> next_id_;
};

} // namespace rigmind
