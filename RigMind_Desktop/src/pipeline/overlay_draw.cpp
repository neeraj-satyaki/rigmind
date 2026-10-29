#include "pipeline/overlay_draw.h"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

namespace rigmind {

namespace {

const cv::Scalar kPlatformGreen(0, 255, 0);
const cv::Scalar kRoiBlue(235, 206, 135);  // sky blue (BGR)
const cv::Scalar kAlertRed(0, 0, 255);

cv::Scalar class_color(const std::string& model, const std::string& label) {
    static const cv::Scalar palette[] = {{255, 160, 40}, {40, 200, 255}, {120, 255, 120}, {255, 120, 220}, {60, 140, 255}, {255, 255, 80}};
    std::size_t h = std::hash<std::string>{}(model + label);
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

void dashed_line(cv::Mat& img, cv::Point a, cv::Point b, const cv::Scalar& color, int thickness, int dash = 10) {
    const double length = std::hypot(b.x - a.x, b.y - a.y);
    if (length < 1) return;
    const cv::Point2d step((b.x - a.x) / length, (b.y - a.y) / length);
    for (double d = 0; d < length; d += 2 * dash) {
        const double e = std::min(d + dash, length);
        cv::line(img, cv::Point(cvRound(a.x + step.x * d), cvRound(a.y + step.y * d)),
                 cv::Point(cvRound(a.x + step.x * e), cvRound(a.y + step.y * e)), color, thickness, cv::LINE_8);
    }
}

void dashed_rect(cv::Mat& img, const cv::Rect& r, const cv::Scalar& color, int thickness) {
    dashed_line(img, r.tl(), {r.x + r.width, r.y}, color, thickness);
    dashed_line(img, {r.x + r.width, r.y}, r.br(), color, thickness);
    dashed_line(img, r.br(), {r.x, r.y + r.height}, color, thickness);
    dashed_line(img, {r.x, r.y + r.height}, r.tl(), color, thickness);
}

// Class name on a filled tag (PPE items only; the IDSS overlay carries no text).
void label_box(cv::Mat& img, const std::string& text, cv::Point at, const cv::Scalar& background) {
    int baseline = 0;
    const cv::Size size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    const cv::Point origin(std::clamp(at.x, 0, std::max(0, img.cols - size.width - 4)), std::clamp(at.y, size.height + 2, img.rows - baseline - 1));
    cv::rectangle(img, cv::Rect(origin.x, origin.y - size.height - 2, size.width + 4, size.height + baseline + 2), background, cv::FILLED);
    cv::putText(img, text, cv::Point(origin.x + 2, origin.y), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
}

std::vector<cv::Point> to_points(const std::vector<DrawPoint>& in) {
    std::vector<cv::Point> out;
    out.reserve(in.size());
    for (const DrawPoint& p : in) out.emplace_back(p.x, p.y);
    return out;
}

// Translucent fill of a polygon: only the polygon's bounding rectangle is blended.
void fill_mask(cv::Mat& img, const std::vector<cv::Point>& poly, const cv::Scalar& color) {
    if (poly.size() < 3) return;
    const cv::Rect r = cv::boundingRect(poly) & cv::Rect(0, 0, img.cols, img.rows);
    if (r.empty()) return;
    cv::Mat region = img(r);
    cv::Mat tinted = region.clone();
    std::vector<cv::Point> local = poly;
    for (cv::Point& p : local) p -= r.tl();
    cv::fillPoly(tinted, std::vector<std::vector<cv::Point>>{local}, color);
    cv::addWeighted(tinted, 0.35, region, 0.65, 0, region);
}

} // namespace

void draw_overlay(cv::Mat& bgr, const std::vector<DrawResult>& results) {
    // Mask fills first, so outlines and labels stay crisp on top of them.
    for (const DrawResult& r : results) {
        for (const DrawItem& item : r.items) {
            if (r.model == "idss" && !item.polygon.empty()) {
                const bool alert = item.label == "derrickman" && !item.in_roi;
                fill_mask(bgr, to_points(item.polygon), alert ? kAlertRed : class_color(r.model, item.label));
            }
        }
    }
    for (const DrawResult& r : results) {
        if (r.has_roi) {
            const cv::Rect roi(r.roi[0], r.roi[1], r.roi[2] - r.roi[0], r.roi[3] - r.roi[1]);
            dashed_rect(bgr, roi, kRoiBlue, 2);
        }
        if (r.has_platform) { // IDSS: green box, then the contour of the scaled mask
            cv::rectangle(bgr, cv::Rect(r.platform_box[0], r.platform_box[1], r.platform_box[2] - r.platform_box[0], r.platform_box[3] - r.platform_box[1]), kPlatformGreen, 2);
            for (const auto& contour : r.outline) {
                const std::vector<cv::Point> pts = to_points(contour);
                if (pts.size() > 1) cv::polylines(bgr, pts, true, kPlatformGreen, 2);
            }
        }
        for (const DrawItem& item : r.items) {
            const cv::Rect box(item.box[0], item.box[1], item.box[2] - item.box[0], item.box[3] - item.box[1]);
            const bool derrickman_out = r.model == "idss" && item.label == "derrickman" && !item.in_roi;
            const cv::Scalar color = derrickman_out ? kAlertRed : class_color(r.model, item.label);
            if (derrickman_out) {
                cv::rectangle(bgr, box, kAlertRed, 3);
            } else if (item.held || !item.in_roi) {
                dashed_rect(bgr, box, color, 2);
            } else {
                cv::rectangle(bgr, box, color, 2);
            }
            if (r.model == "ppe") label_box(bgr, item.label, {box.x, box.y}, color * 0.6);
        }
    }
}

} // namespace rigmind
