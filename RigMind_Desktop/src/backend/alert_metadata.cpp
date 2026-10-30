#include "alert_metadata.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>
#include <iomanip>
#include <sstream>

namespace rigmind {

namespace {

using json = nlohmann::json;

bool flag(const json& o, const char* key) { return o.is_object() && o.contains(key) && o[key].is_boolean() && o[key].get<bool>(); }

bool number(const json& o, const char* key, double& out) {
    if (!o.is_object() || !o.contains(key) || !o[key].is_number()) return false;
    out = o[key].get<double>();
    return std::isfinite(out);
}

std::string percent(double value, double whole) {
    if (whole <= 0) return {};
    const long p = std::lround(100.0 * value / whole);
    return std::to_string(std::max(0L, std::min(100L, p))) + "%";
}

std::string clock_text(std::int64_t ts_ms, const char* format) {
    const std::time_t seconds = static_cast<std::time_t>(ts_ms / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds) != 0) return {};
#else
    if (!localtime_r(&seconds, &local)) return {};
#endif
    std::ostringstream out;
    out << std::put_time(&local, format);
    return out.str();
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : sep) + p;
    return out;
}

const char* state_word(const std::string& s) { return s == "None" ? "no alarm" : s.c_str(); }

std::string cycle_words(const std::string& path) {
    std::string out;
    for (char c : path) {
        if (c == '>') continue;
        const char* word = c == 'Y' ? "Yellow" : c == 'O' ? "Orange" : c == 'R' ? "Red" : c == 'N' ? "no alarm" : nullptr;
        if (word) out += (out.empty() ? "" : " > ") + std::string(word);
    }
    return out;
}

} // namespace

json trim_facts(const json& e) {
    json out = json::object();
    if (!e.is_object()) return out;
    for (const char* key : {"machine_present", "link_tilt_present", "link_tilt_angled", "elevator_present", "elevator_near_platform", "zoomed"})
        if (e.contains(key) && e[key].is_boolean()) out[key] = e[key];
    for (const char* key : {"angle_deg", "derrickman_cx", "machine_cx", "w", "h"})
        if (e.contains(key) && (e[key].is_number() || e[key].is_null())) out[key] = e[key];
    if (e.contains("roi") && (e["roi"].is_array() || e["roi"].is_null())) out["roi"] = e["roi"];
    if (e.contains("pct") && e["pct"].is_object()) out["pct"] = e["pct"];
    return out;
}

const char* level_label(int level) { return level >= 2 ? "Red alert" : level == 1 ? "Orange alert" : "Yellow monitoring"; }

std::string facts_list(const json& f) {
    if (!f.is_object()) return {};
    std::vector<std::string> items;
    double v = 0, w = 0;
    if (flag(f, "machine_present")) items.push_back("machine present");
    if (flag(f, "link_tilt_present")) items.push_back("link tilt present");
    if (flag(f, "link_tilt_angled")) {
        items.push_back(number(f, "angle_deg", v) ? "link tilt angled " + std::to_string(std::lround(std::fabs(v))) + " degrees" : "link tilt angled");
    } else if (flag(f, "link_tilt_present") && number(f, "angle_deg", v)) {
        items.back() = "link tilt present, " + std::to_string(std::lround(std::fabs(v))) + " degrees";
    }
    if (flag(f, "elevator_present")) items.push_back("elevator present");
    if (flag(f, "elevator_near_platform")) items.push_back("elevator near platform");
    if (number(f, "w", w)) {
        if (number(f, "derrickman_cx", v)) items.push_back("derrickman at " + percent(v, w) + " of the frame width");
        if (number(f, "machine_cx", v)) items.push_back("machine at " + percent(v, w) + " of the frame width");
    }
    return join(items, "; ");
}

std::string describe_prompt(const json& meta, const std::string& severity, const std::string& label, const std::string& camera,
                            std::int64_t ts_ms) {
    // The model answers a data list with a markdown report, so the facts are handed over as plain running text. The facts
    // themselves (and their values) come from the stored metadata only.
    std::string data;
    const std::string kind = meta.is_object() ? meta.value("kind", std::string()) : std::string();
    const std::string cam = !camera.empty() ? camera : (meta.is_object() ? meta.value("camera", std::string()) : std::string());
    const std::string who = cam.empty() ? std::string("The camera") : "Camera " + cam;
    if (kind == "alarm") {
        const std::string cycle = cycle_words(meta.value("cycle", std::string()));
        data = who + " reached the " + meta.value("peak", std::string("Yellow")) + " level";
        if (!cycle.empty()) data += " (" + cycle + (meta.value("closed", false) ? ", finished" : ", still going") + ")";
        if (!severity.empty()) data += "; severity " + severity;
        data += ". ";
        for (const json& s : meta.value("states", json::array())) {
            const std::string state = s.value("state", std::string());
            const std::string list = facts_list(s.value("facts", json()));
            data += "At " + clock_text(s.value("tsMs", std::int64_t{0}), "%d %b %Y %H:%M:%S") + " (" + state_word(state) + ")";
            data += list.empty() ? std::string(" it had no EWS fact to report. ") : " it saw: " + list + ". ";
        }
    } else if (kind == "ppe") {
        std::vector<std::string> missing;
        for (const json& m : meta.value("missing", json::array()))
            if (m.is_string()) missing.push_back(m.get<std::string>());
        data = who + " did not detect these PPE items: " + (missing.empty() ? std::string("none") : join(missing, ", ")) + ", with " +
               std::to_string(meta.value("persons", 0)) + " people in view, at " + clock_text(ts_ms, "%d %b %Y %H:%M") + ". ";
    } else {
        data = who + " raised a " + (label.empty() ? std::string("safety") : label) + " (severity " + severity + ") at " +
               clock_text(ts_ms, "%d %b %Y %H:%M") + ". ";
    }
    return data +
           "Write one or two plain sentences for a site supervisor. Name the camera, say what happened and how it changed "
           "(use the times, the angle and the positions given), and end with the action the supervisor must take (for example "
           "check the area, stop the work, or confirm that everyone wears the required PPE). When a scene list follows, use only "
           "what it shows; never invent anything that is not given. Do not use lists, bold text or headings. No jargon.";
}

} // namespace rigmind
