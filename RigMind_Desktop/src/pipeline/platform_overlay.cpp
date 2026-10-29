#include "pipeline/platform_overlay.h"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

namespace rigmind {

cv::Mat resize_platform_mask(const cv::Mat& mask, float scale_x) {
    cv::Mat resized;
    cv::resize(mask, resized, cv::Size(), scale_x, 1.0, cv::INTER_NEAREST);
    return resized;
}

PlatformOverlay platform_overlay(const cv::Mat& mask, const cv::Rect& box, int frame_w, int frame_h, float scale_x) {
    PlatformOverlay out;
    const cv::Rect frame(0, 0, frame_w, frame_h);
    out.box = box & frame;
    if (mask.empty() || mask.channels() != 1) return out;

    const cv::Mat resized = resize_platform_mask(mask, scale_x);
    // Centred on the box. As in IDSS the offsets are clamped so the mask lies inside the frame when it fits
    // (max(0, min(offset, frame - mask))); a mask larger than the frame starts at 0 and the excess is cut off below.
    int x_off = box.x - (resized.cols - box.width) / 2;
    int y_off = box.y - (resized.rows - box.height) / 2;
    x_off = std::max(0, std::min(x_off, frame_w - resized.cols));
    y_off = std::max(0, std::min(y_off, frame_h - resized.rows));

    cv::Mat edges;
    cv::Canny(resized, edges, 100, 200);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edges, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    for (const auto& contour : contours) {
        std::vector<cv::Point> moved;
        for (const cv::Point& p : contour) {
            const cv::Point q(p.x + x_off, p.y + y_off); // translated, shape unchanged
            if (frame.contains(q)) moved.push_back(q);    // only the part inside the frame is kept
        }
        if (!moved.empty()) out.outline.push_back(std::move(moved));
    }
    return out;
}

cv::Mat decode_box_mask(const float* proto, int seg_ch, int seg_h, int seg_w, const float* coeffs, int input_w, float scale,
                        int pad_left, int pad_top, int frame_w, int frame_h, const cv::Rect& box_in) {
    const cv::Rect box = box_in & cv::Rect(0, 0, frame_w, frame_h);
    if (box.empty() || seg_ch <= 0) return {};

    // The prototype grid is input / 4. Work in that space, only over the box, and resize just that region to frame
    // pixels (a full-frame resize per detection would be far too costly).
    const float g = static_cast<float>(seg_w) / input_w;
    const int gx1 = std::max(0, static_cast<int>(std::floor((box.x * scale + pad_left) * g)));
    const int gy1 = std::max(0, static_cast<int>(std::floor((box.y * scale + pad_top) * g)));
    const int gx2 = std::min(seg_w, std::max(gx1 + 1, static_cast<int>(std::ceil(((box.x + box.width) * scale + pad_left) * g))));
    const int gy2 = std::min(seg_h, std::max(gy1 + 1, static_cast<int>(std::ceil(((box.y + box.height) * scale + pad_top) * g))));
    if (gx2 <= gx1 || gy2 <= gy1) return {};

    cv::Mat logits(gy2 - gy1, gx2 - gx1, CV_32F, cv::Scalar(0));
    for (int k = 0; k < seg_ch; ++k) {
        const cv::Mat plane(seg_h, seg_w, CV_32F, const_cast<float*>(proto + static_cast<size_t>(k) * seg_h * seg_w));
        logits += plane(cv::Rect(gx1, gy1, gx2 - gx1, gy2 - gy1)) * coeffs[k];
    }
    cv::Mat probability;
    cv::exp(-logits, probability);
    probability = 1.0 / (1.0 + probability);
    cv::Mat resized;
    cv::resize(probability, resized, box.size(), 0, 0, cv::INTER_LINEAR);
    return resized > 0.5f; // CV_8U box-sized mask, 255 where set (IDSS boxMask)
}

// Largest contour of a box mask, simplified to at most max_points, in frame pixels.
std::vector<cv::Point> mask_polygon(const cv::Mat& box_mask, const cv::Point& origin, int max_points) {
    if (box_mask.empty()) return {};
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(box_mask.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) return {};
    const auto largest = std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
        return cv::contourArea(a) < cv::contourArea(b);
    });
    std::vector<cv::Point> poly;
    double epsilon = 1.0;
    cv::approxPolyDP(*largest, poly, epsilon, true);
    while (static_cast<int>(poly.size()) > max_points && epsilon < 200) { // coarser until it fits
        epsilon *= 1.5;
        cv::approxPolyDP(*largest, poly, epsilon, true);
    }
    for (cv::Point& p : poly) p += origin;
    return poly;
}

} // namespace rigmind
