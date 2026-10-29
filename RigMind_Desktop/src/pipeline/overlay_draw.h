// Burns the detections into a BGR frame (in place). No text on the IDSS items (class names only on PPE items, never a confidence): boxes, translucent mask fills for IDSS items, the
// IDSS-style platform box + contour, the dashed ROI rectangle; class by colour, outside the ROI = dashed box, derrickman
// outside the ROI = strong red.
#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "pipeline/draw_types.h"

namespace rigmind {

void draw_overlay(cv::Mat& bgr, const std::vector<DrawResult>& results);

} // namespace rigmind
