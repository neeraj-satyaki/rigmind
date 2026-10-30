// Scene memory (pipeline/scene_trail.h) and the summary prompt text (slm/scene_text.h).
#include <cstdlib>
#include <iostream>

#include "pipeline/scene_trail.h"
#include "slm/scene_text.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
rigmind::SceneObs obs(const char* label, float x, float y, int id = 0, bool contour = false) {
    rigmind::SceneObs o;
    o.label = label;
    o.track_id = id;
    o.box[0] = x - 20; o.box[1] = y - 20; o.box[2] = x + 20; o.box[3] = y + 20;
    if (contour) for (int k = 0; k < 40; ++k) o.contour.emplace_back(static_cast<int>(x) + k, static_cast<int>(y) + 2 * k);
    return o;
}
} // namespace

int main() {
    rigmind::SceneTrail trail;
    // A moving elevator (no tracker id) and a tracked helmet: the centroid series grows, ids stay stable.
    for (int i = 0; i < 20; ++i) {
        const std::int64_t t = 1000 + i * 250;
        trail.update("idss", t, {obs("Elevators", 600.0f + 10 * i, 300.0f, 0, true), obs("Platform", 900, 500, 0, true)});
        trail.update("ppe", t, {obs("helmet", 400, 200 + i, 7)});
    }
    const std::int64_t now = 1000 + 19 * 250;
    const auto scene = trail.snapshot("CAM-01", now, 1280, 720, 12, 8);
    check(scene["objects"].size() == 3, "three objects: Elevators, Platform, helmet");
    bool saw_elevator = false, saw_helmet = false;
    for (const auto& o : scene["objects"]) {
        if (o["label"] == "Elevators") {
            saw_elevator = true;
            check(o["id"] == 1, "elevator keeps id 1 while it moves");
            check(o["trail"].size() == 8, "trail thinned to 8 samples");
            check(o["trail"][0][0].get<int>() < o["trail"][7][0].get<int>(), "trail runs oldest to newest, moving right");
            check(o["contour"].size() == 12, "contour thinned to 12 points");
        }
        if (o["label"] == "helmet") {
            saw_helmet = true;
            check(o["id"] == 7, "tracker id is kept");
            check(o["contour"].empty(), "box-only class has no contour");
        }
    }
    check(saw_elevator && saw_helmet, "both classes present");

    // Objects that vanish are reported only while fresh, then dropped.
    check(trail.snapshot("CAM-01", now + 2000, 1280, 720)["objects"].empty(), "stale objects are not reported");
    trail.update("idss", now + 5000, {});
    check(trail.snapshot("CAM-01", now + 5000, 1280, 720)["objects"].empty(), "forgotten after 3 s");

    // Prompt text: class names, positions, outlines and paths, inside the budget.
    const std::string text = rigmind::scene_prompt_text(scene);
    check(text.find("Elevators #1 (idss)") != std::string::npos, "class name and id in text");
    check(text.find("outline") != std::string::npos && text.find("path") != std::string::npos, "outline and path in text");
    check(text.size() <= 2400 + 200, "text within budget");
    nlohmann::json many = scene;
    for (int i = 0; i < 40; ++i) many["objects"].push_back(scene["objects"][0]);
    const std::string capped = rigmind::scene_prompt_text(many, 1500);
    check(capped.size() <= 1500 + 120, "large scene thinned under the budget");
    check(rigmind::scene_prompt_text(nlohmann::json{{"objects", nlohmann::json::array()}}).empty(), "empty scene gives no text");

    if (failures) return 1;
    std::cout << "scene tests passed\n";
    return 0;
}
