// ByteTrack (Zhang et al., 2022) multi-object tracker, dependency free and unit tested alone.
//
// Used to remove flicker from the PPE boxes that are displayed and from the PPE presence check, and to give each IDSS
// 'derrickman' a stable id for the PPE check. It never invents a class the model does not detect: a track needs
// detections to be born and to stay alive; it only bridges short gaps and weak (low score) frames.
//
//   Kalman filter     constant velocity on (cx, cy, aspect = w/h, h) plus the four velocities; the standard ByteTrack /
//                     DeepSORT noise weights (position 1/20, velocity 1/160 of the box height). Output boxes are the
//                     filter state, so jitter is smoothed.
//   Association       cost = 1 - IoU (infinite across classes), Hungarian assignment, pairs above match_thresh rejected.
//   Stage 1           high score detections (>= high_thresh) against all tracks (tracked and lost).
//   Stage 2           the tracks still unmatched against LOW score detections (low_thresh .. high_thresh): weak boxes keep
//                     a track alive. (The paper uses only tracked tracks here; lost tracks are included on purpose so a
//                     weak box can also recover a track that was lost a frame ago. A tentative track only takes a low
//                     detection when it reaches new_track_thresh.)
//   New tracks        a detection left over with conf >= new_track_thresh starts a tentative track. (The paper starts
//                     tracks from high score detections only; PPE items rarely exceed 0.5, so the model's own conf is used.)
//                     Confirmed after min_hits consecutive hits, or at once when conf >= high_thresh. A tentative track that
//                     misses a frame is dropped.
//   Lost tracks       a confirmed track without a match is 'lost': kept for track_buffer_s (time, not frames) and drawn at
//                     the Kalman prediction (velocity damped) as held. Then removed.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "pipeline/ews_facts.h"

namespace rigmind {

struct TrackConfig {
    bool enabled = true;
    float high_thresh = 0.5f;
    float low_thresh = 0.15f;
    float new_track_thresh = 0.25f;
    float match_thresh = 0.8f;   // maximum IoU distance (1 - IoU) of an accepted match
    float track_buffer_s = 1.0f;
    int min_hits = 2;
};

struct TrackDet {
    int cls = 0;
    std::string label;
    float conf = 0;
    Box box;
};

struct TrackOut {
    int id = 0;
    int cls = 0;
    std::string label;
    float conf = 0;
    Box box;           // Kalman state
    bool held = false; // lost for now: drawn at the predicted box
    Box det;           // the detection box this track matched on THIS frame (the model's own localisation; empty when held)
    int lost_frames = 0; // consecutive frames without a match (0 when matched)
};

inline float box_iou(const Box& a, const Box& b) {
    const float ix = std::max(0.0f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    const float iy = std::max(0.0f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    const float inter = ix * iy;
    const float uni = a.width() * a.height() + b.width() * b.height() - inter;
    return uni > 0 ? inter / uni : 0.0f;
}

// Hungarian method (Kuhn-Munkres, potentials, O(n^2 m)) for a rectangular cost matrix. Returns for each row the column it is
// assigned to (-1: none). Minimises the total cost; every row gets a column when rows <= columns.
inline std::vector<int> hungarian(const std::vector<std::vector<double>>& a) {
    const int n = static_cast<int>(a.size());
    std::vector<int> result(static_cast<std::size_t>(n), -1);
    if (n == 0 || a[0].empty()) return result;
    const int m = static_cast<int>(a[0].size());
    const bool transposed = n > m;
    const int R = transposed ? m : n, C = transposed ? n : m;
    auto cost = [&](int i, int j) { return transposed ? a[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)] : a[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]; };
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> u(static_cast<std::size_t>(R) + 1, 0.0), v(static_cast<std::size_t>(C) + 1, 0.0);
    std::vector<int> p(static_cast<std::size_t>(C) + 1, 0), way(static_cast<std::size_t>(C) + 1, 0);
    for (int i = 1; i <= R; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(static_cast<std::size_t>(C) + 1, inf);
        std::vector<char> used(static_cast<std::size_t>(C) + 1, 0);
        do {
            used[static_cast<std::size_t>(j0)] = 1;
            const int i0 = p[static_cast<std::size_t>(j0)];
            double delta = inf;
            int j1 = 0;
            for (int j = 1; j <= C; ++j) {
                if (used[static_cast<std::size_t>(j)]) continue;
                const double cur = cost(i0 - 1, j - 1) - u[static_cast<std::size_t>(i0)] - v[static_cast<std::size_t>(j)];
                if (cur < minv[static_cast<std::size_t>(j)]) { minv[static_cast<std::size_t>(j)] = cur; way[static_cast<std::size_t>(j)] = j0; }
                if (minv[static_cast<std::size_t>(j)] < delta) { delta = minv[static_cast<std::size_t>(j)]; j1 = j; }
            }
            for (int j = 0; j <= C; ++j) {
                if (used[static_cast<std::size_t>(j)]) { u[static_cast<std::size_t>(p[static_cast<std::size_t>(j)])] += delta; v[static_cast<std::size_t>(j)] -= delta; }
                else minv[static_cast<std::size_t>(j)] -= delta;
            }
            j0 = j1;
        } while (p[static_cast<std::size_t>(j0)] != 0);
        do {
            const int j1 = way[static_cast<std::size_t>(j0)];
            p[static_cast<std::size_t>(j0)] = p[static_cast<std::size_t>(j1)];
            j0 = j1;
        } while (j0);
    }
    for (int j = 1; j <= C; ++j) {
        if (p[static_cast<std::size_t>(j)] == 0) continue;
        const int row = p[static_cast<std::size_t>(j)] - 1, col = j - 1;
        if (transposed) result[static_cast<std::size_t>(col)] = row; else result[static_cast<std::size_t>(row)] = col;
    }
    return result;
}

// Constant velocity Kalman filter on (cx, cy, aspect, height) and their velocities, one step per frame.
struct KalmanBox {
    using Vec = std::array<double, 8>;
    using Mat = std::array<std::array<double, 8>, 8>;
    static constexpr double kPos = 1.0 / 20, kVel = 1.0 / 160;

    Vec x{};
    Mat P{};

    static std::array<double, 4> measure(const Box& b) {
        const double h = std::max(1e-3f, b.height());
        return {b.cx(), b.cy(), b.width() / h, h};
    }
    void init(const Box& b) {
        const auto z = measure(b);
        x = {z[0], z[1], z[2], z[3], 0, 0, 0, 0};
        const double h = z[3];
        const double s[8] = {2 * kPos * h, 2 * kPos * h, 1e-2, 2 * kPos * h, 10 * kVel * h, 10 * kVel * h, 1e-5, 10 * kVel * h};
        for (auto& row : P) row.fill(0);
        for (int i = 0; i < 8; ++i) P[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = s[i] * s[i];
    }
    // `step` is the elapsed time in units of the reference frame interval (100 ms): the velocity moves the box by
    // velocity * step, so frames that arrive unevenly (bursts, skipped frames) do not distort the prediction.
    void predict(double velocity_keep = 1.0, double step = 1.0) {
        for (int i = 4; i < 8; ++i) x[static_cast<std::size_t>(i)] *= velocity_keep;
        const double h = std::max(1.0, x[3]);
        const double q[8] = {kPos * h, kPos * h, 1e-2, kPos * h, kVel * h, kVel * h, 1e-5, kVel * h};
        for (int i = 0; i < 4; ++i) x[static_cast<std::size_t>(i)] += x[static_cast<std::size_t>(i) + 4] * step;
        // P = F P F^T + Q with F = [I I; 0 I]
        Mat fp = P;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j) fp[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] += P[static_cast<std::size_t>(i) + 4][static_cast<std::size_t>(j)];
        Mat out = fp;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 8; ++i) out[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] += fp[static_cast<std::size_t>(i)][static_cast<std::size_t>(j) + 4];
        for (int i = 0; i < 8; ++i) out[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] += q[i] * q[i];
        P = out;
    }
    void update(const Box& b) {
        const auto z = measure(b);
        const double h = std::max(1.0, x[3]);
        const double r[4] = {kPos * h, kPos * h, 1e-1, kPos * h};
        double S[4][4], inv[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) S[i][j] = P[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] + (i == j ? r[i] * r[i] : 0.0);
        invert4(S, inv);
        double K[8][4];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) {
                double sum = 0;
                for (int k = 0; k < 4; ++k) sum += P[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] * inv[k][j];
                K[i][j] = sum;
            }
        double y[4];
        for (int i = 0; i < 4; ++i) y[i] = z[static_cast<std::size_t>(i)] - x[static_cast<std::size_t>(i)];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) x[static_cast<std::size_t>(i)] += K[i][j] * y[j];
        Mat next = P;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j) {
                double sum = 0;
                for (int k = 0; k < 4; ++k) sum += K[i][k] * P[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)];
                next[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] -= sum;
            }
        P = next;
    }
    Box box() const {
        const double h = std::max(1e-3, x[3]), w = x[2] * h;
        return {static_cast<float>(x[0] - w / 2), static_cast<float>(x[1] - h / 2), static_cast<float>(x[0] + w / 2), static_cast<float>(x[1] + h / 2)};
    }

private:
    static void invert4(double a[4][4], double out[4][4]) {
        double m[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c) {
            int piv = c;
            for (int r = c + 1; r < 4; ++r) if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
            if (std::fabs(m[piv][c]) < 1e-12) m[piv][c] = 1e-12;
            for (int j = 0; j < 8; ++j) std::swap(m[c][j], m[piv][j]);
            const double d = m[c][c];
            for (int j = 0; j < 8; ++j) m[c][j] /= d;
            for (int r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double f = m[r][c];
                for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
            }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out[i][j] = m[i][j + 4];
    }
};

class ByteTracker {
public:
    // One frame. `now_ms` is the frame's clock (the buffer is measured in it). `dets` should include everything down to
    // low_thresh. Returns the confirmed tracks (tracked this frame, or lost and held).
    std::vector<TrackOut> update(std::int64_t now_ms, const std::vector<TrackDet>& dets, const TrackConfig& cfg) {
        std::vector<TrackOut> out;
        if (!cfg.enabled) {
            tracks_.clear();
            for (const TrackDet& d : dets)
                if (d.conf >= cfg.new_track_thresh) out.push_back({0, d.cls, d.label, d.conf, d.box, false, d.box, 0});
            return out;
        }
        // Elapsed time since the previous frame in 100 ms units (clamped: a long gap must not fling the prediction).
        const double step = last_update_ms_ > 0 ? std::clamp(static_cast<double>(now_ms - last_update_ms_) / 100.0, 0.3, 4.0) : 1.0;
        last_update_ms_ = now_ms;
        for (Track& t : tracks_) t.kf.predict(t.lost ? 0.5 : 1.0, step);

        std::vector<int> high, low;
        for (std::size_t i = 0; i < dets.size(); ++i) {
            if (dets[i].conf >= cfg.high_thresh) high.push_back(static_cast<int>(i));
            else if (dets[i].conf >= cfg.low_thresh) low.push_back(static_cast<int>(i));
        }
        std::vector<char> track_done(tracks_.size(), 0), det_done(dets.size(), 0);

        // Stage 1: high score detections against every track.
        associate(dets, high, track_done, det_done, cfg, now_ms, /*low_stage=*/false);
        // Stage 2: what is left against the low score detections.
        associate(dets, low, track_done, det_done, cfg, now_ms, /*low_stage=*/true);

        // Unmatched tracks: tentative are dropped, confirmed become lost, lost expire after the buffer.
        const std::int64_t buffer_ms = static_cast<std::int64_t>(cfg.track_buffer_s * 1000.0f);
        std::vector<Track> next;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            Track& t = tracks_[i];
            if (!track_done[i]) {
                if (!t.confirmed) continue;
                t.lost = true;
                ++t.lost_frames;
                if (now_ms - t.last_ms > buffer_ms) continue;
            }
            next.push_back(std::move(t));
        }
        // New tracks from the detections nothing claimed.
        std::vector<int> order;
        for (std::size_t i = 0; i < dets.size(); ++i)
            if (!det_done[i] && dets[i].conf >= cfg.new_track_thresh) order.push_back(static_cast<int>(i));
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return dets[static_cast<std::size_t>(a)].conf > dets[static_cast<std::size_t>(b)].conf; });
        for (int i : order) {
            const TrackDet& d = dets[static_cast<std::size_t>(i)];
            Track t;
            t.id = ++next_id_;
            t.cls = d.cls;
            t.label = d.label;
            t.conf = d.conf;
            t.kf.init(d.box);
            t.det_box = d.box;
            t.hits = 1;
            t.last_ms = now_ms;
            t.confirmed = cfg.min_hits <= 1 || d.conf >= cfg.high_thresh;
            next.push_back(std::move(t));
        }
        tracks_ = std::move(next);
        for (const Track& t : tracks_)
            if (t.confirmed) out.push_back({t.id, t.cls, t.label, t.conf, t.kf.box(), t.lost, t.lost ? Box{} : t.det_box, t.lost ? t.lost_frames : 0});
        return out;
    }
    void reset() { tracks_.clear(); }
    int ids_issued() const { return next_id_; }

private:
    struct Track {
        int id = 0, cls = 0, hits = 0;
        std::string label;
        float conf = 0;
        KalmanBox kf;
        bool confirmed = false, lost = false;
        int lost_frames = 0;
        Box det_box;
        std::int64_t last_ms = 0;
    };

    void associate(const std::vector<TrackDet>& dets, const std::vector<int>& pool, std::vector<char>& track_done,
                   std::vector<char>& det_done, const TrackConfig& cfg, std::int64_t now_ms, bool low_stage) {
        std::vector<int> rows;
        for (std::size_t i = 0; i < tracks_.size(); ++i) if (!track_done[i]) rows.push_back(static_cast<int>(i));
        std::vector<int> cols;
        for (int d : pool) if (!det_done[static_cast<std::size_t>(d)]) cols.push_back(d);
        if (rows.empty() || cols.empty()) return;
        constexpr double kBig = 100.0;
        std::vector<std::vector<double>> cost(rows.size(), std::vector<double>(cols.size(), kBig));
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const Track& t = tracks_[static_cast<std::size_t>(rows[r])];
            const Box tb = t.kf.box();
            for (std::size_t c = 0; c < cols.size(); ++c) {
                const TrackDet& d = dets[static_cast<std::size_t>(cols[c])];
                if (d.cls != t.cls) continue;
                if (low_stage && !t.confirmed && d.conf < cfg.new_track_thresh) continue;
                const double dist = 1.0 - box_iou(tb, d.box);
                if (dist <= cfg.match_thresh) cost[r][c] = dist;
            }
        }
        const std::vector<int> assign = hungarian(cost);
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const int c = assign[r];
            if (c < 0 || cost[r][static_cast<std::size_t>(c)] >= kBig) continue;
            Track& t = tracks_[static_cast<std::size_t>(rows[r])];
            const TrackDet& d = dets[static_cast<std::size_t>(cols[static_cast<std::size_t>(c)])];
            t.kf.update(d.box);
            t.det_box = d.box;
            t.lost_frames = 0;
            t.conf = d.conf;
            t.label = d.label;
            ++t.hits;
            t.lost = false;
            t.last_ms = now_ms;
            if (!t.confirmed && (t.hits >= cfg.min_hits || d.conf >= cfg.high_thresh)) t.confirmed = true;
            track_done[static_cast<std::size_t>(rows[r])] = 1;
            det_done[static_cast<std::size_t>(cols[static_cast<std::size_t>(c)])] = 1;
        }
    }

    std::vector<Track> tracks_;
    int next_id_ = 0;
    std::int64_t last_update_ms_ = 0;
};

} // namespace rigmind
