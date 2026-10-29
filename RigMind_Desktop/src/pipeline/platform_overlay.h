// Platform mask overlay, as IDSS draws it (idss src/inference/yolov8_seg.cpp draw_objects, "Platform" branch): the
// Platform's box mask is stretched horizontally by the scale, centred on its box, and its outline is reported so the UI
// can draw it (box first, outline second). OpenCV imgproc only; no GStreamer or ONNX, so it is unit tested alone.
#pragma once

#include <vector>

#include <opencv2/core.hpp>

namespace rigmind {

struct PlatformOverlay {
    cv::Rect box;                                  // the detection rect, clipped to the frame
    std::vector<std::vector<cv::Point>> outline;   // contours in frame pixels; empty when the mask is empty
};

// IDSS RESIZED_MASK_SCALE (ews.cpp setThresholds): 1.4 zoomed, 1.2 wide.
inline float overlay_scale(bool zoomed) { return zoomed ? 1.4f : 1.2f; }

// cv::resize(mask, Size(), scale_x, 1.0, INTER_NEAREST). Nearest only: the mask stays binary.
cv::Mat resize_platform_mask(const cv::Mat& mask, float scale_x);

// mask: single-channel binary mask of the box (rect.size()), 0 / non-zero. scale_x: from overlay_scale or the
// auto_scale setting. An empty mask gives the box with an empty outline.
PlatformOverlay platform_overlay(const cv::Mat& mask, const cv::Rect& box, int frame_w, int frame_h, float scale_x);

// YOLOv8-seg mask of one detection (IDSS yolov8_seg.cpp:268-288, but only over the box): sigmoid(coeffs . proto) on the
// prototype grid, resized to the box, threshold 0.5. proto: seg_ch x seg_h x seg_w floats; coeffs: seg_ch. scale / pad_left /
// pad_top / input_w describe the letterbox of the model input. Returns a box-sized CV_8U mask (255 inside).
cv::Mat decode_box_mask(const float* proto, int seg_ch, int seg_h, int seg_w, const float* coeffs, int input_w, float scale,
                        int pad_left, int pad_top, int frame_w, int frame_h, const cv::Rect& box);

// Largest contour of a box mask, simplified (Douglas-Peucker) to at most max_points, shifted by the box origin.
std::vector<cv::Point> mask_polygon(const cv::Mat& box_mask, const cv::Point& origin, int max_points = 64);

} // namespace rigmind
