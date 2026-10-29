#include "bridge.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace rigmind {

namespace {

using json = nlohmann::json;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

FrameFacts facts_from_json(const json& p) {
    FrameFacts f;
    f.machine_present = p.value("machinePresent", false);
    f.link_tilt_present = p.value("linkTiltPresent", false);
    f.link_tilt_angled = p.value("linkTiltAngled", false);
    f.elevator_present = p.value("elevatorPresent", false);
    f.elevator_near_platform = p.value("elevatorNearPlatform", false);
    f.platform_elevator_collision = p.value("platformElevatorCollision", false);
    f.derrickman_out_of_safe_zone = p.value("derrickmanOutOfSafeZone", false);
    f.machine_past_right_edge = p.value("machinePastRightEdge", false);
    f.link_tilt_near_derrickman = p.value("linkTiltNearDerrickman", false);
    return f;
}

std::string require_string(const json& p, const char* key, std::size_t max_len) {
    const std::string value = p.at(key).get<std::string>();
    if (value.empty() || value.size() > max_len) throw std::runtime_error(std::string("invalid ") + key);
    return value;
}

} // namespace

Bridge::Bridge(EventStore& store, StreamSink* stream, AuditLog* audit)
    : store_(store), stream_(stream), audit_(audit) {}

void Bridge::audit_event(const std::string& action, const std::string& target, const std::string& detail) {
    if (audit_) audit_->record("backend", action, target, detail, now_ms());
}

// Plain-language facts for one event: when, where, what, and what the severity means for the supervisor.
// Built from metadata only. The SLM rewrites this text when it is running; this is the fallback.
std::string plain_facts(const std::string& severity, const std::string& condition, const std::string& place,
                        const std::string& cam, std::int64_t ts_ms) {
    const std::time_t seconds = static_cast<std::time_t>(ts_ms / 1000);
    const std::tm* local = std::localtime(&seconds);
    std::ostringstream when;
    if (local) when << std::put_time(local, "%d %b %Y %H:%M");
    const char* meaning = severity == "Critical" ? "needs immediate action"
                          : severity == "Warning" ? "needs attention soon"
                          : "is for information";
    std::string text = "At " + when.str() + ", ";
    text += cam.empty() ? std::string("an event") : "camera " + cam;
    if (!place.empty()) text += " at " + place;
    text += " recorded: " + condition + ". This " + severity + " alert " + meaning + ".";
    return text;
}

// Trims to at most max_bytes without splitting a UTF-8 sequence.
std::string clip(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

// Replays a scripted detection sequence through the EWS state machine until the scenario state is reached:
// "yellow" -> "orange" -> "red". Every transition is stored as an alarm event and as an alert, and returned
// so the UI can raise it (toast, bell, tone). Starts from a fresh state machine.
json Bridge::simulate(const json& params) {
    const std::string scenario = require_string(params, "scenario", 16);
    const std::string place = require_string(params, "place", 128);

    AlarmState target = AlarmState::None;
    if (scenario == "yellow") target = AlarmState::Yellow;
    else if (scenario == "orange") target = AlarmState::Orange;
    else if (scenario == "red") target = AlarmState::Red;
    else throw std::runtime_error("invalid scenario");

    FrameFacts tilt;
    tilt.machine_present = true;
    tilt.link_tilt_present = true;
    tilt.link_tilt_angled = true;
    FrameFacts near = tilt;
    near.elevator_present = true;
    near.elevator_near_platform = true;
    FrameFacts collide = near;
    collide.platform_elevator_collision = true;

    alarm_ = AlarmStateMachine(AlarmThresholds{});
    json alerts = json::array();
    for (int frame = 0; frame < 1000 && alarm_.current() != target; ++frame) {
        const AlarmState before = alarm_.current();
        // Yellow phase builds the tilt counter, then near-platform, then collision.
        const FrameFacts& facts = before == AlarmState::None ? tilt
                                  : before == AlarmState::Yellow ? near
                                  : collide;
        const AlarmState after = alarm_.step(facts);
        if (after == before) continue;

        const std::int64_t ts = now_ms();
        store_.record_transition(before, after, ts);
        const char* severity = after == AlarmState::Red ? "Critical"
                               : after == AlarmState::Orange ? "Warning" : "Advisory";
        const char* condition = after == AlarmState::Red ? "Platform-elevator collision"
                                : after == AlarmState::Orange ? "Elevator near platform" : "Link-tilt angled";
        const std::string id = "EV-" + std::to_string(next_event_++);
        store_.raise_alert(id, severity, condition, place, ts);
        alerts.push_back({{"id", id}, {"severity", severity}, {"condition", condition}, {"place", place}});
    }
    if (alarm_.current() != target) throw std::runtime_error("scenario did not reach target state");
    audit_event("alarm.simulate", scenario, place);

    // Red with a camera: ask the pipeline to save the latest clips for that camera.
    const std::string cam = params.value("cam", std::string());
    const bool clip_requested = target == AlarmState::Red && !cam.empty() && stream_ != nullptr;
    if (clip_requested) {
        stream_->send({{"cmd", "clip"}, {"cam", require_string(params, "cam", 32)}});
        audit_event("footage.request", cam, "red");
    }
    return {{"state", to_string(alarm_.current())}, {"alerts", alerts}, {"clipRequested", clip_requested}};
}

std::string Bridge::handle(const std::string& request_json) {
    json id;
    try {
        const json req = json::parse(request_json);
        id = req.value("id", json(nullptr));
        const std::string method = req.at("method").get<std::string>();
        const json params = req.value("params", json::object());

        json result;
        if (method == "app.version") {
            result = {{"name", "RigMind"}, {"native", true}};

        // Alarm state machine (IDSS EWS port)
        } else if (method == "alarm.step") {
            const AlarmState before = alarm_.current();
            const AlarmState after = alarm_.step(facts_from_json(params));
            if (after != before) store_.record_transition(before, after, now_ms());
            result = {{"state", to_string(after)}, {"changed", after != before}};
        } else if (method == "event.describe") {
            // Metadata -> plain language. Returns the fallback text now; the SLM text arrives later as a
            // "description" event, keyed by alertId.
            const std::string alert_id = require_string(params, "alertId", 32);
            const std::string severity = require_string(params, "severity", 16);
            const std::string condition = require_string(params, "condition", 128);
            const std::string place = clip(params.value("place", std::string()), 128);
            const std::string cam = clip(params.value("camId", std::string()), 32);
            const std::int64_t ts = params.value("tsMs", now_ms());
            const std::string facts = plain_facts(severity, condition, place, cam, ts);
            // Prompt contract (checked by tests/slm/prompt_harness.py): the event facts plus what the sentence
            // must contain. The chat system message (host and harness) fixes the output format.
            const std::string prompt =
                "Event: " + facts + "\nWrite one plain sentence for a site supervisor. Name the camera and the place, "
                "say what happened, and say what must be done. No jargon.";;
            const bool requested = stream_ != nullptr;
            if (requested) {
                stream_->send({{"cmd", "describe"}, {"alertId", alert_id}, {"prompt", prompt},
                               {"severity", severity}, {"camId", cam}, {"place", place}});
            }
            audit_event("event.describe", alert_id, requested ? "slm" : "template");
            result = {{"alertId", alert_id}, {"text", facts}, {"slm", requested}};
        } else if (method == "footage.list") {
            result = store_.list_footage(std::clamp(params.value("limit", 50), 1, 500));
        } else if (method == "footage.files") {
            result = store_.list_footage_files(require_string(params, "path", 64));
        } else if (method == "footage.record") {
            // Sent by the host when the pipeline finishes a clip: {cam, stamp, count}.
            const std::string cam = require_string(params, "cam", 32);
            const std::string stamp = require_string(params, "stamp", 20);
            const int count = params.at("count").get<int>();
            if (count < 0 || count > 1000) throw std::runtime_error("invalid count");
            store_.record_footage(cam, std::stoll(stamp), count, cam + "/" + stamp);
            audit_event("footage.save", cam, std::to_string(count) + " clips");
            result = {{"pruned", store_.prune_footage(now_ms())}};
        } else if (method == "audit.write") {
            // UI actions: batch of {ts, action, target, detail}. Overlong text is clipped, not rejected.
            if (!audit_) throw std::runtime_error("audit log not available");
            const json& entries = params.at("entries");
            if (!entries.is_array() || entries.size() > 100) throw std::runtime_error("invalid entries");
            for (const json& e : entries) {
                const std::string action = require_string(e, "action", 64);
                const std::string target = clip(e.value("target", std::string()), 128);
                const std::string detail = clip(e.value("detail", std::string()), 512);
                audit_->record("ui", action, target, detail, e.value("ts", now_ms()));
            }
            result = {{"accepted", entries.size()}};
        } else if (method == "alarm.simulate") {
            result = simulate(params);
        } else if (method == "alarm.reset") {
            alarm_ = AlarmStateMachine(AlarmThresholds::for_zoom(params.value("zoom", false)));
            result = {{"state", to_string(alarm_.current())}};
        } else if (method == "events.recent") {
            result = store_.recent_transitions(std::clamp(params.value("limit", 50), 1, 500));

        // Safety alerts: toast, bell and push are shown by the UI from these rows
        } else if (method == "alert.raise") {
            store_.raise_alert(require_string(params, "id", 32), require_string(params, "severity", 16),
                               require_string(params, "condition", 128), require_string(params, "place", 128),
                               now_ms());
            result = {{"id", params.at("id")}};
        } else if (method == "alert.respond") {
            const std::string status = require_string(params, "status", 16);
            if (status != "Open" && status != "Acknowledged" && status != "Escalated")
                throw std::runtime_error("invalid status");
            const std::string alert_id = require_string(params, "id", 32);
            store_.respond_alert(alert_id, status, require_string(params, "owner", 64), now_ms());
            audit_event("alert.respond", alert_id, status);
            result = {{"id", params.at("id")}, {"status", status}};
        } else if (method == "alerts.list") {
            result = store_.list_alerts();
        } else if (method == "alerts.feed") {
            // Raised alerts with status and owner; the UI polls this for the live alert list.
            result = store_.list_raised_alerts(std::clamp(params.value("limit", 100), 1, 500));

        // Live video: WebRTC session with the rigmind-pipeline process (vanilla ICE, no decode in the host)
        } else if (method == "stream.open") {
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            const std::string uri = require_string(params, "uri", 512);
            // Only RTSP sources; keeps the pipeline from opening files or other schemes.
            if (uri.rfind("rtsp://", 0) != 0) throw std::runtime_error("invalid uri");
            const std::string cam = require_string(params, "cam", 32);
            stream_->send({{"cmd", "open"}, {"cam", cam}, {"uri", uri}});
            audit_event("stream.open", cam, "");
            result = {{"accepted", true}};
        } else if (method == "stream.offer") {
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            stream_->send({{"cmd", "offer"}, {"cam", require_string(params, "cam", 32)},
                           {"sdp", require_string(params, "sdp", 16384)}});
            result = {{"accepted", true}};
        } else if (method == "stream.close") {
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            const std::string cam = require_string(params, "cam", 32);
            stream_->send({{"cmd", "close"}, {"cam", cam}});
            audit_event("stream.close", cam, "");
            result = {{"accepted", true}};

        // Key/value records behind preferences, camera details, HSE decisions and report summaries
        } else if (method == "records.get") {
            const auto value = store_.get_record(require_string(params, "key", 256));
            result = value ? json::parse(*value) : json(nullptr);
        } else if (method == "records.set") {
            store_.put_record(require_string(params, "key", 256), params.at("value").dump());
            result = {{"ok", true}};
        } else if (method == "records.list") {
            result = store_.list_records(params.value("prefix", std::string()));

        } else {
            throw std::runtime_error("unknown method: " + method);
        }
        return json{{"id", id}, {"ok", true}, {"result", result}}.dump();
    } catch (const std::exception& e) {
        return json{{"id", id}, {"ok", false}, {"error", e.what()}}.dump();
    }
}

} // namespace rigmind
