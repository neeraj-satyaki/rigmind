#include "alarm_recorder.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>

namespace rigmind {

namespace {

constexpr std::size_t kMaxAlertId = 32;      // bridge.cpp require_string(.., "id", 32)
constexpr std::size_t kBootLength = 5;       // base36, 36^5 seconds is about 1.9 years before the nonce repeats

std::optional<AlarmState> parse_state(const std::string& text) {
    if (text == "None") return AlarmState::None;
    if (text == "Yellow") return AlarmState::Yellow;
    if (text == "Orange") return AlarmState::Orange;
    if (text == "Red") return AlarmState::Red;
    return std::nullopt;
}

const char* state_name(AlarmState state) {
    switch (state) {
    case AlarmState::Yellow: return "Yellow";
    case AlarmState::Orange: return "Orange";
    case AlarmState::Red: return "Red";
    default: return "None";
    }
}

int level_of(AlarmState state) {
    switch (state) {
    case AlarmState::Yellow: return 0;
    case AlarmState::Orange: return 1;
    case AlarmState::Red: return 2;
    default: return -1;
    }
}

const char* severity_of_level(int level) {
    return level >= 2 ? "Critical" : level == 1 ? "Warning" : "Advisory";
}

std::string clip_text(const std::string& text, std::size_t max) {
    return text.size() <= max ? text : text.substr(0, max);
}

std::string base36(std::uint64_t value, std::size_t width) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string out(width, '0');
    for (std::size_t i = width; i-- > 0;) {
        out[i] = digits[value % 36];
        value /= 36;
    }
    return out;
}

// Camera ids keep letters, digits, '-' and '_' only.
std::string safe_cam(const std::string& cam) {
    std::string out;
    for (char c : cam) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out.push_back(c);
    }
    return out;
}

} // namespace

AlarmRecorder::AlarmRecorder(EventStore& store, AuditLog* audit, std::string boot)
    : store_(store), audit_(audit), boot_(std::move(boot)) {
    if (boot_.empty()) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        boot_ = base36(static_cast<std::uint64_t>(now), kBootLength);
    }
    boot_ = clip_text(safe_cam(boot_), 8);
}

namespace {
std::string prefixed_alert_id(const char* prefix, const std::string& cam, const std::string& boot, int episode) {
    const std::string suffix = boot + std::to_string(episode);
    // "<prefix>-" + cam + "-" + suffix must fit the 32-char alert id limit. A long camera id is shortened and
    // keeps a 4-char hash so two long ids do not collide.
    const std::size_t room = kMaxAlertId - 4 - 1 - suffix.size();
    std::string name = safe_cam(cam);
    if (name.size() > room) {
        const std::size_t keep = room > 5 ? room - 5 : 0;
        name = name.substr(0, keep) + "~" + base36(std::hash<std::string>{}(cam), 4);
    }
    return std::string(prefix) + "-" + name + "-" + suffix;
}
} // namespace

std::string AlarmRecorder::make_alert_id(const std::string& cam, int episode) const {
    return prefixed_alert_id("ALM", cam, boot_, episode);
}

nlohmann::json AlarmRecorder::handle(const nlohmann::json& event, std::int64_t now_ms) {
    nlohmann::json out = event;
    if (!event.is_object() || event.value("ev", std::string()) != "alarm") return out;
    const std::string cam = event.value("cam", std::string());
    if (!event.contains("state") || !event["state"].is_string() || safe_cam(cam).empty()) return out;
    const auto next = parse_state(event["state"].get<std::string>());
    if (!next) return out;

    std::string condition = clip_text(event.value("condition", std::string()), 128);
    std::string place = clip_text(event.value("place", std::string()), 128);
    const std::int64_t frame = event.value("frame", static_cast<std::int64_t>(0));

    std::lock_guard<std::mutex> lock(mutex_);
    Camera& c = cameras_[cam];
    const AlarmState from = c.state;
    const bool changed = *next != from;

    if (!changed && c.episode == 0) return out; // None repeated before any episode: nothing to attach

    if (changed) {
        if (from == AlarmState::None) {
            ++c.episode;
            c.peak = -1;
            c.alert_id = make_alert_id(cam, c.episode);
        }
        c.peak = std::max(c.peak, level_of(*next));
        if (!condition.empty()) c.condition = condition;
        if (!place.empty()) c.place = place;
        c.state = *next;

        const std::string status = *next == AlarmState::None ? "Cleared" : c.peak >= 1 ? "Escalated" : "Monitoring";
        const std::string severity = severity_of_level(c.peak);
        const std::string& shown_condition = condition.empty() ? c.condition : condition;
        const std::string& shown_place = place.empty() ? c.place : place;
        try {
            EventStore::AlarmEpisodeRecord record;
            record.cam_id = cam;
            record.alert_id = c.alert_id;
            record.from = from;
            record.to = *next;
            record.severity = severity;
            record.status = status;
            record.condition = shown_condition;
            record.place = shown_place;
            record.frame = frame;
            record.timestamp_ms = now_ms;
            store_.record_alarm_episode(record);
        } catch (const std::exception& e) {
            std::cerr << "alarm record failed: " << e.what() << '\n';
        }
        if (audit_) {
            audit_->record("backend", "alarm.transition", c.alert_id,
                           clip_text(std::string(state_name(from)) + " -> " + state_name(*next) + " | " + shown_condition +
                                         " | " + shown_place + " | " + cam,
                                     512),
                           now_ms);
        }
    }

    out["alertId"] = c.alert_id;
    out["episode"] = c.episode;
    out["phase"] = c.state == AlarmState::None ? "Cleared" : c.peak >= 1 ? "Escalated" : "Monitoring";
    out["escalation"] = std::max(0, level_of(c.state));
    out["peak"] = c.peak;
    out["changed"] = changed;
    return out;
}

PpeRecorder::PpeRecorder(EventStore& store, AuditLog* audit, std::string boot)
    : store_(store), audit_(audit), boot_(std::move(boot)) {
    if (boot_.empty()) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        boot_ = base36(static_cast<std::uint64_t>(now), kBootLength);
    }
    boot_ = clip_text(safe_cam(boot_), 8);
}

nlohmann::json PpeRecorder::handle(const nlohmann::json& event, std::int64_t now_ms) {
    nlohmann::json out = event;
    if (!event.is_object() || event.value("ev", std::string()) != "ppe") return out;
    const std::string cam = event.value("cam", std::string());
    const std::string state = event.value("state", std::string());
    if (safe_cam(cam).empty() || (state != "missing" && state != "ok")) return out;

    std::vector<std::string> missing;
    if (event.contains("missing") && event["missing"].is_array()) {
        for (const auto& m : event["missing"])
            if (m.is_string()) missing.push_back(clip_text(m.get<std::string>(), 24));
    }
    const int persons = event.value("persons", 0);

    std::lock_guard<std::mutex> lock(mutex_);
    Camera& c = cameras_[cam];
    bool changed = false;
    std::string action;
    if (state == "missing") {
        if (missing.empty()) return out;
        if (!c.open) {
            ++c.episode;
            c.open = true;
            c.alert_id = prefixed_alert_id("PPE", cam, boot_, c.episode);
            c.missing = missing;
            changed = true;
            action = "raised";
        } else if (missing != c.missing) {
            c.missing = missing;
            changed = true;
            action = "updated";
        }
    } else {
        if (!c.open) return out; // an 'ok' with no open episode: nothing to attach
        c.open = false;
        changed = true;
        action = "cleared";
    }
    c.persons = persons;
    // 'not detected', never 'not worn': the camera may simply not see the item. A cleared row keeps the last missing set.
    std::string list;
    for (const auto& m : c.missing) list += (list.empty() ? "" : ", ") + m;
    c.condition = clip_text("PPE not detected: " + list, 128);
    if (changed) {
        const std::string status = c.open ? "Open" : "Cleared";
        try {
            store_.upsert_alert(c.alert_id, "Advisory", c.condition, clip_text(cam, 128), status, now_ms);
        } catch (const std::exception& e) {
            std::cerr << "ppe record failed: " << e.what() << '\n';
        }
        if (audit_) {
            audit_->record("backend", "ppe.transition", c.alert_id,
                           clip_text(action + " | " + c.condition + " | persons " + std::to_string(persons) + " | " + cam, 512),
                           now_ms);
        }
    }
    out["alertId"] = c.alert_id;
    out["episode"] = c.episode;
    out["phase"] = c.open ? "Open" : "Cleared";
    out["condition"] = c.condition;
    out["severity"] = "Advisory";
    out["changed"] = changed;
    return out;
}

} // namespace rigmind
