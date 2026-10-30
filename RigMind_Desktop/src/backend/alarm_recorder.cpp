#include "alarm_recorder.h"

#include "alert_metadata.h"

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

namespace {

const char* state_label(AlarmState state) {
    return state == AlarmState::None ? "Cleared" : level_label(level_of(state));
}

const char* peak_name(int peak) { return peak >= 2 ? "Red" : peak == 1 ? "Orange" : "Yellow"; }

nlohmann::json fresh_meta(const std::string& cam) {
    return {{"v", 1}, {"kind", "alarm"}, {"camera", cam}, {"peak", "Yellow"}, {"cycle", ""}, {"closed", false},
            {"states", nlohmann::json::array()}};
}

void finish_meta(nlohmann::json& meta, int peak, const std::string& path, bool closed) {
    meta["peak"] = peak_name(peak);
    meta["cycle"] = path;
    meta["closed"] = closed;
}

} // namespace

void AlarmRecorder::note_facts(const nlohmann::json& facts_event, std::int64_t now_ms) {
    if (!facts_event.is_object()) return;
    const std::string cam = facts_event.value("cam", std::string());
    if (safe_cam(cam).empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    facts_[cam] = Facts{trim_facts(facts_event), now_ms};
}

nlohmann::json AlarmRecorder::handle(const nlohmann::json& event, std::int64_t now_ms) {
    nlohmann::json out = event;
    if (!event.is_object() || event.value("ev", std::string()) != "alarm") return out;
    const std::string cam = event.value("cam", std::string());
    if (!event.contains("state") || !event["state"].is_string() || safe_cam(cam).empty()) return out;
    const auto next = parse_state(event["state"].get<std::string>());
    if (!next) return out;

    // Alert rows hold facts and neutral labels only. Any wording the line may carry is ignored: the plain-English
    // description of an alert comes from the SLM, built from the stored metadata.
    const std::string place = clip_text(cam, 128);
    const std::int64_t frame = event.value("frame", static_cast<std::int64_t>(0));

    std::lock_guard<std::mutex> lock(mutex_);
    Camera& c = cameras_[cam];
    const AlarmState from = c.state;
    const bool changed = *next != from;

    if (!changed && c.episode == 0) return out; // None repeated before any episode: nothing to attach

    nlohmann::json closed; // set when this line ends an event cycle
    bool peak_raised = false;
    if (changed) {
        nlohmann::json entry = {{"state", state_name(*next)}, {"tsMs", now_ms}, {"frame", frame}, {"facts", nullptr}};
        const auto known = facts_.find(cam);
        if (known != facts_.end()) {
            entry["facts"] = known->second.facts;
            entry["factsAgeMs"] = std::max<std::int64_t>(0, now_ms - known->second.at_ms);
        }
        const std::string next_label = state_label(*next);
        const auto persist = [&](const std::string& alert_id, const nlohmann::json& meta) {
            try {
                store_.put_alert_metadata(alert_id, meta.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), now_ms);
            } catch (const std::exception& e) {
                std::cerr << "alert metadata failed: " << e.what() << '\n';
            }
        };

        // Event cycle: Yellow -> Orange -> ... -> Yellow. Falling back to Yellow from Orange or Red ends the cycle (its alert
        // row is Cleared with the peak it reached) and that Yellow is the baseline of the next cycle. Reaching None ends it too.
        AlarmState recorded_from = from;
        bool closing = false, pushed_to_current = false;
        const int old_peak = c.peak;
        if (level_of(from) >= 1 && (*next == AlarmState::Yellow || *next == AlarmState::None)) {
            closing = true;
            c.path += *next == AlarmState::Yellow ? ">Y" : ">N";
            const bool critical = c.peak >= 2;
            closed = {{"alertId", c.alert_id}, {"peak", c.peak}, {"critical", critical},
                      {"cycle", c.path}, {"kind", critical ? "critical" : "normal"}};
            const std::string peak_label = level_label(c.peak);
            if (audit_) {
                audit_->record("backend", "alarm.cycle", c.alert_id,
                               clip_text(std::string(critical ? "critical" : "normal") + " cycle " + c.path + " | " + peak_label + " | " + cam, 512),
                               now_ms);
            }
            c.meta["states"].push_back(entry);
            finish_meta(c.meta, c.peak, c.path, true);
            persist(c.alert_id, c.meta);
            if (*next == AlarmState::Yellow) {
                try {
                    EventStore::AlarmEpisodeRecord record;
                    record.cam_id = cam;
                    record.alert_id = c.alert_id;
                    record.from = from;
                    record.to = *next;
                    record.severity = severity_of_level(c.peak);
                    record.status = "Cleared";
                    record.condition = next_label;
                    record.alert_condition = peak_label; // the row keeps its peak level
                    record.place = place;
                    record.frame = frame;
                    record.timestamp_ms = now_ms;
                    store_.record_alarm_episode(record);
                } catch (const std::exception& e) {
                    std::cerr << "alarm record failed: " << e.what() << '\n';
                }
                if (audit_) {
                    audit_->record("backend", "alarm.transition", c.alert_id,
                                   clip_text(std::string(state_name(from)) + " -> Yellow | cycle closed | " + peak_label + " | " + cam, 512),
                                   now_ms);
                }
                recorded_from = AlarmState::None; // the same Yellow opens the next cycle's row
                c.state = AlarmState::None;
            }
        }
        if (recorded_from == AlarmState::None) {
            ++c.episode;
            c.peak = -1;
            c.path.clear();
            c.alert_id = make_alert_id(cam, c.episode);
            c.meta = fresh_meta(cam);
            if (*next != AlarmState::None) {
                c.meta["states"].push_back(entry);
                pushed_to_current = true;
            }
        }
        if (*next != AlarmState::None) c.path += (c.path.empty() ? "" : ">") + std::string(1, *next == AlarmState::Yellow ? 'Y' : *next == AlarmState::Orange ? 'O' : 'R');
        c.peak = std::max(c.peak, level_of(*next));
        peak_raised = c.peak > (recorded_from == AlarmState::None ? -1 : old_peak);
        c.state = *next;
        if (!closing && !pushed_to_current) c.meta["states"].push_back(entry);
        finish_meta(c.meta, c.peak, c.path, *next == AlarmState::None);

        const std::string status = *next == AlarmState::None ? "Cleared" : c.peak >= 1 ? "Escalated" : "Monitoring";
        const std::string severity = severity_of_level(c.peak);
        const std::string row_label = level_label(c.peak);
        try {
            EventStore::AlarmEpisodeRecord record;
            record.cam_id = cam;
            record.alert_id = c.alert_id;
            record.from = recorded_from;
            record.to = *next;
            record.severity = severity;
            record.status = status;
            record.condition = next_label;
            record.alert_condition = row_label;
            record.place = place;
            record.frame = frame;
            record.timestamp_ms = now_ms;
            store_.record_alarm_episode(record);
        } catch (const std::exception& e) {
            std::cerr << "alarm record failed: " << e.what() << '\n';
        }
        persist(c.alert_id, c.meta);
        if (audit_) {
            audit_->record("backend", "alarm.transition", c.alert_id,
                           clip_text(std::string(state_name(recorded_from)) + " -> " + state_name(*next) + " | " + row_label + " | " + cam, 512),
                           now_ms);
        }
    }

    out["alertId"] = c.alert_id;
    out["episode"] = c.episode;
    out["phase"] = c.state == AlarmState::None ? "Cleared" : c.peak >= 1 ? "Escalated" : "Monitoring";
    out["escalation"] = std::max(0, level_of(c.state));
    out["peak"] = c.peak;
    out["changed"] = changed;
    out["peakRaised"] = peak_raised;
    out["condition"] = state_label(*next); // neutral label; the sentence comes from the SLM
    out["place"] = place;
    if (closed.is_object()) out["closed"] = closed;
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
    c.since_ms = event.value("since_ms", static_cast<std::int64_t>(0));
    // 'not detected', never 'not worn': the camera may simply not see the item. A cleared row keeps the last missing set.
    // The row holds a neutral label; the items and the people count are metadata, the sentence comes from the SLM.
    const std::string label = "PPE items not detected";
    std::string list;
    for (const auto& m : c.missing) list += (list.empty() ? "" : ", ") + m;
    if (changed) {
        const std::string status = c.open ? "Open" : "Cleared";
        try {
            store_.upsert_alert(c.alert_id, "Advisory", label, clip_text(cam, 128), status, now_ms);
            const nlohmann::json meta = {{"v", 1}, {"kind", "ppe"}, {"camera", cam}, {"missing", c.missing}, {"persons", c.persons},
                                         {"since_ms", c.since_ms}, {"closed", !c.open}};
            store_.put_alert_metadata(c.alert_id, meta.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), now_ms);
        } catch (const std::exception& e) {
            std::cerr << "ppe record failed: " << e.what() << '\n';
        }
        if (audit_) {
            audit_->record("backend", "ppe.transition", c.alert_id,
                           clip_text(action + " | " + label + ": " + list + " | persons " + std::to_string(persons) + " | " + cam, 512),
                           now_ms);
        }
    }
    out["alertId"] = c.alert_id;
    out["episode"] = c.episode;
    out["phase"] = c.open ? "Open" : "Cleared";
    out["condition"] = label;
    out["place"] = clip_text(cam, 128);
    out["severity"] = "Advisory";
    out["action"] = changed ? action : "";
    out["changed"] = changed;
    return out;
}

} // namespace rigmind
