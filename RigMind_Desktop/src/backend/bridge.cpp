#include "bridge.h"

#include <ctime>

#include "evidence_query.h"

#include "backend/alert_metadata.h"

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
    : store_(store), stream_(stream), audit_(audit), auth_(store, audit), chats_(store) {}

void Bridge::audit_event(const std::string& action, const std::string& target, const std::string& detail) {
    if (audit_) audit_->record("backend", action, target, detail, now_ms());
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
        const char* severity = after == AlarmState::Red ? "Critical"
                               : after == AlarmState::Orange ? "Warning" : "Advisory";
        const char* condition = after == AlarmState::Red ? "Platform-elevator collision"
                                : after == AlarmState::Orange ? "Elevator near platform" : "Link-tilt angled";
        const std::string id = "EV-" + std::to_string(next_event_++);
        const std::string cam_for_row = params.value("cam", std::string());
        const EventStore::TransitionDetail detail{id, severity, condition, place, cam_for_row};
        store_.record_transition(before, after, ts, &detail); // the row carries the alert id (joins to alerts.feed)
        store_.raise_alert(id, severity, condition, place, ts);
        // Same audit row the pipeline path (AlarmRecorder) writes, so every stored transition has one.
        audit_event("alarm.transition", id, clip(std::string(to_string(before)) + " -> " + to_string(after) + " | " + condition + " | " + place + " | " + cam_for_row, 512));
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

namespace {
std::int64_t local_midnight_ms(std::int64_t now) {
    std::time_t t = static_cast<std::time_t>(now / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm)) * 1000;
}
// "30 Oct 14:22" in local time
std::string local_stamp(std::int64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d %b %H:%M", &tm);
    return buf;
}
} // namespace

bool Bridge::authenticated() const {
    return !auth_required_ || (session_ && session_->expires_ms > now_ms());
}

// What the sign-in page itself needs before anyone is signed in: the sign-in calls, the app version, and the saved theme.
bool Bridge::allowed_before_sign_in(const std::string& method, const json& params) {
    if (method == "auth.status" || method == "auth.setup" || method == "auth.login" || method == "app.version") return true;
    if ((method == "records.get" || method == "records.set") && params.is_object() && params.value("key", std::string()) == "preferences") return true;
    return false;
}

void Bridge::start_session(const json& user) {
    session_ = Session{user.value("username", std::string()), user.value("role", std::string()), now_ms() + kSessionMs};
}

std::string Bridge::handle_impl(const std::string& request_json, bool trusted) {
    json id;
    try {
        const json req = json::parse(request_json);
        id = req.value("id", json(nullptr));
        const std::string method = req.at("method").get<std::string>();
        const json params = req.value("params", json::object());
        // Sign-in is enforced here, in the host: a page that skips its own login screen still gets nothing from the bridge.
        if (!trusted && auth_required_ && !authenticated() && !allowed_before_sign_in(method, params)) throw std::runtime_error("Sign in required.");

        json result;
        if (method == "app.version") {
            result = {{"name", "RigMind"}, {"native", true}};

        // Alarm state machine (IDSS EWS port)
        } else if (method == "alarm.step") {
            const AlarmState before = alarm_.current();
            const AlarmState after = alarm_.step(facts_from_json(params));
            if (after != before) store_.record_transition(before, after, now_ms());
            result = {{"state", to_string(after)}, {"changed", after != before}};
        // Local sign-in (see auth_service.h). Passwords arrive here only to be hashed or checked; they are never stored or logged.
        } else if (method == "auth.status") {
            result = auth_.status();
        } else if (method == "auth.setup") {
            result = auth_.setup(require_string(params, "username", 64), require_string(params, "password", 128), now_ms());
            start_session(result);
        } else if (method == "auth.login") {
            session_.reset(); // a new sign-in attempt starts from signed out
            result = auth_.login(require_string(params, "username", 64), require_string(params, "password", 128), now_ms());
            start_session(result);
        } else if (method == "auth.logout") {
            audit_event("auth.logout", session_ ? session_->username : std::string(), "signed out");
            session_.reset();
            result = {{"ok", true}};
        } else if (method == "event.describe") {
            // The plain-English description of an alert is the SLM's only. The prompt is built here from the metadata the host
            // stored for the alert id (facts per state change, cycle path, camera) - never from text the page supplies. The
            // sentence arrives later as a "description" event keyed by alertId; the host stores it on the alert row.
            const std::string alert_id = require_string(params, "alertId", 32);
            const json row = store_.find_raised_alert(alert_id);
            json meta = nullptr;
            std::string severity = clip(params.value("severity", std::string("Advisory")), 16);
            std::string label, cam = clip(params.value("camId", std::string()), 32);
            if (row.is_object()) {
                if (row["metadata"].is_object()) meta = row["metadata"];
                severity = row.value("severity", severity);
                label = row.value("condition", std::string());
                if (meta.is_object() && !meta.value("camera", std::string()).empty()) cam = clip(meta.value("camera", std::string()), 32);
                else if (cam.empty()) cam = clip(row.value("place", std::string()), 32);
            }
            const std::int64_t ts = params.value("tsMs", now_ms());
            const std::string prompt = describe_prompt(meta, severity, label, cam, ts);
            const bool requested = stream_ != nullptr && stream_->can_describe();
            if (requested) {
                // place = the camera id: the sentence guard (slm_guard) restores the camera name when the model leaves it out.
                stream_->send({{"cmd", "describe"}, {"alertId", alert_id}, {"prompt", prompt},
                               {"severity", severity}, {"camId", cam}, {"place", cam},
                               {"sceneTs", std::max<std::int64_t>(0, params.value("sceneTs", static_cast<std::int64_t>(0)))}});
            }
            if (!requested && row.is_object()) store_.put_alert_summary(alert_id, "", "unavailable", now_ms()); // the UI says "BitNet not available"
            audit_event("event.describe", alert_id, requested ? "slm" : "unavailable");
            result = {{"alertId", alert_id}, {"text", nullptr}, {"slm", requested}, {"hasMetadata", meta.is_object()}};
        // Evidence chat sessions: TOON text, LZ4 compressed, stored in the database (see chat_sessions.h)
        } else if (method == "chat.save") {
            if (!params.contains("session") || !params["session"].is_object()) throw std::runtime_error("session is required");
            result = chats_.save(params["session"], now_ms());
        } else if (method == "chat.list") {
            result = chats_.list();
        } else if (method == "chat.load") {
            result = chats_.load(require_string(params, "id", 48));
        } else if (method == "chat.toon") {
            // The compact TOON text of one session: the cheapest form to put back in front of a model.
            const std::string chat_id = require_string(params, "id", 48);
            result = {{"id", chat_id}, {"toon", chats_.toon(chat_id)}};
        } else if (method == "chat.delete") {
            const std::string chat_id = require_string(params, "id", 48);
            chats_.remove(chat_id);
            audit_event("chat.delete", chat_id, "");
            result = {{"ok", true}};
        } else if (method == "evidence.ask") {
            // Evidence chat. The host finds the matching evidence sets and alerts itself (so finding works without any model) and,
            // when BitNet is installed, asks it to explain from that data only; the sentence arrives later as a "chat" event.
            const std::string question = require_string(params, "question", 300);
            const std::int64_t now = now_ms();
            const json footage = store_.list_footage(500);
            std::vector<std::string> cams;
            for (const json& row : footage) {
                const std::string cam = row.value("camId", std::string());
                if (!cam.empty() && std::find(cams.begin(), cams.end(), cam) == cams.end()) cams.push_back(cam);
            }
            const EvidenceQuery query = parse_evidence_question(question, now, local_midnight_ms(now), cams);
            EvidenceQuery every = query;
            every.limit = 100000;
            const json total = select_evidence(footage, every, now);
            const json matches = select_evidence(footage, query, now);
            const std::string note = describe_matches(matches, query, static_cast<int>(total.size()));
            std::string evidence_lines, alert_lines;
            for (const json& m : matches) {
                evidence_lines += std::string(m.value("kind", std::string("video")) == "image" ? "Orange image set" : "Red video set") + " | " +
                                  m.value("camId", std::string()) + " | " + local_stamp(m.value("timestampMs", std::int64_t{0})) + " | " +
                                  std::to_string(m.value("clipCount", 0)) + " files\n";
            }
            int alert_count = 0;
            for (const json& a : store_.list_raised_alerts(200)) {
                const std::int64_t at = a.value("raisedMs", std::int64_t{0});
                if (at < query.since_ms || at >= (query.until_ms ? query.until_ms : now + 1)) continue;
                const std::string id = a.value("id", std::string());
                if (!query.cam.empty() && id.find(query.cam) == std::string::npos && a.value("place", std::string()).find(query.cam) == std::string::npos) continue;
                if (++alert_count > 6) break;
                // "summary" is an explicit null until BitNet has written it; value() would throw on a null.
                const std::string summary = clip(a.contains("summary") && a["summary"].is_string() ? a["summary"].get<std::string>() : std::string(), 200);
                alert_lines += id + " | " + a.value("severity", std::string()) + " | " + a.value("phase", std::string()) + " | " + local_stamp(at) + " | " +
                               (summary.empty() ? std::string("no summary yet") : summary) + "\n";
            }
            const std::string chat_id = "chat-" + std::to_string(now) + "-" + std::to_string(next_chat_++);
            // The model is asked only when there is something for it to explain.
            const bool slm = stream_ != nullptr && stream_->can_describe() && (!matches.empty() || !alert_lines.empty());
            // Earlier turns of the same chat reach the prompt as a compact table (a few tokens each).
            const std::string history = params.contains("history") && params["history"].is_array() ? toon_history(params["history"]) : std::string();
            if (slm) stream_->send({{"cmd", "chat"}, {"chatId", chat_id}, {"prompt", evidence_chat_prompt(question, note, evidence_lines, alert_lines, 2200, history)}});
            audit_event("evidence.ask", chat_id, clip(question, 160) + " | " + std::to_string(matches.size()) + " matches" + (slm ? " | asked BitNet" : ""));
            result = {{"chatId", chat_id}, {"matches", matches}, {"note", note}, {"slm", slm}};
        } else if (method == "footage.list") {
            result = store_.list_footage(std::clamp(params.value("limit", 50), 1, 500));
        } else if (method == "footage.files") {
            result = store_.list_footage_files(require_string(params, "path", 64));
        } else if (method == "footage.record") {
            // Sent by the host when the pipeline finishes a clip or an image set: {cam, stamp, count, kind: video|image}.
            const std::string cam = require_string(params, "cam", 32);
            const std::string stamp = require_string(params, "stamp", 20);
            const int count = params.at("count").get<int>();
            if (count < 0 || count > 1000) throw std::runtime_error("invalid count");
            const std::string kind = params.value("kind", std::string("video"));
            store_.record_footage(cam, std::stoll(stamp), count, cam + "/" + stamp, kind);
            audit_event("footage.save", cam, std::to_string(count) + (kind == "image" ? " images" : " clips"));
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
        } else if (method == "audit.recent") {
            // Real audit rows for one action (default alarm.transition), newest first.
            result = store_.recent_audit(clip(params.value("action", std::string("alarm.transition")), 64),
                                         std::clamp(params.value("limit", 200), 1, 1000));
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
        } else if (method == "alert.get") {
            // One alert row with its metadata (facts) and summary; null when the id is unknown.
            result = store_.find_raised_alert(require_string(params, "id", 32));
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
            // Optional camera login. Both blank = a camera without credentials (the "ignore" choice on the Camera page).
            const std::string user = params.value("user", std::string());
            const std::string pass = params.value("pass", std::string());
            if (user.size() > 128 || pass.size() > 128) throw std::runtime_error("invalid credentials");
            stream_->send({{"cmd", "open"}, {"cam", cam}, {"uri", uri}, {"user", user}, {"pass", pass}});
            audit_event("stream.open", cam, "");
            result = {{"accepted", true}};
        } else if (method == "stream.offer") {
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            // profile: "small" (640x360, low bitrate: dashboard tiles) or "full" (1280x720, the Live view); anything else is "full"
            const std::string profile = params.value("profile", std::string("full")) == "small" ? "small" : "full";
            stream_->send({{"cmd", "offer"}, {"cam", require_string(params, "cam", 32)},
                           {"sdp", require_string(params, "sdp", 16384)}, {"profile", profile}});
            result = {{"accepted", true}};
        } else if (method == "stream.detach") {
            // The viewer left the Live page. The camera keeps recording and raising alarms.
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            stream_->send({{"cmd", "detach"}, {"cam", require_string(params, "cam", 32)}});
            result = {{"accepted", true}};
        } else if (method == "stream.overlay") {
            // Which models are burned into the live video: {cam, ppe, idss}. Both false = the camera picture as it comes in
            // (Connect a camera preview). The pipeline applies it to the viewer's video at once.
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            if (!params.contains("ppe") || !params["ppe"].is_boolean() || !params.contains("idss") || !params["idss"].is_boolean())
                throw std::runtime_error("ppe and idss must be true or false");
            stream_->send({{"cmd", "overlay"}, {"cam", require_string(params, "cam", 32)},
                           {"ppe", params["ppe"].get<bool>()}, {"idss", params["idss"].get<bool>()}});
            result = {{"accepted", true}};
        } else if (method == "stream.status") {
            // Asks the pipeline to re-send the status of every camera (answered as "status" events).
            if (!stream_) throw std::runtime_error("stream pipeline not available");
            stream_->send({{"cmd", "status"}});
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
            const std::string record_key = require_string(params, "key", 256);
            store_.put_record(record_key, params.at("value").dump());
            if (record_hook_) record_hook_(record_key);
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
