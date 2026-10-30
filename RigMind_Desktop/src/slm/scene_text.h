// Turns a pipeline "scene" event (pipeline/scene_trail.h) into the compact text block of the summary prompt:
// for every class its name, position, mask contour points and centroid trajectory, as percent of the frame
// (x to the right, y downward). Header-only. The block is kept under a character budget by thinning the contour
// and trail points, never by dropping a class.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

namespace rigmind {

namespace scene_detail {

inline int pct(double v, double whole) { return whole > 0 ? static_cast<int>(std::lround(100.0 * v / whole)) : 0; }

inline std::string render(const nlohmann::json& scene, std::size_t contour_n, std::size_t trail_n) {
    const double w = scene.value("w", 0), h = scene.value("h", 0);
    std::string out;
    for (const auto& o : scene.at("objects")) {
        const auto& box = o.at("box");
        const auto& c = o.at("centroid");
        out += "- " + o.value("label", std::string("object")) + " #" + std::to_string(o.value("id", 0)) + " (" + o.value("model", std::string()) +
               "): centre " + std::to_string(pct(c[0].get<double>(), w)) + "," + std::to_string(pct(c[1].get<double>(), h)) +
               ", box " + std::to_string(pct(box[0].get<double>(), w)) + "," + std::to_string(pct(box[1].get<double>(), h)) + " to " +
               std::to_string(pct(box[2].get<double>(), w)) + "," + std::to_string(pct(box[3].get<double>(), h));
        const auto& contour = o.at("contour");
        if (contour_n > 0 && !contour.empty()) {
            out += ", outline";
            const std::size_t n = contour.size();
            const std::size_t take = (std::min)(n, contour_n);
            for (std::size_t k = 0; k < take; ++k) {
                const auto& p = contour[take == 1 ? 0 : k * (n - 1) / (take - 1)];
                out += " " + std::to_string(pct(p[0].get<double>(), w)) + "," + std::to_string(pct(p[1].get<double>(), h));
            }
        }
        const auto& trail = o.at("trail");
        if (trail_n > 0 && trail.size() > 1) {
            out += ", path";
            const std::size_t n = trail.size();
            const std::size_t take = (std::min)(n, trail_n);
            for (std::size_t k = 0; k < take; ++k) {
                const auto& p = trail[take == 1 ? n - 1 : k * (n - 1) / (take - 1)];
                out += " " + std::to_string(pct(p[0].get<double>(), w)) + "," + std::to_string(pct(p[1].get<double>(), h));
            }
            const double span = trail[0][2].get<double>() / 1000.0;
            out += " over " + std::to_string(static_cast<int>(std::lround(span))) + "s";
        }
        out += "\n";
    }
    return out;
}

} // namespace scene_detail

// Empty when the scene has no objects. `max_chars` bounds the block (about 2.5 characters per token on the 2B model).
inline std::string scene_prompt_text(const nlohmann::json& scene, std::size_t max_chars = 2400) {
    if (!scene.is_object() || !scene.contains("objects") || !scene["objects"].is_array() || scene["objects"].empty()) return {};
    static const std::size_t kSteps[][2] = {{8, 6}, {6, 5}, {4, 4}, {3, 3}, {0, 3}, {0, 0}};
    std::string text;
    for (const auto& step : kSteps) {
        text = scene_detail::render(scene, step[0], step[1]);
        if (text.size() <= max_chars) break;
    }
    if (text.size() > max_chars) text.resize(text.find_last_of('\n', max_chars) == std::string::npos ? max_chars : text.find_last_of('\n', max_chars) + 1);
    return "Scene now, in percent of the frame (x to the right, y downward; path is oldest to newest):\n" + text;
}

} // namespace rigmind
