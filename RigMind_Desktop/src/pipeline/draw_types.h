// What the detection workers hand to the video overlay (pipeline/overlay_draw.h): plain data, no OpenCV.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rigmind {

struct DrawPoint {
    int x = 0, y = 0;
};

struct DrawItem {
    std::string label;      // class name, drawn without confidence
    int box[4] = {0, 0, 0, 0}; // x1, y1, x2, y2 in frame pixels
    std::vector<DrawPoint> polygon; // mask outline (IDSS items), may be empty
    bool in_roi = true;
    bool held = false;      // PPE: tracker prediction without a detection on this frame (drawn dashed)
};

// One model's result for one frame.
struct DrawResult {
    std::string model;      // "ppe" or "idss"
    std::string cam;        // camera the frame came from: results are matched by (cam, key)
    std::int64_t key = -1;  // buffer PTS in ns of the frame it was computed on
    std::vector<DrawItem> items;
    bool has_platform = false;           // IDSS only: platform box + scaled mask outline, as IDSS draws it
    int platform_box[4] = {0, 0, 0, 0};
    std::vector<std::vector<DrawPoint>> outline;
    bool has_roi = false;
    int roi[4] = {0, 0, 0, 0};
};

} // namespace rigmind
