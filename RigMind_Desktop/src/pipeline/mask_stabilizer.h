// Display-only stabiliser for the IDSS masks and boxes: what is drawn on the video, never what the alarm logic sees.
//
// A segmentation model drops an object for a frame or two and changes its outline a little every frame, which shows as masks that
// blink and shimmer. For each camera this keeps a short memory per object (matched by class and position):
//   - an object that disappears keeps being drawn for `hold_ms` (default 500 ms) at its last smoothed shape,
//   - a returning or moving object is matched to its memory (same class, nearest centre) and its box and outline ease towards the
//     new values (`alpha`, default 0.55) instead of jumping,
//   - outlines are resampled to a fixed number of points by arc length and aligned to the previous outline, so a different starting
//     vertex or vertex count between frames does not make the shape spin or jitter,
//   - a new object appears at once (no delay) and an object gone longer than the hold time disappears.
// The EWS facts, person tracking and PPE check still use the raw detections (they are unsmoothed by design).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pipeline/draw_types.h"

namespace rigmind {

class MaskStabilizer {
public:
    struct Config {
        std::int64_t hold_ms = 500;   // how long a vanished object stays on screen
        float alpha = 0.55f;          // weight of the new frame (1 = no smoothing)
        int samples = 48;             // points per smoothed outline
        float match_frac = 0.15f;     // max centre distance for a match, as a share of the frame width
    };

    MaskStabilizer() = default;
    explicit MaskStabilizer(Config config) : cfg_(config) {}

    // detections: this frame's items (label, box, optional polygon). Returns what to draw: every live object smoothed, plus the
    // objects held over from the last `hold_ms`. Order: detections first, then held objects.
    std::vector<DrawItem> update(std::int64_t ts_ms, const std::vector<DrawItem>& detections, int frame_w) {
        const float max_dist = cfg_.match_frac * static_cast<float>(std::max(frame_w, 1));
        std::vector<bool> used(tracks_.size(), false);
        std::vector<DrawItem> out;
        for (const DrawItem& d : detections) {
            const float cx = (d.box[0] + d.box[2]) / 2.0f, cy = (d.box[1] + d.box[3]) / 2.0f;
            int best = -1;
            float best_d = max_dist;
            for (std::size_t i = 0; i < tracks_.size(); ++i) {
                if (used[i] || tracks_[i].label != d.label) continue;
                const float dist = std::hypot(tracks_[i].cx - cx, tracks_[i].cy - cy);
                if (dist < best_d) { best_d = dist; best = static_cast<int>(i); }
            }
            if (best < 0) {
                Track t;
                t.label = d.label;
                for (int k = 0; k < 4; ++k) t.box[k] = static_cast<float>(d.box[k]);
                t.poly = resample(d.polygon, cfg_.samples);
                t.item = d;
                t.cx = cx;
                t.cy = cy;
                t.seen_ms = ts_ms;
                tracks_.push_back(std::move(t));
                used.push_back(true);
                out.push_back(d); // new: drawn exactly as detected, at once
                continue;
            }
            Track& t = tracks_[static_cast<std::size_t>(best)];
            used[static_cast<std::size_t>(best)] = true;
            for (int k = 0; k < 4; ++k) t.box[k] += cfg_.alpha * (static_cast<float>(d.box[k]) - t.box[k]);
            if (!d.polygon.empty()) {
                const auto fresh = align(resample(d.polygon, cfg_.samples), t.poly);
                if (t.poly.empty()) t.poly = fresh;
                else for (std::size_t i = 0; i < t.poly.size(); ++i) {
                    t.poly[i].first += cfg_.alpha * (fresh[i].first - t.poly[i].first);
                    t.poly[i].second += cfg_.alpha * (fresh[i].second - t.poly[i].second);
                }
            }
            t.cx = (t.box[0] + t.box[2]) / 2.0f;
            t.cy = (t.box[1] + t.box[3]) / 2.0f;
            t.seen_ms = ts_ms;
            t.item = d;
            out.push_back(materialise(t, d));
        }
        // held objects and forgetting
        std::vector<Track> keep;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            Track& t = tracks_[i];
            if (ts_ms - t.seen_ms > cfg_.hold_ms) continue; // gone for good
            if (!used[i]) {
                DrawItem held = materialise(t, t.item);
                held.held = false; // drawn as a normal mask: that is the point (no blinking)
                out.push_back(std::move(held));
            }
            keep.push_back(std::move(t));
        }
        tracks_ = std::move(keep);
        return out;
    }

    void reset() { tracks_.clear(); }
    std::size_t tracked() const { return tracks_.size(); }

    // Fixed number of points around a closed outline, evenly spaced by arc length.
    static std::vector<std::pair<float, float>> resample(const std::vector<DrawPoint>& poly, int n) {
        std::vector<std::pair<float, float>> out;
        if (poly.size() < 3 || n < 3) return out;
        std::vector<float> cum(poly.size() + 1, 0.0f);
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const DrawPoint& a = poly[i];
            const DrawPoint& b = poly[(i + 1) % poly.size()];
            cum[i + 1] = cum[i] + std::hypot(static_cast<float>(b.x - a.x), static_cast<float>(b.y - a.y));
        }
        const float total = cum.back();
        if (total <= 0.0f) return out;
        std::size_t seg = 0;
        for (int k = 0; k < n; ++k) {
            const float at = total * static_cast<float>(k) / static_cast<float>(n);
            while (seg + 1 < poly.size() && cum[seg + 1] < at) ++seg;
            const DrawPoint& a = poly[seg];
            const DrawPoint& b = poly[(seg + 1) % poly.size()];
            const float len = cum[seg + 1] - cum[seg];
            const float f = len > 0.0f ? (at - cum[seg]) / len : 0.0f;
            out.emplace_back(static_cast<float>(a.x) + f * static_cast<float>(b.x - a.x), static_cast<float>(a.y) + f * static_cast<float>(b.y - a.y));
        }
        return out;
    }

private:
    struct Track {
        std::string label;
        float box[4] = {0, 0, 0, 0};
        float cx = 0, cy = 0;
        std::vector<std::pair<float, float>> poly; // smoothed outline, `samples` points
        DrawItem item;                              // the last detection (flags such as in_roi)
        std::int64_t seen_ms = 0;
    };

    static float signed_area(const std::vector<std::pair<float, float>>& p) {
        float a = 0;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const auto& u = p[i];
            const auto& v = p[(i + 1) % p.size()];
            a += u.first * v.second - v.first * u.second;
        }
        return a / 2.0f;
    }

    // Same winding as `ref` and the start vertex that lies nearest to it, so frame to frame the points correspond.
    static std::vector<std::pair<float, float>> align(std::vector<std::pair<float, float>> fresh, const std::vector<std::pair<float, float>>& ref) {
        if (fresh.size() != ref.size() || fresh.empty()) return fresh;
        if ((signed_area(fresh) < 0) != (signed_area(ref) < 0)) std::reverse(fresh.begin(), fresh.end());
        std::size_t best_shift = 0;
        float best = 1e30f;
        const std::size_t n = fresh.size();
        for (std::size_t shift = 0; shift < n; ++shift) {
            float sum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const auto& a = fresh[(i + shift) % n];
                sum += std::hypot(a.first - ref[i].first, a.second - ref[i].second);
            }
            if (sum < best) { best = sum; best_shift = shift; }
        }
        std::vector<std::pair<float, float>> out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = fresh[(i + best_shift) % n];
        return out;
    }

    DrawItem materialise(const Track& t, const DrawItem& like) const {
        DrawItem d = like;
        for (int k = 0; k < 4; ++k) d.box[k] = static_cast<int>(std::lround(t.box[k]));
        d.polygon.clear();
        for (const auto& p : t.poly) d.polygon.push_back({static_cast<int>(std::lround(p.first)), static_cast<int>(std::lround(p.second))});
        return d;
    }

    Config cfg_;
    std::vector<Track> tracks_;
};

} // namespace rigmind
