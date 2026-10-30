// Display-only mask stabiliser (pipeline/mask_stabilizer.h): no blinking, no shimmer, no spinning.
#include <cmath>
#include <iostream>
#include <random>

#include "pipeline/mask_stabilizer.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
using namespace rigmind;

DrawItem blob(const char* label, float cx, float cy, float r, int points, int start = 0, bool reverse = false, float jitter = 0, std::mt19937* rng = nullptr) {
    DrawItem d;
    d.label = label;
    std::uniform_real_distribution<float> j(-jitter, jitter);
    for (int i = 0; i < points; ++i) {
        const int k = reverse ? (points - i + start) % points : (i + start) % points;
        const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(points);
        const float dx = rng ? j(*rng) : 0, dy = rng ? j(*rng) : 0;
        d.polygon.push_back({static_cast<int>(std::lround(cx + r * std::cos(a) + dx)), static_cast<int>(std::lround(cy + r * std::sin(a) + dy))});
    }
    d.box[0] = static_cast<int>(cx - r); d.box[1] = static_cast<int>(cy - r); d.box[2] = static_cast<int>(cx + r); d.box[3] = static_cast<int>(cy + r);
    return d;
}

std::pair<float, float> centroid(const DrawItem& d) {
    float x = 0, y = 0;
    for (const auto& p : d.polygon) { x += static_cast<float>(p.x); y += static_cast<float>(p.y); }
    return {x / static_cast<float>(d.polygon.size()), y / static_cast<float>(d.polygon.size())};
}
// How rough the outline is: the spread of the points' distance from the centre (0 for a perfect circle).
float roughness(const DrawItem& d) {
    const auto c = centroid(d);
    std::vector<float> r;
    for (const auto& p : d.polygon) r.push_back(std::hypot(static_cast<float>(p.x) - c.first, static_cast<float>(p.y) - c.second));
    float mean = 0;
    for (float v : r) mean += v;
    mean /= static_cast<float>(r.size());
    float var = 0;
    for (float v : r) var += (v - mean) * (v - mean);
    return std::sqrt(var / static_cast<float>(r.size()));
}
float area(const DrawItem& d) {
    float a = 0;
    for (std::size_t i = 0; i < d.polygon.size(); ++i) {
        const auto& u = d.polygon[i];
        const auto& v = d.polygon[(i + 1) % d.polygon.size()];
        a += static_cast<float>(u.x * v.y - v.x * u.y);
    }
    return std::fabs(a / 2.0f);
}
} // namespace

int main() {
    // ---- resampling: even spacing, fixed count
    const auto sq = MaskStabilizer::resample(blob("a", 0, 0, 100, 4).polygon, 40);
    check(sq.size() == 40, "resampled to a fixed number of points");
    check(MaskStabilizer::resample({{0, 0}, {1, 1}}, 40).empty(), "fewer than 3 points gives no outline");

    // ---- a new object is drawn at once and exactly as detected
    {
        MaskStabilizer s;
        const auto out = s.update(0, {blob("Elevators", 600, 300, 80, 12)}, 1280);
        check(out.size() == 1 && out[0].polygon.size() == 12, "a new object appears at once, as detected");
    }

    // ---- an object missing for two frames stays on screen (no blinking), then goes after the hold time
    {
        MaskStabilizer s;
        std::int64_t t = 0;
        std::vector<int> counts;
        for (int f = 0; f < 12; ++f) {
            t += 100;
            const bool missing = (f == 4 || f == 5);
            counts.push_back(static_cast<int>(s.update(t, missing ? std::vector<DrawItem>{} : std::vector<DrawItem>{blob("Elevators", 600, 300, 80, 12)}, 1280).size()));
        }
        check(counts == std::vector<int>(12, 1), "two missed frames do not blink the mask");
        // gone for good: held for 500 ms, then removed
        std::vector<std::size_t> after;
        for (int f = 0; f < 8; ++f) { t += 100; after.push_back(s.update(t, {}, 1280).size()); }
        check(after[0] == 1 && after[3] == 1 && after[4] == 1, "held through the hold time");
        check(after[5] == 0 && after[7] == 0, "removed once the hold time has passed");
        check(s.tracked() == 0, "the memory is released");
    }

    // ---- shimmer: jittering outlines move far less after smoothing, and the area stays steady
    {
        std::mt19937 rng(11);
        MaskStabilizer s;
        float raw_rough = 0, smooth_rough = 0;
        float min_area = 1e30f, max_area = 0;
        for (int f = 0; f < 60; ++f) {
            const DrawItem raw = blob("Machine", 640, 360, 90, 40, f * 7, f % 2 == 1, 6.0f, &rng); // different start vertex and winding every frame
            const auto out = s.update(f * 100, {raw}, 1280);
            check(out.size() == 1, "one object in, one object out");
            if (f > 5) {
                raw_rough += roughness(raw);
                smooth_rough += roughness(out[0]);
                min_area = std::min(min_area, area(out[0]));
                max_area = std::max(max_area, area(out[0]));
            }
        }
        check(smooth_rough < raw_rough * 0.8, "the smoothed outline is smoother than the raw one (less shimmer)");
        check(max_area < min_area * 1.25 && min_area > 15000, "the outline does not spin or collapse (area stays steady, a circle of r=90 is about 25000)");
    }

    // ---- a moving object follows its motion with a small lag
    {
        MaskStabilizer s;
        float last_x = 0;
        for (int f = 0; f < 30; ++f) {
            const auto out = s.update(f * 100, {blob("Derrickman", 300.0f + 20.0f * f, 400, 40, 16)}, 1280);
            last_x = centroid(out[0]).first;
        }
        const float truth = 300.0f + 20.0f * 29;
        check(last_x < truth && truth - last_x < 40, "a moving object trails its true position by less than two frames of travel");
    }

    // ---- two instances of one class and different classes are tracked separately
    {
        MaskStabilizer s;
        const auto a = s.update(0, {blob("Derrickman", 200, 400, 40, 16), blob("Derrickman", 900, 400, 40, 16), blob("Machine", 200, 400, 60, 16)}, 1280);
        check(a.size() == 3, "three objects");
        const auto b = s.update(100, {blob("Derrickman", 905, 402, 40, 16), blob("Derrickman", 205, 398, 40, 16), blob("Machine", 205, 400, 60, 16)}, 1280);
        check(b.size() == 3 && s.tracked() == 3, "listed in another order, still matched to the right memory (no extra objects)");
        // only the left derrickman disappears: it is held, the right one and the machine are untouched
        const auto c = s.update(200, {blob("Derrickman", 905, 402, 40, 16), blob("Machine", 205, 400, 60, 16)}, 1280);
        check(c.size() == 3, "the missing one is held, the others are drawn");
    }

    // ---- box only classes (no polygon) are smoothed and held too
    {
        MaskStabilizer s;
        DrawItem box;
        box.label = "helmet";
        box.box[0] = 100; box.box[1] = 100; box.box[2] = 140; box.box[3] = 140;
        s.update(0, {box}, 1280);
        const auto held = s.update(100, {}, 1280);
        check(held.size() == 1 && held[0].polygon.empty() && held[0].box[0] == 100, "a box-only object is held at its box");
    }

    // ---- the original flags survive (in_roi decides how the overlay draws it)
    {
        MaskStabilizer s;
        DrawItem d = blob("Platform", 400, 500, 120, 20);
        d.in_roi = false;
        const auto out = s.update(0, {d}, 1280);
        check(!out[0].in_roi, "in_roi is kept");
        const auto held = s.update(100, {}, 1280);
        check(held.size() == 1 && !held[0].in_roi && !held[0].held, "a held object keeps its flags and is not marked dashed");
    }

    if (failures) return 1;
    std::cout << "mask_stabilizer tests passed\n";
    return 0;
}
