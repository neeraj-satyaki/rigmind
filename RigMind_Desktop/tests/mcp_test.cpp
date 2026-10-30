// RigMind MCP module: protocol, tools, resources and the local HTTP transport (token, Origin/Host checks).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "mcp/mcp_http.h"
#include "mcp/mcp_server.h"

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

using json = nlohmann::json;

json rpc(rigmind::McpServer& s, int id, const std::string& method, json params = json::object()) {
    return s.handle({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
}
json tool(rigmind::McpServer& s, const std::string& name, json args = json::object()) {
    return rpc(s, 1, "tools/call", {{"name", name}, {"arguments", args}});
}
// The JSON the tool put in its first text block.
json payload(const json& response) {
    return json::parse(response.at("result").at("content").at(0).at("text").get<std::string>());
}

// One HTTP exchange with the server: returns "<status>\n<body>".
std::string http(unsigned short port, const std::string& method, const std::string& path, const std::string& headers, const std::string& body) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return "connect failed";
    std::string req = method + " " + path + " HTTP/1.1\r\n" + headers + "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string reply;
    char buf[4096];
    for (int n; (n = recv(s, buf, sizeof(buf), 0)) > 0;) reply.append(buf, static_cast<std::size_t>(n));
    closesocket(s);
    const auto sp = reply.find(' ');
    const std::string status = reply.substr(sp + 1, 3);
    const auto sep = reply.find("\r\n\r\n");
    return status + "\n" + (sep == std::string::npos ? "" : reply.substr(sep + 4));
}

} // namespace

static int run() {
    const auto dir = std::filesystem::temp_directory_path() / ("rigmind-mcp-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code cleanup;
    std::filesystem::remove_all(dir, cleanup);
    std::filesystem::create_directories(dir / "footage" / "CAM-01" / "1000000");
    std::filesystem::create_directories(dir / "footage" / "CAM-01" / "2000000");
    std::filesystem::create_directories(dir / "reports");
    {
        std::ofstream(dir / "footage" / "CAM-01" / "2000000" / "orange-1.jpg", std::ios::binary) << "\xFF\xD8\xFF\xE0 fake jpeg bytes \xFF\xD9";
        std::ofstream(dir / "footage" / "CAM-01" / "1000000" / "seg0.mp4") << "not a real mp4";
        std::ofstream(dir / "reports" / "weekly.pdf") << "%PDF-1.4";
        std::ofstream(dir / "reports" / "notes.txt") << "ignored";
    }
    const std::string db = (dir / "rigmind.db").string();
    const std::int64_t now = 10'000'000;
    {
        rigmind::EventStore store(db);
        store.set_footage_root((dir / "footage").string());
        store.upsert_alert("ALM-CAM-01-a1", "Warning", "Elevator near the platform", "Camera CAM-01", "Cleared", now - 60000);
        store.upsert_alert("ALM-CAM-01-a2", "Critical", "Elevator in the platform danger zone", "Camera CAM-01", "Escalated", now - 1000);
        store.upsert_alert("PPE-CAM-01-a1", "Advisory", "PPE not detected: vest", "Camera CAM-01", "Open", now - 2000);
        store.record_footage("CAM-01", 1000000, 1, "CAM-01/1000000", "video");
        store.record_footage("CAM-01", 2000000, 1, "CAM-01/2000000", "image");
        store.put_record("plain:ALM-CAM-01-a1", R"({"alertId":"ALM-CAM-01-a1","text":"Camera CAM-01 saw it. Check the area.","source":"slm","at":"2026-10-30T14:00:00Z"})");
    }

    json live_scene = {{"ev", "scene"}, {"cam", "CAM-01"}, {"ts", now - 500}, {"w", 1280}, {"h", 720},
                       {"objects", json::array({{{"model", "idss"}, {"label", "Elevators"}, {"id", 1}, {"box", {600, 280, 640, 320}}, {"centroid", {620, 300}},
                                                 {"contour", {{600, 280}, {640, 280}, {640, 320}, {600, 320}, {610, 300}, {630, 300}}},
                                                 {"trail", {{600, 300, 2000}, {610, 300, 1000}, {620, 300, 0}}}}})}};
    json asked;
    rigmind::McpHooks hooks;
    hooks.footage_root = dir / "footage";
    hooks.reports_dir = dir / "reports";
    hooks.now_ms = [now] { return now; };
    hooks.scene = [&](const std::string& cam) { return cam.empty() || cam == "CAM-01" ? live_scene : json(nullptr); };
    hooks.scene_cameras = [] { return std::vector<std::string>{"CAM-01"}; };
    hooks.request_summary = [&](const json& alert, int) {
        asked = alert;
        return json{{"text", "Camera CAM-01, Camera CAM-01: stop the work."}, {"source", "slm"}};
    };
    rigmind::McpServer server(db, hooks);

    // protocol
    json init = rpc(server, 1, "initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "t"}, {"version", "1"}}}});
    check(init["result"]["protocolVersion"] == "2025-03-26" && init["result"]["serverInfo"]["name"] == "rigmind", "initialize echoes a supported version");
    check(init["result"]["capabilities"].contains("tools") && init["result"]["capabilities"].contains("resources"), "capabilities");
    check(server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null(), "notification has no response");
    check(rpc(server, 2, "ping")["result"].is_object(), "ping");
    check(rpc(server, 3, "nope")["error"]["code"] == -32601, "unknown method");
    check(server.handle({{"method", "x"}})["error"]["code"] == -32600, "missing jsonrpc is an invalid request");
    const json tools = rpc(server, 4, "tools/list")["result"]["tools"];
    check(tools.size() == 10, "ten tools");
    for (const json& t : tools) check(t.contains("inputSchema") && t["inputSchema"]["type"] == "object", "every tool has a schema");
    json batch = server.handle(json::array({{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}}, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}}));
    check(batch.is_array() && batch.size() == 1, "batch drops notifications");

    // alerts
    json alerts = payload(tool(server, "list_alerts"));
    check(alerts["count"] == 3 && alerts["alerts"][0]["id"] == "ALM-CAM-01-a2", "alerts newest first");
    check(payload(tool(server, "list_alerts", {{"status", "Open"}}))["count"] == 1, "status filter");
    json detail = payload(tool(server, "get_alert", {{"alertId", "ALM-CAM-01-a1"}}));
    check(detail["alert"]["severity"] == "Warning" && detail["summary"]["source"] == "slm", "alert detail with its summary");
    check(tool(server, "get_alert", {{"alertId", "nope"}})["result"]["isError"] == true, "unknown alert is a tool error");

    // live scene
    json scene = payload(tool(server, "get_live_scene", {{"camId", "CAM-01"}, {"contour_points", 4}, {"trail_points", 2}}));
    check(scene["available"] == true && scene["frame"]["width"] == 1280 && scene["stale"] == false, "scene is live");
    check(scene["objects"][0]["label"] == "Elevators" && scene["objects"][0]["contour"].size() == 4 && scene["objects"][0]["trail"].size() == 2, "class name, thinned contour and path");
    check(scene["text"].get<std::string>().find("Elevators #1") != std::string::npos, "prompt text form");
    check(payload(tool(server, "get_live_scene", {{"camId", "CAM-09"}}))["available"] == false, "no scene for another camera");

    // evidence
    json evidence = payload(tool(server, "list_evidence"));
    check(evidence["count"] == 2, "two evidence sets");
    check(payload(tool(server, "list_evidence", {{"kind", "image"}}))["count"] == 1, "image filter");
    json got = tool(server, "get_evidence", {{"path", "CAM-01/2000000"}, {"include_images", true}});
    check(got["result"]["content"].size() == 2 && got["result"]["content"][1]["type"] == "image" && got["result"]["content"][1]["mimeType"] == "image/jpeg", "orange images come back as image content");
    check(got["result"]["content"][1]["data"].get<std::string>().size() > 8, "image data present");
    json video = tool(server, "get_evidence", {{"path", "CAM-01/1000000"}, {"include_images", true}});
    check(video["result"]["content"].size() == 1, "video evidence returns metadata only");
    for (const char* bad : {"../x", "CAM-01/..", "/etc", "CAM-01/1000000/../../x", "a b", ""})
        check(tool(server, "get_evidence", {{"path", bad}})["result"]["isError"] == true, "path traversal is refused");

    // summaries, reports, audit
    check(payload(tool(server, "list_summaries"))["count"] == 1, "summaries");
    json report = payload(tool(server, "report_summary", {{"days", 7}}));
    check(report["alerts"]["total"] == 3 && report["alerts"]["bySeverity"]["Critical"] == 1, "report totals");
    check(report["evidence"]["videoSets"] == 1 && report["evidence"]["imageSets"] == 1 && report["summaries"]["bitnet"] == 1, "report evidence and summaries");
    json reports = payload(tool(server, "list_reports"));
    check(reports["count"] == 1 && reports["reports"][0]["name"] == "weekly.pdf", "only pdf and csv reports");
    check(payload(tool(server, "get_audit"))["rows"].is_array(), "audit rows");

    // request_summary: wired to the summary model, scene only for a live alert
    json summary = payload(tool(server, "request_summary", {{"alertId", "ALM-CAM-01-a2"}}));
    check(summary["status"] == "done" && summary["source"] == "slm", "summary answered");
    check(asked["camId"] == "CAM-01" && asked["sceneTs"] == now && asked["severity"] == "Critical", "live alert: own camera and scene time");
    tool(server, "request_summary", {{"alertId", "ALM-CAM-01-a1"}});
    check(asked["sceneTs"] == 0 || asked["sceneTs"] == now, "summary request carries a scene time field");

    // resources
    check(rpc(server, 8, "resources/list")["result"]["resources"].size() == 4, "3 fixed resources + 1 scene");
    json res = rpc(server, 9, "resources/read", {{"uri", "rigmind://scene/CAM-01"}});
    check(res["result"]["contents"][0]["mimeType"] == "application/json", "scene resource");
    check(rpc(server, 10, "resources/read", {{"uri", "rigmind://alerts"}})["result"]["contents"].size() == 1, "alerts resource");
    check(rpc(server, 11, "resources/read", {{"uri", "file:///etc/passwd"}}).contains("error"), "foreign uri refused");

    // HTTP transport
    const std::string token = rigmind::load_or_create_mcp_token(dir / "mcp-token.txt");
    check(token.size() == 64 && rigmind::load_or_create_mcp_token(dir / "mcp-token.txt") == token, "token created once, then reused");
    rigmind::McpHttp http_server(server, token);
    check(http_server.start(0), "listener starts on a free loopback port");
    const unsigned short port = http_server.port();
    const std::string host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    const std::string auth = "Authorization: Bearer " + token + "\r\n";
    const std::string rpc_body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    std::string ok = http(port, "POST", "/mcp", host + auth + "Content-Type: application/json\r\n", rpc_body);
    check(ok.rfind("200\n", 0) == 0 && json::parse(ok.substr(4))["result"]["tools"].size() == 10, "authorised request works");
    check(http(port, "POST", "/mcp", host, rpc_body).rfind("401", 0) == 0, "no token is refused");
    check(http(port, "POST", "/mcp", host + "Authorization: Bearer wrong\r\n", rpc_body).rfind("401", 0) == 0, "wrong token is refused");
    check(http(port, "POST", "/mcp", host + auth + "Origin: https://evil.example\r\n", rpc_body).rfind("403", 0) == 0, "browser Origin is refused");
    check(http(port, "POST", "/mcp", "Host: evil.example\r\n" + auth, rpc_body).rfind("403", 0) == 0, "foreign Host is refused (DNS rebinding)");
    check(http(port, "GET", "/mcp", host + auth, "").rfind("405", 0) == 0, "GET is not allowed");
    check(http(port, "POST", "/other", host + auth, rpc_body).rfind("404", 0) == 0, "other paths are 404");
    check(http(port, "POST", "/mcp", host + auth, "{not json").rfind("400", 0) == 0, "bad JSON is 400");
    check(http(port, "POST", "/mcp", host + auth, R"({"jsonrpc":"2.0","method":"notifications/initialized"})").rfind("202", 0) == 0, "notification is 202");
    http_server.stop();
    check(http(port, "POST", "/mcp", host + auth, rpc_body) == "connect failed", "stopped listener refuses connections");

    std::filesystem::remove_all(dir, cleanup); // the server still holds its database open here; a leftover temp folder is harmless
    if (failures) return 1;
    std::cout << "mcp tests passed\n";
    return 0;
}

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "exception: " << e.what() << '\n';
        return 2;
    }
}
