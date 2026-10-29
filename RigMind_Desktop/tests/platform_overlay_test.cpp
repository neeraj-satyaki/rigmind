// Unit tests for the platform mask overlay (pipeline/platform_overlay.h). OpenCV only; no GStreamer, no ONNX.
#include <cstdio>

#include <opencv2/imgproc.hpp>

#include "pipeline/platform_overlay.h"

using namespace rigmind;

static int failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

// A 100x50 box mask: a filled ellipse, so the outline is a real curve.
static cv::Mat ellipse_mask() {
    cv::Mat mask = cv::Mat::zeros(50, 100, CV_8UC1);
    cv::ellipse(mask, cv::Point(50, 25), cv::Size(45, 20), 0, 0, 360, cv::Scalar(255), cv::FILLED);
    return mask;
}

static bool all_inside(const PlatformOverlay& o, int w, int h) {
    for (const auto& c : o.outline)
        for (const cv::Point& p : c)
            if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h) return false;
    return true;
}

int main() {
    const cv::Mat mask = ellipse_mask();

    // 5.1 wide: 100x50 -> 120x50 (x * 1.2, y * 1.0, nearest).
    const cv::Mat wide = resize_platform_mask(mask, overlay_scale(false));
    CHECK(wide.cols == 120 && wide.rows == 50);
    // 5.2 zoomed: 100x50 -> 140x50.
    const cv::Mat zoomed = resize_platform_mask(mask, overlay_scale(true));
    CHECK(zoomed.cols == 140 && zoomed.rows == 50);
    // Nearest keeps the mask binary.
    CHECK(cv::countNonZero((zoomed != 0) & (zoomed != 255)) == 0);

    // Centred on the box: box x=300 (width 100), zoomed -> x_off = 300 - 20 = 280, so the outline spans 280..420.
    const cv::Rect box(300, 200, 100, 50);
    const PlatformOverlay o = platform_overlay(mask, box, 1280, 720, 1.4f);
    CHECK(o.box == box);
    CHECK(!o.outline.empty());
    int min_x = 99999, max_x = -1;
    for (const auto& c : o.outline)
        for (const cv::Point& p : c) {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
        }
    CHECK(min_x >= 280 && max_x <= 420 && min_x < 300 && max_x > 400);
    CHECK(all_inside(o, 1280, 720));

    // 5.3 near the frame edges: nothing outside the frame, whatever the corner.
    for (const cv::Rect& edge : {cv::Rect(0, 0, 100, 50), cv::Rect(1180, 670, 100, 50), cv::Rect(1230, 0, 100, 50), cv::Rect(0, 700, 100, 50)}) {
        const PlatformOverlay e = platform_overlay(mask, edge, 1280, 720, 1.4f);
        CHECK(all_inside(e, 1280, 720));
        CHECK(e.box.x >= 0 && e.box.y >= 0 && e.box.x + e.box.width <= 1280 && e.box.y + e.box.height <= 720);
    }
    // A mask wider than the frame is cut at the frame, not written outside it.
    const PlatformOverlay huge = platform_overlay(cv::Mat(50, 900, CV_8UC1, cv::Scalar(255)), cv::Rect(0, 0, 900, 50), 1280, 720, 1.4f);
    CHECK(all_inside(huge, 1280, 720));

    // Empty mask: the box alone, no outline, no error. Missing platform is the caller's case (no call).
    const PlatformOverlay empty = platform_overlay(cv::Mat(), box, 1280, 720, 1.4f);
    CHECK(empty.box == box && empty.outline.empty());

    // Translation does not change the outline's shape: same contours at two positions differ by the box shift.
    const PlatformOverlay a = platform_overlay(mask, cv::Rect(300, 200, 100, 50), 1280, 720, 1.2f);
    const PlatformOverlay b = platform_overlay(mask, cv::Rect(500, 300, 100, 50), 1280, 720, 1.2f);
    CHECK(a.outline.size() == b.outline.size() && !a.outline.empty());
    if (a.outline.size() == b.outline.size() && !a.outline.empty() && a.outline[0].size() == b.outline[0].size()) {
        for (size_t i = 0; i < a.outline[0].size(); ++i)
            CHECK(b.outline[0][i] - a.outline[0][i] == cv::Point(200, 100));
    } else {
        CHECK(false);
    }

    // Mask decoding + polygon: one prototype channel that is 1 over a rectangle, coefficient 10 -> a box-sized mask.
    const int seg = 160, input = 640;
    std::vector<float> proto(seg * seg, -5.0f);
    for (int y = 40; y < 100; ++y)
        for (int x = 40; x < 120; ++x) proto[y * seg + x] = 5.0f;
    const float coeff = 1.0f;
    // Frame 1280x720 letterboxed into 640x640: scale 0.5, pad_top 140. Prototype cell (40..120, 40..100) = input px (160..480, 160..400)
    // = frame px x 320..960, y (160-140)/0.5=40 .. (400-140)/0.5=520.
    const cv::Rect target(320, 40, 640, 480);
    const cv::Mat box_mask = decode_box_mask(proto.data(), 1, seg, seg, &coeff, input, 0.5f, 0, 140, 1280, 720, target);
    CHECK(box_mask.size() == target.size());
    CHECK(cv::countNonZero(box_mask) > 0.95 * target.area());
    const auto polygon = mask_polygon(box_mask, target.tl());
    CHECK(polygon.size() >= 4 && polygon.size() <= 64);
    for (const cv::Point& p : polygon) CHECK(p.x >= 319 && p.x <= 961 && p.y >= 39 && p.y <= 521);

    if (failures == 0) std::printf("platform_overlay_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
