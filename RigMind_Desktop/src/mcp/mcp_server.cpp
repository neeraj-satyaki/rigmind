#include "mcp/mcp_server.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "slm/scene_text.h"

namespace rigmind {

namespace {

using json = nlohmann::json;

const char* kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

const char* kInstructions =
    "RigMind rig-safety monitoring. Alerts follow event cycles: Yellow>Orange>Yellow is a normal cycle, Yellow>Orange>Red>Orange>Yellow "
    "or Yellow>Orange>Red>Yellow is a critical cycle. Orange alerts save image evidence, Red alerts save a video clip. "
    "get_live_scene returns what the models see right now (class names, mask contours, centroid paths as pixel coordinates of the "
    "frame size given). Everything is read-only except request_summary, which asks the on-device BitNet model for one alert.";

// Cuts at a character boundary (never inside a multi-byte sequence) so the result stays valid UTF-8.
std::string clip_utf8(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s;
}

// Serialises without throwing on invalid UTF-8 (database text may hold anything): bad bytes become U+FFFD.
std::string safe_dump(const json& v, int indent = -1) { return v.dump(indent, ' ', false, json::error_handler_t::replace); }

// MCP tool result: one text block holding JSON.
json text_result(const json& value, bool is_error = false) {
    return {{"content", json::array({{{"type", "text"}, {"text", value.is_string() ? value.get<std::string>() : safe_dump(value, 2)}}})},
            {"isError", is_error}};
}

json rpc_error(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json rpc_result(const json& id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

std::string base64(const std::string& bytes) {
    static const char* kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out += kDigits[a >> 2];
        out += kDigits[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? kDigits[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? kDigits[c & 63] : '=';
    }
    return out;
}

int clamp_int(const json& args, const char* key, int fallback, int lo, int hi) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_number_integer()) return fallback;
    return std::clamp(args[key].get<int>(), lo, hi);
}

std::string str_arg(const json& args, const char* key, std::size_t max_len = 256) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_string()) return {};
    return clip_utf8(args[key].get<std::string>(), max_len);
}

std::string lower_ext(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// Evenly spaced subset, first and last always kept.
json thin(const json& list, std::size_t limit) {
    if (!list.is_array() || list.size() <= limit) return list;
    json out = json::array();
    if (limit == 0) return out;
    if (limit == 1) return json::array({list.back()});
    for (std::size_t k = 0; k < limit; ++k) out.push_back(list[k * (list.size() - 1) / (limit - 1)]);
    return out;
}

json schema(json properties, std::vector<std::string> required = {}) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required}};
}

json tool(const char* name, const char* description, json input) {
    return {{"name", name}, {"description", description}, {"inputSchema", std::move(input)}};
}

const char* kAuditActions[] = {"alarm.transition", "alarm.cycle", "alert.respond", "ppe.transition", "event.describe", "summary.ready",
                               "camera.status", "evidence.started", "evidence.saved", "evidence.snapshot", "footage.save",
                               "notification.tray", "stream.open", "stream.close", "app.start", "app.stop", "app.error", "slm.start",
                               "pipeline.start", "report.pdf", "mcp.start", "mcp.call", "mcp.set"};

// The model's sentence of an alert row as {alertId, text, source, atMs}, or null while none was written.
json summary_of(const json& alert) {
    if (!alert.is_object() || !alert.contains("summary") || !alert["summary"].is_string()) return nullptr;
    return {{"alertId", alert.value("id", std::string())}, {"text", alert["summary"]}, {"source", alert.value("summarySource", std::string("slm"))},
            {"atMs", alert.value("summaryAt", std::int64_t{0})}};
}

} // namespace

McpServer::McpServer(const std::string& db_path, McpHooks hooks) : hooks_(std::move(hooks)), store_(std::make_unique<EventStore>(db_path)) {
    if (!hooks_.footage_root.empty()) store_->set_footage_root(hooks_.footage_root.string());
    if (!hooks_.now_ms) {
        hooks_.now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        };
    }
}

std::int64_t McpServer::now() const { return hooks_.now_ms(); }

json McpServer::handle(const json& request) {
    if (request.is_array()) {
        // A batch is one level deep and at most 32 messages: no recursion on attacker-shaped input.
        if (request.empty() || request.size() > 32) return rpc_error(nullptr, -32600, "batch must hold 1 to 32 messages");
        json out = json::array();
        for (const json& one : request) {
            if (one.is_array()) {
                out.push_back(rpc_error(nullptr, -32600, "nested batches are not allowed"));
                continue;
            }
            const json r = handle_one(one);
            if (!r.is_null()) out.push_back(r);
        }
        return out.empty() ? json(nullptr) : out;
    }
    return handle_one(request);
}

json McpServer::handle_one(const json& request) {
    try {
        return dispatch(request);
    } catch (const std::exception& e) { // any malformed shape ends here as a JSON-RPC error, never as a crash
        const json id = request.is_object() && request.contains("id") && (request["id"].is_string() || request["id"].is_number()) ? request["id"] : json(nullptr);
        return rpc_error(id, -32603, std::string("internal error: ") + e.what());
    }
}

json McpServer::dispatch(const json& request) {
    const json no_id = nullptr;
    if (!request.is_object() || !request.contains("jsonrpc") || !request["jsonrpc"].is_string() || request["jsonrpc"].get<std::string>() != "2.0" ||
        !request.contains("method") || !request["method"].is_string())
        return rpc_error(no_id, -32600, "invalid request");
    const std::string method = request["method"].get<std::string>();
    const bool notification = !request.contains("id");
    if (!notification && !(request["id"].is_string() || request["id"].is_number() || request["id"].is_null())) return rpc_error(no_id, -32600, "invalid id");
    const json id = notification ? json(nullptr) : request["id"];
    const json params = request.contains("params") && request["params"].is_object() ? request["params"] : json::object();
    if (notification) return nullptr; // notifications/initialized, notifications/cancelled: nothing to answer

    try {
        if (method == "initialize") {
            std::string version = kProtocolVersions[0];
            const std::string asked = params.contains("protocolVersion") && params["protocolVersion"].is_string() ? params["protocolVersion"].get<std::string>() : std::string();
            for (const char* v : kProtocolVersions)
                if (asked == v) version = v;
            return rpc_result(id, {{"protocolVersion", version},
                                   {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"subscribe", false}, {"listChanged", false}}}}},
                                   {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
                                   {"instructions", kInstructions}});
        }
        if (method == "ping") return rpc_result(id, json::object());
        if (method == "tools/list") return rpc_result(id, {{"tools", tools_list()}});
        if (method == "tools/call") {
            if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) return rpc_error(id, -32602, "tools/call needs a name");
            const json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
            try {
                return rpc_result(id, call_tool(params["name"].get<std::string>(), args));
            } catch (const std::exception& e) {
                return rpc_result(id, text_result(std::string("error: ") + e.what(), true)); // tool failures are results, not protocol errors
            }
        }
        if (method == "resources/list") return rpc_result(id, {{"resources", resources_list()}});
        if (method == "resources/templates/list")
            return rpc_result(id, {{"resourceTemplates", json::array({{{"uriTemplate", "rigmind://scene/{cam}"}, {"name", "Live scene of a camera"}, {"mimeType", "application/json"}},
                                                                      {{"uriTemplate", "rigmind://alert/{id}"}, {"name", "One alert with its history"}, {"mimeType", "application/json"}}})}});
        if (method == "resources/read") {
            if (!params.is_object() || !params.contains("uri") || !params["uri"].is_string()) return rpc_error(id, -32602, "resources/read needs a uri");
            const std::string uri = clip_utf8(params["uri"].get<std::string>(), 200);
            if (hooks_.audit) hooks_.audit("resources/read", uri); // resources are data access like tools
            const json body = read_resource(uri);
            return rpc_result(id, {{"contents", json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", safe_dump(body, 2)}}})}});
        }
        return rpc_error(id, -32601, "method not found: " + method);
    } catch (const std::invalid_argument& e) {
        return rpc_error(id, -32602, e.what());
    } catch (const std::exception& e) {
        return rpc_error(id, -32603, e.what());
    }
}

json McpServer::tools_list() const {
    const json limit = {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}};
    return json::array({
        tool("list_alerts", "Raised alerts, newest first, each with severity, a neutral condition label (e.g. Orange alert), place (the camera id), status (Open/Monitoring/Escalated/Cleared/Acknowledged), owner, times, metadata (facts only: camera, peak level, cycle path, and for every state change the EWS facts true at that moment; for PPE the items not detected and the people count) and summary / summarySource / summaryAt (the on-device model's plain-English sentence, null until written).",
             schema({{"limit", limit}, {"status", {{"type", "string"}, {"description", "only alerts with this status or phase"}}}})),
        tool("get_alert", "One alert with its metadata (facts), event-cycle history (audit rows), the plain-language summary (written by the on-device model from the metadata) and its evidence.",
             schema({{"alertId", {{"type", "string"}}}}, {"alertId"})),
        tool("get_live_scene",
             "What the detection models see now: for every object its model, class name, tracker id, box, centroid, mask contour points and a centroid path (x, y, ms ago), in pixels of the frame size returned. Also a compact text form for prompts.",
             schema({{"camId", {{"type", "string"}}}, {"contour_points", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}}, {"trail_points", {{"type", "integer"}, {"minimum", 0}, {"maximum", 16}}}})),
        tool("list_evidence", "Saved evidence sets: Red alerts save video clips, Orange alerts save images. Each has camera, time, kind, file count and a path for get_evidence.",
             schema({{"limit", limit}, {"kind", {{"type", "string"}, {"enum", json::array({"video", "image"})}}}})),
        tool("get_evidence", "Files of one evidence set; with include_images, up to 3 JPEG/PNG images (under 1.5 MB each) come back as image content.",
             schema({{"path", {{"type", "string"}, {"description", "the path value from list_evidence, e.g. CAM-01/1793350666337"}}}, {"include_images", {{"type", "boolean"}}}, {"max_images", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}}}, {"path"})),
        tool("list_summaries", "Plain-language summaries the on-device model wrote for alerts, newest first.", schema({{"limit", limit}})),
        tool("report_summary", "Report-style totals for the last N days: alerts by severity and status, normal and critical event cycles, PPE advisories, evidence sets by kind, summaries by source.",
             schema({{"days", {{"type", "integer"}, {"minimum", 1}, {"maximum", 90}}}})),
        tool("list_reports", "Report files saved on this PC (PDF and CSV in Documents\\RigMind Reports).", schema(json::object())),
        tool("get_audit", "Audit trail rows (app activity, alarms, responses, summaries), newest first. Without an action the main actions are merged.",
             schema({{"action", {{"type", "string"}}}, {"limit", limit}})),
        tool("request_summary", "Ask the on-device BitNet model to describe one alert now (uses the live scene when the alert is live). Waits up to 120 seconds for the sentence.",
             schema({{"alertId", {{"type", "string"}}}}, {"alertId"})),
    });
}

json McpServer::call_tool(const std::string& name, const json& args) {
    if (hooks_.audit) hooks_.audit(name, args.is_object() ? clip_utf8(safe_dump(args), 300) : std::string());
    if (name == "get_live_scene") {
        const json scene = scene_for(args);
        return text_result(scene);
    }
    if (name == "request_summary") {
        const std::string id = str_arg(args, "alertId", 32);
        if (id.empty()) throw std::invalid_argument("alertId is required");
        json alert;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            alert = store_->find_raised_alert(id);
        }
        if (alert.is_null()) throw std::runtime_error("no alert " + id);
        if (!hooks_.request_summary) throw std::runtime_error("summaries are not available");
        std::string cam;
        if (alert["metadata"].is_object()) cam = alert["metadata"].value("camera", std::string());
        if (cam.empty() && hooks_.scene_cameras)
            for (const std::string& c : hooks_.scene_cameras())
                if (id.rfind("ALM-" + c + "-", 0) == 0 || id.rfind("PPE-" + c + "-", 0) == 0) cam = c;
        const bool live = alert.value("phase", std::string()) != "Cleared";
        // The host builds the model prompt from the metadata stored for the alert id; only the scene time is passed on.
        json req = {{"alertId", id}, {"severity", alert.value("severity", std::string())}, {"camId", cam}, {"sceneTs", live && !cam.empty() ? now() : 0}};
        const json answer = hooks_.request_summary(req, 120000);
        if (answer.is_null()) return text_result({{"alertId", id}, {"status", "pending"}, {"note", "no answer within 120 seconds; try list_summaries later"}});
        return text_result({{"alertId", id}, {"status", "done"}, {"text", answer.value("text", std::string())}, {"source", answer.value("source", std::string("unavailable"))}});
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (name == "list_alerts") {
        const int limit = clamp_int(args, "limit", 50, 1, 500);
        const std::string status = str_arg(args, "status", 16);
        json out = json::array();
        for (const json& row : store_->list_raised_alerts(500)) {
            if (!status.empty() && row.value("status", std::string()) != status && row.value("phase", std::string()) != status) continue;
            out.push_back(row);
            if (static_cast<int>(out.size()) >= limit) break;
        }
        return text_result({{"count", out.size()}, {"alerts", out}});
    }
    if (name == "get_alert") {
        const std::string id = str_arg(args, "alertId", 32);
        if (id.empty()) throw std::invalid_argument("alertId is required");
        return text_result(alert_detail(id));
    }
    if (name == "list_evidence") {
        const int limit = clamp_int(args, "limit", 50, 1, 500);
        const std::string kind = str_arg(args, "kind", 8);
        json out = json::array();
        for (const json& row : store_->list_footage(500)) {
            if (!kind.empty() && row.value("kind", std::string("video")) != kind) continue;
            out.push_back(row);
            if (static_cast<int>(out.size()) >= limit) break;
        }
        return text_result({{"count", out.size()}, {"evidence", out}, {"note", "Red = video clips, Orange = image sets; pass path to get_evidence"}});
    }
    if (name == "get_evidence") {
        json images = json::array();
        const json detail = evidence_detail(args, images);
        json content = json::array({{{"type", "text"}, {"text", safe_dump(detail, 2)}}});
        for (const json& img : images) content.push_back(img);
        return {{"content", content}, {"isError", false}};
    }
    if (name == "list_summaries") return text_result(summaries(clamp_int(args, "limit", 50, 1, 500)));
    if (name == "report_summary") return text_result(report_summary(clamp_int(args, "days", 7, 1, 90)));
    if (name == "list_reports") return text_result(list_report_files());
    if (name == "get_audit") {
        const int limit = clamp_int(args, "limit", 100, 1, 1000);
        const std::string action = str_arg(args, "action", 64);
        json rows = json::array();
        if (!action.empty()) {
            rows = store_->recent_audit(action, limit);
        } else {
            for (const char* a : kAuditActions)
                for (const json& r : store_->recent_audit(a, limit)) rows.push_back(r);
            std::sort(rows.begin(), rows.end(), [](const json& x, const json& y) { return x.value("tsMs", std::int64_t{0}) > y.value("tsMs", std::int64_t{0}); });
            if (static_cast<int>(rows.size()) > limit) rows.erase(rows.begin() + limit, rows.end());
        }
        return text_result({{"count", rows.size()}, {"rows", rows}});
    }
    throw std::invalid_argument("unknown tool: " + name);
}

json McpServer::scene_for(const json& args) {
    if (!hooks_.scene) throw std::runtime_error("live scene is not available");
    std::string cam = str_arg(args, "camId", 32);
    json scene = hooks_.scene(cam);
    if (!scene.is_object()) return {{"available", false}, {"note", "no scene yet: the camera is offline or the models have not seen anything"}};
    const std::int64_t age = now() - scene.value("ts", std::int64_t{0});
    const std::size_t contour_n = static_cast<std::size_t>(clamp_int(args, "contour_points", 16, 0, 64));
    const std::size_t trail_n = static_cast<std::size_t>(clamp_int(args, "trail_points", 8, 0, 16));
    json objects = json::array();
    for (json o : scene.value("objects", json::array())) {
        o["contour"] = thin(o.value("contour", json::array()), contour_n);
        o["trail"] = thin(o.value("trail", json::array()), trail_n);
        objects.push_back(std::move(o));
    }
    scene["objects"] = objects;
    return {{"available", true}, {"camId", scene.value("cam", cam)}, {"ageMs", age}, {"stale", age > 5000},
            {"frame", {{"width", scene.value("w", 0)}, {"height", scene.value("h", 0)}}}, {"objects", objects},
            {"text", scene_prompt_text(scene)}};
}

json McpServer::alert_detail(const std::string& id) {
    const json alert = store_->find_raised_alert(id);
    if (alert.is_null()) throw std::runtime_error("no alert " + id);
    json history = json::array();
    for (const char* action : {"alarm.transition", "alarm.cycle", "alert.respond", "ppe.transition", "event.describe", "summary.ready", "notification.tray"})
        for (const json& r : store_->recent_audit(action, 1000))
            if (r.value("target", std::string()) == id) history.push_back(r);
    std::sort(history.begin(), history.end(), [](const json& x, const json& y) { return x.value("tsMs", std::int64_t{0}) < y.value("tsMs", std::int64_t{0}); });
    json summary = summary_of(alert);
    // Evidence in the alert's time window (the footage table is by camera and time).
    json evidence = json::array();
    const std::int64_t from = alert.value("raisedMs", std::int64_t{0}) - 60000;
    const std::int64_t to = alert.value("updatedMs", std::int64_t{0}) + 60000;
    for (const json& f : store_->list_footage(500)) {
        const std::int64_t t = f.value("timestampMs", std::int64_t{0});
        if (t >= from && t <= to) evidence.push_back(f);
    }
    return {{"alert", alert}, {"history", history}, {"summary", summary}, {"evidence", evidence}};
}

std::filesystem::path McpServer::safe_evidence_dir(const std::string& rel) const {
    if (rel.empty() || rel.size() > 96) throw std::invalid_argument("path is required");
    for (char c : rel)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '/' || c == '.')) throw std::invalid_argument("invalid path");
    if (rel.find("..") != std::string::npos || rel.front() == '/' || rel.front() == '.') throw std::invalid_argument("invalid path");
    const std::filesystem::path root = std::filesystem::weakly_canonical(hooks_.footage_root);
    const std::filesystem::path dir = std::filesystem::weakly_canonical(root / rel);
    const std::string r = root.string(), d = dir.string();
    if (d.size() <= r.size() || d.compare(0, r.size(), r) != 0) throw std::invalid_argument("invalid path");
    return dir;
}

json McpServer::evidence_detail(const json& args, json& images) {
    const std::string rel = str_arg(args, "path", 96);
    const std::filesystem::path dir = safe_evidence_dir(rel);
    json folder = store_->list_footage_files(rel);
    // {"dir","files":[videos],"images":[jpgs],"kind"} (older hosts: a plain list of names)
    json files = json::array();
    if (folder.is_object()) {
        for (const char* key : {"files", "images"})
            if (folder.contains(key) && folder[key].is_array())
                for (const json& n : folder[key]) files.push_back(n);
    } else if (folder.is_array()) {
        files = folder;
    }
    json row = nullptr;
    for (const json& f : store_->list_footage(500))
        if (f.value("path", std::string()) == rel) row = f;
    if (row.is_null()) throw std::runtime_error("no evidence at " + rel);
    const bool include = args.is_object() && args.value("include_images", false);
    const int max_images = clamp_int(args, "max_images", 3, 1, 3);
    json listed = json::array();
    for (const json& f : files) {
        const std::string name = f.is_string() ? f.get<std::string>() : f.value("name", std::string());
        if (name.empty() || name.find_first_of("/\\") != std::string::npos) continue;
        const std::filesystem::path file = dir / name;
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        json entry = {{"name", name}, {"bytes", ec ? 0 : size}, {"url", "https://footage.rigmind.app/" + rel + "/" + name}, {"file", file.string()}};
        listed.push_back(entry);
        const std::string ext = lower_ext(file);
        if (include && static_cast<int>(images.size()) < max_images && (ext == ".jpg" || ext == ".jpeg" || ext == ".png") && !ec && size > 0 && size <= 1500000) {
            std::ifstream in(file, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            images.push_back({{"type", "image"}, {"data", base64(bytes)}, {"mimeType", ext == ".png" ? "image/png" : "image/jpeg"}});
        }
    }
    return {{"evidence", row}, {"files", listed}, {"note", "Red evidence is video (mp4 segments), Orange evidence is images"}};
}

json McpServer::summaries(int limit) {
    json rows = json::array();
    for (const json& alert : store_->list_raised_alerts(500)) {
        const json s = summary_of(alert);
        if (s.is_object()) rows.push_back(s);
    }
    std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a.value("atMs", std::int64_t{0}) > b.value("atMs", std::int64_t{0}); });
    if (static_cast<int>(rows.size()) > limit) rows.erase(rows.begin() + limit, rows.end());
    return {{"count", rows.size()}, {"summaries", rows}};
}

json McpServer::report_summary(int days) {
    const std::int64_t since = now() - static_cast<std::int64_t>(days) * 86400000;
    std::map<std::string, int> by_severity, by_status;
    int alerts = 0;
    for (const json& a : store_->list_raised_alerts(500)) {
        if (a.value("raisedMs", std::int64_t{0}) < since) continue;
        ++alerts;
        ++by_severity[a.value("severity", std::string())];
        ++by_status[a.value("status", std::string())];
    }
    int normal = 0, critical = 0, ppe_raised = 0;
    for (const json& r : store_->recent_audit("alarm.cycle", 1000)) {
        if (r.value("tsMs", std::int64_t{0}) < since) continue;
        (r.value("detail", std::string()).rfind("critical", 0) == 0 ? critical : normal)++;
    }
    for (const json& r : store_->recent_audit("ppe.transition", 1000))
        if (r.value("tsMs", std::int64_t{0}) >= since && r.value("detail", std::string()).rfind("raised", 0) == 0) ++ppe_raised;
    int video = 0, image = 0;
    for (const json& f : store_->list_footage(500)) {
        if (f.value("timestampMs", std::int64_t{0}) < since) continue;
        (f.value("kind", std::string("video")) == "image" ? image : video)++;
    }
    int slm = 0;
    for (const json& a : store_->list_raised_alerts(500))
        if (a.value("raisedMs", std::int64_t{0}) >= since && summary_of(a).is_object()) ++slm;
    return {{"days", days}, {"alerts", {{"total", alerts}, {"bySeverity", by_severity}, {"byStatus", by_status}}},
            {"eventCycles", {{"normal", normal}, {"critical", critical}}}, {"ppeAdvisories", ppe_raised},
            {"evidence", {{"videoSets", video}, {"imageSets", image}}}, {"summaries", {{"bitnet", slm}}}};
}

json McpServer::list_report_files() const {
    json out = json::array();
    std::error_code ec;
    if (hooks_.reports_dir.empty() || !std::filesystem::is_directory(hooks_.reports_dir, ec)) return {{"count", 0}, {"reports", out}, {"dir", hooks_.reports_dir.string()}};
    for (const auto& entry : std::filesystem::directory_iterator(hooks_.reports_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = lower_ext(entry.path());
        if (ext != ".pdf" && ext != ".csv") continue;
        const auto modified = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::time_point_cast<std::chrono::system_clock::duration>(entry.last_write_time() - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()).time_since_epoch())
                                  .count();
        out.push_back({{"name", entry.path().filename().string()}, {"bytes", entry.file_size()}, {"modifiedMs", modified}, {"file", entry.path().string()}});
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a.value("modifiedMs", std::int64_t{0}) > b.value("modifiedMs", std::int64_t{0}); });
    return {{"count", out.size()}, {"reports", out}, {"dir", hooks_.reports_dir.string()}};
}

json McpServer::resources_list() {
    json out = json::array({{{"uri", "rigmind://alerts"}, {"name", "Alerts"}, {"mimeType", "application/json"}, {"description", "Raised alerts, newest first"}},
                            {{"uri", "rigmind://summaries"}, {"name", "Plain-language summaries"}, {"mimeType", "application/json"}},
                            {{"uri", "rigmind://evidence"}, {"name", "Evidence sets (Orange images, Red video)"}, {"mimeType", "application/json"}}});
    if (hooks_.scene_cameras)
        for (const std::string& cam : hooks_.scene_cameras())
            out.push_back({{"uri", "rigmind://scene/" + cam}, {"name", "Live scene " + cam}, {"mimeType", "application/json"}});
    return out;
}

json McpServer::read_resource(const std::string& uri) {
    const std::string prefix = "rigmind://";
    if (uri.rfind(prefix, 0) != 0) throw std::invalid_argument("unknown resource " + uri);
    const std::string rest = uri.substr(prefix.size());
    if (rest.rfind("scene/", 0) == 0) return scene_for({{"camId", rest.substr(6)}});
    std::lock_guard<std::mutex> lock(mutex_);
    if (rest == "alerts") return store_->list_raised_alerts(100);
    if (rest == "summaries") return summaries(100);
    if (rest == "evidence") return store_->list_footage(100);
    if (rest.rfind("alert/", 0) == 0) return alert_detail(rest.substr(6));
    throw std::invalid_argument("unknown resource " + uri);
}

} // namespace rigmind
