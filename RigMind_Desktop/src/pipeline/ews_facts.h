// Platform-based dynamic ROI and the detections -> EWS facts rules. Plain C++, no GStreamer or ONNX: unit tested alone.
//
// Source of truth is the IDSS codebase (idss-dashboard\idss\src\safety\ews.cpp, src\inference\yolov8_seg.cpp). The
// wide or zoomed threshold set follows the scale mode (ScaleSelector), as IDSS setThresholds(zoomFlag) does.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "backend/alarm_state.h"

namespace rigmind {

struct Box {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float cx() const { return (x1 + x2) / 2.0f; } // ews.cpp:192 calculate_centroid(rect)
    float cy() const { return (y1 + y2) / 2.0f; }
    float width() const { return x2 - x1; }
    float height() const { return y2 - y1; }
};

struct PolyPoint {
    float x = 0, y = 0;
};

struct NamedBox {
    std::string label; // class name from the model ("platform", "derrickman", ...)
    float conf = 0;
    Box box;
    std::vector<PolyPoint> contour; // mask outline in frame pixels (platform and elevator only; empty when unknown)
};

// Even-odd ray cast: is the point inside the polygon.
inline bool point_in_polygon(const PolyPoint& p, const std::vector<PolyPoint>& poly) {
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const bool crosses = (poly[i].y > p.y) != (poly[j].y > p.y) &&
                             p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x;
        if (crosses) inside = !inside;
    }
    return inside;
}

inline bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// ---- ROI ------------------------------------------------------------------------------------------------------------
struct RoiConfig {
    bool enabled = true;
    std::string reference_class = "Platform";
    float scale = 1.2f; // set per frame from ScaleSelector (IDSS RESIZED_MASK_SCALE: 1.2 wide, 1.4 zoomed)
    int hold_frames = 30;
};

struct RoiState {
    std::optional<Box> last;
    int hold_count = 0;
};

// The highest-confidence reference-class detection, scaled about its centre and clamped to the frame. The RAW rect is
// used (not the 1.1x overlay rect of ews.cpp:63).
inline std::optional<Box> compute_platform_roi(const std::vector<NamedBox>& dets, const RoiConfig& cfg, int frame_w, int frame_h) {
    const NamedBox* best = nullptr;
    for (const NamedBox& d : dets) {
        if (same_name(d.label, cfg.reference_class) && (!best || d.conf > best->conf)) best = &d;
    }
    if (!best) return std::nullopt;
    const float w = best->box.width() * cfg.scale, h = best->box.height() * cfg.scale;
    const float cx = best->box.cx(), cy = best->box.cy();
    Box roi;
    roi.x1 = std::clamp(cx - w / 2, 0.0f, static_cast<float>(frame_w - 1));
    roi.y1 = std::clamp(cy - h / 2, 0.0f, static_cast<float>(frame_h - 1));
    roi.x2 = std::clamp(cx + w / 2, 0.0f, static_cast<float>(frame_w - 1));
    roi.y2 = std::clamp(cy + h / 2, 0.0f, static_cast<float>(frame_h - 1));
    return roi;
}

// Per frame: a found ROI is stored; a missing one is held for hold_frames frames, then dropped (no ROI = whole frame).
inline std::optional<Box> update_roi(RoiState& state, const std::optional<Box>& found, const RoiConfig& cfg) {
    if (found) {
        state.last = found;
        state.hold_count = 0;
        return state.last;
    }
    if (state.last && state.hold_count < cfg.hold_frames) {
        ++state.hold_count;
        return state.last;
    }
    state.last.reset();
    return std::nullopt;
}

inline bool box_centroid_in(const Box& b, const Box& roi) {
    return b.cx() >= roi.x1 && b.cx() <= roi.x2 && b.cy() >= roi.y1 && b.cy() <= roi.y2;
}

// No ROI keeps everything; otherwise the reference class itself is always kept and the rest only if its centroid is inside.
inline std::vector<NamedBox> filter_detections(const std::vector<NamedBox>& dets, const std::optional<Box>& roi, const RoiConfig& cfg) {
    if (!roi) return dets;
    std::vector<NamedBox> kept;
    for (const NamedBox& d : dets) {
        if (same_name(d.label, cfg.reference_class) || box_centroid_in(d.box, *roi)) kept.push_back(d);
    }
    return kept;
}

// ---- Scale ----------------------------------------------------------------------------------------------------------
// One scale for the ROI (both axes) and the platform overlay (x only). No operator flag: models\inference.cfg "auto_scale"
// pins it ("force") or lets the Platform's width decide, with hysteresis and a median over the last decisions.
struct AutoScaleConfig {
    bool enabled = true;
    float wide = 1.2f, zoomed = 1.4f;
    float width_fraction = 0.30f; // Platform width / frame width at or above this: zoomed
    float hysteresis = 0.03f;
    int median_frames = 5;
    std::string force; // "", "wide" or "zoomed"
};

class ScaleSelector {
public:
    // platform_width_fraction: nullopt when no Platform was found (the previous decision stands).
    float update(const std::optional<float>& platform_width_fraction, const AutoScaleConfig& cfg) {
        if (cfg.force == "zoomed" || cfg.force == "wide") {
            mode_ = cfg.force;
            return cfg.force == "zoomed" ? cfg.zoomed : cfg.wide;
        }
        if (!cfg.enabled) { // neither pinned nor automatic: IDSS default (wide)
            mode_ = "wide";
            return cfg.wide;
        }
        if (platform_width_fraction) {
            // Hysteresis on the raw decision: enter at threshold + h, leave at threshold - h.
            if (!raw_zoomed_ && *platform_width_fraction >= cfg.width_fraction + cfg.hysteresis) raw_zoomed_ = true;
            else if (raw_zoomed_ && *platform_width_fraction <= cfg.width_fraction - cfg.hysteresis) raw_zoomed_ = false;
            decisions_.push_back(raw_zoomed_);
            while (static_cast<int>(decisions_.size()) > std::max(1, cfg.median_frames)) decisions_.erase(decisions_.begin());
            const auto zoomed = std::count(decisions_.begin(), decisions_.end(), true);
            zoomed_ = zoomed * 2 > static_cast<long>(decisions_.size()); // median of 0/1 values
        }
        mode_ = zoomed_ ? "auto-zoomed" : "auto-wide";
        return zoomed_ ? cfg.zoomed : cfg.wide;
    }
    const std::string& mode() const { return mode_; }

private:
    bool raw_zoomed_ = false, zoomed_ = false;
    std::vector<bool> decisions_;
    std::string mode_ = "auto-wide";
};

// ---- Facts ----------------------------------------------------------------------------------------------------------
// Thresholds in 1280x720 frame pixels (IDSS draws and decides on its 1280x720 frame, main.cpp:534), ews.cpp:110-130 setThresholds().
struct FactThresholds {
    float link_tilt_max_angle = 15.0f;                  // :113
    float link_tilt_min_angle = -20.0f;                 // :114
    float right_edge = 700;                             // :116 RIGHT_EDGE_THRESHOLD
    float max_elevator_platform_dist = 300.0f;          // :117
    float derrickman_linktilt_dist = 0.0f;              // :118 (0 in wide mode: this rule can never fire, as in IDSS)
    float max_derrickman_safe_zone = 450;               // :119 MAX_DERRICKAMAN_SAFE_ZONE

    // ews.cpp:110-130 setThresholds(zoomFlag): the whole set follows the zoom flag (the operator's declaration of the camera
    // view in IDSS, main.cpp:559; here the scale mode of ScaleSelector: forced or from the Platform width).
    static FactThresholds for_zoom(bool zoom) {
        FactThresholds t;
        if (zoom) {
            t.link_tilt_min_angle = -50.0f;       // :114
            t.right_edge = 900;                   // :116
            t.max_elevator_platform_dist = 320.0f; // :117
            t.derrickman_linktilt_dist = 200.0f;  // :118
            t.max_derrickman_safe_zone = 400;     // :119
        }
        return t;
    }
};

// Model class names -> IDSS object names. The IDSS "Link-tilt" is the model's "actuator", "Elevator" is "elevators".
inline std::string idss_object(const std::string& label) {
    static const struct { const char* from; const char* to; } map[] = {
        {"platform", "Platform"}, {"machine", "Machine"}, {"derrickman", "Derrickman"}, {"elevators", "Elevator"},
        {"elevator", "Elevator"}, {"actuator", "Link-tilt"}, {"link-tilt", "Link-tilt"}, {"link_tilt", "Link-tilt"}};
    for (const auto& m : map) {
        if (same_name(label, m.from)) return m.to;
    }
    return "";
}

// The nine EWS facts for one frame of (ROI-filtered) IDSS detections. Mirrors the flag conditions in ews.cpp:
//   machine_present / elevator_present / link_tilt_present: object state != absent (:431, :472, :503). IDSS also grades
//     full/partial from the MASK area (:154-164); that grade never feeds an alarm condition, so presence is enough.
//   link_tilt_angled: Machine found and angle(link-tilt centroid -> machine centroid) in (-20, 15) (:80-92, :285-307).
//   elevator_near_platform: |dx elevator-platform centroids| < 300 (:450).
//   derrickman_out_of_safe_zone: derrickman centroid x > 450 (:476).   machine_past_right_edge: machine centroid x > 700 (:476).
//   link_tilt_near_derrickman: |dy derrickman-link-tilt| < 0 (:503), which is never true in wide mode.
//   platform_elevator_collision: from the mask contours when both are known (as IDSS); APPROXIMATED with rectangles otherwise. IDSS (yolov8_seg.cpp:420-500) tests whether any Platform mask contour
//     point lies inside an Elevator mask contour. Without masks: the Platform rect outline passes through the Elevator
//     rect (they overlap and the elevator is not wholly inside the platform).
// Several detections of one class: centroids come from the highest confidence one, the link-tilt from the largest box
// (IDSS picks the largest mask area, :32-42, but then reads whichever centroid came last; the intent is used).
// Raw numbers behind the facts, for the low-rate "facts" diagnostic event. NaN = not available.
struct FactsDetail {
    float angle_deg = std::nanf("");
    float derrickman_cx = std::nanf("");
    float machine_cx = std::nanf("");
};

inline FrameFacts derive_facts(const std::vector<NamedBox>& dets, const FactThresholds& t = {}, FactsDetail* detail = nullptr) {
    const NamedBox *platform = nullptr, *machine = nullptr, *derrickman = nullptr, *elevator = nullptr, *tilt = nullptr;
    auto better = [](const NamedBox* current, const NamedBox& d) { return !current || d.conf > current->conf; };
    for (const NamedBox& d : dets) {
        const std::string name = idss_object(d.label);
        if (name == "Platform" && better(platform, d)) platform = &d;
        else if (name == "Machine" && better(machine, d)) machine = &d;
        else if (name == "Derrickman" && better(derrickman, d)) derrickman = &d;
        else if (name == "Elevator" && better(elevator, d)) elevator = &d;
        else if (name == "Link-tilt" && (!tilt || d.box.width() * d.box.height() > tilt->box.width() * tilt->box.height())) tilt = &d;
    }

    FrameFacts f;
    f.machine_present = machine != nullptr;
    f.elevator_present = elevator != nullptr;
    f.link_tilt_present = tilt != nullptr;
    if (machine && tilt) {
        const float angle = std::atan2(machine->box.cy() - tilt->box.cy(), machine->box.cx() - tilt->box.cx()) * 180.0f / 3.14159265f;
        f.link_tilt_angled = angle < t.link_tilt_max_angle && angle > t.link_tilt_min_angle;
        if (detail) detail->angle_deg = angle;
    }
    if (detail) {
        if (derrickman) detail->derrickman_cx = derrickman->box.cx();
        if (machine) detail->machine_cx = machine->box.cx();
    }
    if (elevator && platform) {
        f.elevator_near_platform = std::fabs(elevator->box.cx() - platform->box.cx()) < t.max_elevator_platform_dist;
        const Box &p = platform->box, &e = elevator->box;
        if (platform->contour.size() >= 3 && elevator->contour.size() >= 3) {
            // IDSS (yolov8_seg.cpp:420-500): collision when any Platform mask contour point lies inside an Elevator mask contour.
            f.platform_elevator_collision = false;
            for (const PolyPoint& point : platform->contour) {
                if (point_in_polygon(point, elevator->contour)) {
                    f.platform_elevator_collision = true;
                    break;
                }
            }
        } else {
            // No masks available: the Platform rectangle outline passes through the Elevator rectangle.
            const bool overlap = e.x1 < p.x2 && e.x2 > p.x1 && e.y1 < p.y2 && e.y2 > p.y1;
            const bool inside = e.x1 >= p.x1 && e.x2 <= p.x2 && e.y1 >= p.y1 && e.y2 <= p.y2;
            f.platform_elevator_collision = overlap && !inside;
        }
    }
    f.derrickman_out_of_safe_zone = derrickman && derrickman->box.cx() > t.max_derrickman_safe_zone;
    f.machine_past_right_edge = machine && machine->box.cx() > t.right_edge;
    f.link_tilt_near_derrickman = derrickman && tilt && std::fabs(derrickman->box.cy() - tilt->box.cy()) < t.derrickman_linktilt_dist;
    return f;
}

// Facts with the Platform ROI applied where it belongs. IDSS (ews.cpp, yolov8_seg.cpp) has no ROI at all: the machine /
// link-tilt / elevator chain (Yellow, Orange, Red) must see every detection, exactly as IDSS does. The ROI is the
// RigMind addition for "the derrickman must not cross a boundary", so only the derrickman safe-zone rule uses the
// ROI-filtered detections (a derrickman outside the scaled Platform rect is not evaluated).
inline FrameFacts derive_facts_with_roi(const std::vector<NamedBox>& dets, const std::optional<Box>& roi, const RoiConfig& cfg,
                                        const FactThresholds& t = {}, FactsDetail* detail = nullptr) {
    FrameFacts f = derive_facts(dets, t, detail);
    if (roi) f.derrickman_out_of_safe_zone = derive_facts(filter_detections(dets, roi, cfg), t).derrickman_out_of_safe_zone;
    return f;
}

} // namespace rigmind
