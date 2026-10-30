// RigMind MCP module: the Model Context Protocol (JSON-RPC 2.0) surface over RigMind's own data, so an SLM or any MCP client can
// pull alerts, evidence, reports, the audit trail and the live inference scene without scraping the UI.
//
// Read-only except request_summary (asks BitNet to describe one alert, exactly like the "Plain words" button).
// The class is transport-free: mcp_http.cpp feeds it requests from 127.0.0.1; tests call handle() directly.
//
// Tools:      list_alerts, get_alert, get_live_scene, list_evidence, get_evidence, list_summaries, report_summary,
//             list_reports, get_audit, request_summary
// Resources:  rigmind://alerts, rigmind://summaries, rigmind://evidence, rigmind://scene/<cam>, rigmind://alert/<id>
//
// Thread-safe: HTTP worker threads call handle() concurrently; the database connection (its own, never the UI's) is guarded
// by one mutex.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/event_store.h"

namespace rigmind {

struct McpHooks {
    // Latest "scene" event of a camera ("" = any camera): objects with class name, box, centroid, mask contour, centroid path.
    std::function<nlohmann::json(const std::string& cam)> scene;
    std::function<std::vector<std::string>()> scene_cameras;
    // Asks the summary model for one alert and waits up to timeout_ms; {"text","source"} or null when it did not answer in time.
    std::function<nlohmann::json(const nlohmann::json& alert, int timeout_ms)> request_summary;
    std::filesystem::path footage_root;   // <data>\footage
    std::filesystem::path reports_dir;    // Documents\RigMind Reports
    std::function<std::int64_t()> now_ms; // test hook; default: system clock
    std::function<void(const std::string& tool, const std::string& detail)> audit; // every tool call (reading safety data is audited)
};

class McpServer {
public:
    // db_path: the RigMind database, opened as a second connection (read use only).
    McpServer(const std::string& db_path, McpHooks hooks);

    // One JSON-RPC message in, the response out; null for notifications (no response is sent).
    nlohmann::json handle(const nlohmann::json& request);

    static constexpr const char* kServerName = "rigmind";
    static constexpr const char* kServerVersion = "1.0.0";

private:
    nlohmann::json tools_list() const;
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args);
    nlohmann::json resources_list();
    nlohmann::json read_resource(const std::string& uri);

    nlohmann::json alert_detail(const std::string& id);
    nlohmann::json scene_for(const nlohmann::json& args);
    nlohmann::json evidence_detail(const nlohmann::json& args, nlohmann::json& images);
    nlohmann::json report_summary(int days);
    nlohmann::json list_report_files() const;
    nlohmann::json summaries(int limit);

    std::filesystem::path safe_evidence_dir(const std::string& rel) const; // throws when outside footage_root
    std::int64_t now() const;

    McpHooks hooks_;
    std::mutex mutex_;
    std::unique_ptr<EventStore> store_;
};

} // namespace rigmind
