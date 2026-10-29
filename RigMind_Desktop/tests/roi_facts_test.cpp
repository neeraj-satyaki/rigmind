// Unit tests for the Platform ROI, the scale selector and the detections -> EWS facts rules (pipeline/ews_facts.h).
// No GStreamer, no ONNX: builds in every configuration.
#include <cstdio>
#include <cstdlib>

#include "pipeline/ews_facts.h"

using namespace rigmind;

static int failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static NamedBox det(const char* label, float x1, float y1, float x2, float y2, float conf = 0.9f) {
    return {label, conf, {x1, y1, x2, y2}};
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void roi_geometry() {
    RoiConfig cfg;
    cfg.scale = 1.2f;
    // Platform 200x100 centred at (400, 300): wide scale gives 240x120 about the same centre.
    auto roi = compute_platform_roi({det("platform", 300, 250, 500, 350)}, cfg, 1280, 720);
    CHECK(roi && near(roi->x1, 280) && near(roi->x2, 520) && near(roi->y1, 240) && near(roi->y2, 360));
    cfg.scale = 1.4f; // zoomed
    roi = compute_platform_roi({det("platform", 300, 250, 500, 350)}, cfg, 1280, 720);
    CHECK(roi && near(roi->x1, 260) && near(roi->x2, 540) && near(roi->y1, 230) && near(roi->y2, 370));
    // Clamped to [0, w-1] x [0, h-1].
    roi = compute_platform_roi({det("platform", 0, 0, 1280, 720)}, cfg, 1280, 720);
    CHECK(roi && near(roi->x1, 0) && near(roi->y1, 0) && near(roi->x2, 1279) && near(roi->y2, 719));
    // Highest confidence Platform wins; other classes are ignored; none found gives nullopt.
    roi = compute_platform_roi({det("platform", 0, 0, 100, 100, 0.5f), det("platform", 600, 300, 700, 400, 0.9f)}, cfg, 1280, 720);
    CHECK(roi && roi->cx() > 600);
    CHECK(!compute_platform_roi({det("machine", 0, 0, 100, 100)}, cfg, 1280, 720));
}

static void roi_hold() {
    RoiConfig cfg;
    cfg.hold_frames = 30;
    RoiState state;
    const Box found{10, 10, 200, 200};
    CHECK(update_roi(state, found, cfg).has_value());
    for (int i = 0; i < 30; ++i) CHECK(update_roi(state, std::nullopt, cfg).has_value()); // held for exactly hold_frames
    CHECK(!update_roi(state, std::nullopt, cfg).has_value());                              // then released
    CHECK(!update_roi(state, std::nullopt, cfg).has_value());
    // A new platform resets the counter.
    CHECK(update_roi(state, found, cfg).has_value());
    CHECK(update_roi(state, std::nullopt, cfg).has_value());
}

static void roi_filter() {
    RoiConfig cfg;
    const std::vector<NamedBox> all = {det("platform", 100, 100, 300, 300), det("derrickman", 150, 150, 170, 190),
                                       det("machine", 800, 100, 900, 300)};
    const Box roi{50, 50, 350, 350};
    const auto kept = filter_detections(all, roi, cfg);
    CHECK(kept.size() == 2 && kept[0].label == "platform" && kept[1].label == "derrickman"); // outside centroid dropped
    // The platform is kept even when its own centroid is outside a stale ROI.
    const auto only_platform = filter_detections({det("platform", 900, 500, 1000, 600)}, roi, cfg);
    CHECK(only_platform.size() == 1);
    // No ROI: everything passes through.
    CHECK(filter_detections(all, std::nullopt, cfg).size() == 3);
}

static void scale_selection() {
    AutoScaleConfig cfg; // 1.2 / 1.4, threshold 0.30, hysteresis 0.03, median 5
    ScaleSelector fixed;
    cfg.force = "zoomed";
    CHECK(near(fixed.update(0.1f, cfg), 1.4f) && fixed.mode() == "zoomed");
    cfg.force = "wide";
    CHECK(near(fixed.update(0.9f, cfg), 1.2f) && fixed.mode() == "wide");

    cfg.force = "";
    ScaleSelector sel;
    CHECK(near(sel.update(std::nullopt, cfg), 1.2f) && sel.mode() == "auto-wide"); // no platform yet: wide
    for (int i = 0; i < 5; ++i) sel.update(0.31f, cfg);                            // inside the hysteresis band: stays wide
    CHECK(sel.mode() == "auto-wide");
    for (int i = 0; i < 5; ++i) sel.update(0.40f, cfg);                            // clearly wide platform: zoomed
    CHECK(sel.mode() == "auto-zoomed" && near(sel.update(0.40f, cfg), 1.4f));
    sel.update(0.29f, cfg);                                                        // between enter and leave: still zoomed
    CHECK(sel.mode() == "auto-zoomed");
    sel.update(0.10f, cfg);                                                        // one low frame: the median holds zoomed
    CHECK(sel.mode() == "auto-zoomed");
    for (int i = 0; i < 5; ++i) sel.update(0.10f, cfg);                            // sustained low: back to wide
    CHECK(sel.mode() == "auto-wide");
    sel.update(std::nullopt, cfg);                                                 // no platform: decision stands
    CHECK(sel.mode() == "auto-wide");
    cfg.enabled = false;
    CHECK(near(sel.update(0.9f, cfg), 1.2f)); // neither pinned nor automatic: wide
}

static void facts() {
    // Wide 1280x720 frame, platform on the left, derrickman at x=300 (inside the 450 limit).
    std::vector<NamedBox> d = {det("platform", 0, 50, 480, 630), det("machine", 530, 0, 930, 720),
                               det("derrickman", 280, 400, 320, 520), det("elevators", 200, 300, 320, 400),
                               det("actuator", 620, 270, 720, 720)};
    FrameFacts f = derive_facts(d);
    CHECK(f.machine_present && f.elevator_present && f.link_tilt_present);
    CHECK(!f.derrickman_out_of_safe_zone);
    CHECK(f.machine_past_right_edge);    // machine centroid x = 730 > 700
    CHECK(f.elevator_near_platform);     // |260 - 240| < 300
    CHECK(!f.link_tilt_near_derrickman); // threshold 0 in wide mode: never fires, as in IDSS

    // The derrickman steps across the line (centroid x 300 -> 500 > 450): the flag turns on.
    d[2] = det("derrickman", 480, 400, 520, 520);
    CHECK(derive_facts(d).derrickman_out_of_safe_zone);
    // Exactly on the limit is still safe (strictly greater than).
    d[2] = det("derrickman", 430, 400, 470, 520);
    CHECK(!derive_facts(d).derrickman_out_of_safe_zone);

    // Link-tilt angle: actuator centroid (670, 495), machine centroid (730, 360): atan2(-135, 60) = -66 deg, outside (-20, 15).
    CHECK(!derive_facts(d).link_tilt_angled);
    // An actuator level with the machine and to its left is angled 0 deg: inside (-20, 15).
    d[4] = det("actuator", 560, 340, 640, 380);
    CHECK(derive_facts(d).link_tilt_angled);

    // Absent objects give false flags (IDSS default state is absent).
    const FrameFacts none = derive_facts({});
    CHECK(!none.machine_present && !none.derrickman_out_of_safe_zone && !none.elevator_near_platform);

    // Collision approximation: the elevator crossing the platform outline is a collision, one wholly inside is not.
    CHECK(derive_facts({det("platform", 0, 50, 480, 630), det("elevators", 440, 300, 560, 400)}).platform_elevator_collision);
    CHECK(!derive_facts({det("platform", 0, 50, 480, 630), det("elevators", 100, 300, 200, 400)}).platform_elevator_collision);
}

static void zoom_thresholds_and_roi_scope() {
    // setThresholds(zoomFlag) (ews.cpp:110-130): the angled range, right edge, elevator distance and safe zone follow the flag.
    const FactThresholds wide = FactThresholds::for_zoom(false), zoom = FactThresholds::for_zoom(true);
    CHECK(near(wide.link_tilt_min_angle, -20) && near(zoom.link_tilt_min_angle, -50) && near(zoom.link_tilt_max_angle, 15));
    CHECK(near(wide.right_edge, 700) && near(zoom.right_edge, 900));
    CHECK(near(wide.max_elevator_platform_dist, 300) && near(zoom.max_elevator_platform_dist, 320));
    CHECK(near(wide.max_derrickman_safe_zone, 450) && near(zoom.max_derrickman_safe_zone, 400));
    // Actuator centroid (500, 540), machine centroid (740, 360): atan2(-180, 240) = -36.9 deg: outside wide (-20), inside zoom (-50).
    std::vector<NamedBox> d = {det("machine", 700, 340, 780, 380), det("actuator", 460, 520, 540, 560)};
    CHECK(!derive_facts(d, wide).link_tilt_angled);
    CHECK(derive_facts(d, zoom).link_tilt_angled);
    // Zoomed safe zone is 400: derrickman centroid 420 is out in zoom, safe in wide.
    d.push_back(det("derrickman", 400, 400, 440, 520));
    CHECK(!derive_facts(d, wide).derrickman_out_of_safe_zone && derive_facts(d, zoom).derrickman_out_of_safe_zone);

    // The ROI reaches only the derrickman rule: a machine / actuator / elevator outside the ROI still feed the chain, a
    // derrickman outside the ROI is not evaluated, one inside is.
    RoiConfig cfg;
    cfg.scale = 1.2f;
    std::vector<NamedBox> all = {det("platform", 0, 50, 485, 625), det("machine", 700, 300, 780, 400), det("actuator", 460, 340, 540, 400),
                                 det("elevators", 350, 300, 450, 400), det("derrickman", 470, 400, 520, 520)};
    const auto roi = compute_platform_roi(all, cfg, 1280, 720); // ~[0,..,582,..]: the machine (x 740) is outside
    CHECK(roi && roi->x2 < 700);
    const FrameFacts f = derive_facts_with_roi(all, roi, cfg);
    CHECK(f.machine_present && f.link_tilt_present && f.elevator_present && f.link_tilt_angled && f.elevator_near_platform);
    CHECK(f.derrickman_out_of_safe_zone); // centroid x 495 > 450, inside the ROI (< 582)
    all[4] = det("derrickman", 800, 400, 850, 520); // far outside the ROI
    CHECK(!derive_facts_with_roi(all, roi, cfg).derrickman_out_of_safe_zone);
    CHECK(derive_facts_with_roi(all, std::nullopt, cfg).derrickman_out_of_safe_zone); // no ROI: whole frame, as IDSS
}

static void contour_collision() {
    // Boxes overlap, but the platform outline never enters the elevator outline: no collision (IDSS tests mask contours).
    NamedBox platform = det("platform", 0, 50, 480, 630), elevator = det("elevators", 440, 300, 560, 400);
    platform.contour = {{0, 50}, {300, 50}, {300, 630}, {0, 630}};
    elevator.contour = {{440, 300}, {560, 300}, {560, 400}, {440, 400}};
    CHECK(!derive_facts({platform, elevator}).platform_elevator_collision);
    // A platform outline point inside the elevator outline is a collision.
    platform.contour = {{0, 50}, {480, 50}, {480, 350}, {0, 630}};
    CHECK(derive_facts({platform, elevator}).platform_elevator_collision);
}

int main() {
    contour_collision();
    roi_geometry();
    roi_hold();
    roi_filter();
    scale_selection();
    facts();
    zoom_thresholds_and_roi_scope();
    if (failures == 0) std::printf("roi_facts_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
