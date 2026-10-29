#include "event_store.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "sqlite3.h"

namespace rigmind {

namespace {

void exec_or_throw(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "sqlite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

// Prepares, binds and runs one statement that returns no rows.
void run(sqlite3* db, const char* sql, const std::function<void(sqlite3_stmt*)>& bind) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    bind(stmt);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}

std::string column_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : std::string();
}

} // namespace

EventStore::EventStore(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string message = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open event database: " + message);
    }
    // The audit worker writes through its own connection: wait for its lock instead of failing.
    sqlite3_busy_timeout(db_, 5000);
    // WAL + NORMAL: a commit no longer waits for a full fsync (~5 ms each). Losing the last commit on power
    // failure is accepted; the database itself stays consistent.
    exec_or_throw(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    exec_or_throw(db_,
                  "CREATE TABLE IF NOT EXISTS alarm_events ("
                  " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                  " timestamp_ms INTEGER NOT NULL,"
                  " from_state TEXT NOT NULL,"
                  " to_state TEXT NOT NULL);"
                  "CREATE TABLE IF NOT EXISTS alerts ("
                  " id TEXT PRIMARY KEY,"
                  " severity TEXT NOT NULL,"
                  " condition TEXT NOT NULL,"
                  " place TEXT NOT NULL,"
                  " status TEXT NOT NULL DEFAULT 'Open',"
                  " owner TEXT NOT NULL DEFAULT 'Unassigned',"
                  " raised_ms INTEGER NOT NULL,"
                  " updated_ms INTEGER NOT NULL);"
                  "CREATE TABLE IF NOT EXISTS alert_responses ("
                  " id TEXT PRIMARY KEY,"
                  " status TEXT NOT NULL,"
                  " owner TEXT NOT NULL,"
                  " updated_ms INTEGER NOT NULL);"
                  "CREATE TABLE IF NOT EXISTS footage ("
                  " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                  " cam_id TEXT NOT NULL,"
                  " ts_ms INTEGER NOT NULL,"
                  " clip_count INTEGER NOT NULL,"
                  " path TEXT NOT NULL,"
                  " expires_ms INTEGER NOT NULL);"
                  "CREATE TABLE IF NOT EXISTS app_records ("
                  " key TEXT PRIMARY KEY,"
                  " value TEXT NOT NULL);");
}

EventStore::~EventStore() {
    if (db_) sqlite3_close(db_);
}

void EventStore::record_transition(AlarmState from, AlarmState to, std::int64_t timestamp_ms) {
    run(db_, "INSERT INTO alarm_events (timestamp_ms, from_state, to_state) VALUES (?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_int64(s, 1, timestamp_ms);
            sqlite3_bind_text(s, 2, to_string(from), -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 3, to_string(to), -1, SQLITE_STATIC);
        });
}

nlohmann::json EventStore::recent_transitions(int limit) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, timestamp_ms, from_state, to_state FROM alarm_events ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int(stmt, 1, limit);

    nlohmann::json rows = nlohmann::json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            {"id", sqlite3_column_int64(stmt, 0)},
            {"timestampMs", sqlite3_column_int64(stmt, 1)},
            {"from", column_text(stmt, 2)},
            {"to", column_text(stmt, 3)},
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

void EventStore::raise_alert(const std::string& id, const std::string& severity, const std::string& condition,
                             const std::string& place, std::int64_t timestamp_ms) {
    // One row per alert id. A repeat raise (same episode, higher severity) upgrades the row in place
    // and never adds a second row. A lower or equal severity leaves the row unchanged.
    const char* sql =
        "INSERT INTO alerts (id, severity, condition, place, raised_ms, updated_ms) VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        " condition = CASE WHEN "
        "   (CASE excluded.severity WHEN 'Critical' THEN 3 WHEN 'Warning' THEN 2 ELSE 1 END) > "
        "   (CASE alerts.severity WHEN 'Critical' THEN 3 WHEN 'Warning' THEN 2 ELSE 1 END) "
        "   THEN excluded.condition ELSE alerts.condition END,"
        " severity = CASE WHEN "
        "   (CASE excluded.severity WHEN 'Critical' THEN 3 WHEN 'Warning' THEN 2 ELSE 1 END) > "
        "   (CASE alerts.severity WHEN 'Critical' THEN 3 WHEN 'Warning' THEN 2 ELSE 1 END) "
        "   THEN excluded.severity ELSE alerts.severity END,"
        " updated_ms = excluded.updated_ms";
    run(db_, sql, [&](sqlite3_stmt* s) {
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, severity.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, condition.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, place.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, timestamp_ms);
        sqlite3_bind_int64(s, 6, timestamp_ms);
    });
}

// Responses are kept apart from raised alerts, so a response to any alert id is stored.
void EventStore::respond_alert(const std::string& id, const std::string& status, const std::string& owner,
                               std::int64_t timestamp_ms) {
    run(db_, "INSERT OR REPLACE INTO alert_responses (id, status, owner, updated_ms) VALUES (?, ?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, owner.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 4, timestamp_ms);
        });
}

nlohmann::json EventStore::list_alerts() const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, status, owner FROM alert_responses ORDER BY updated_ms DESC, id DESC";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    nlohmann::json rows = nlohmann::json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            {"id", column_text(stmt, 0)},
            {"status", column_text(stmt, 1)},
            {"owner", column_text(stmt, 2)},
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

nlohmann::json EventStore::list_raised_alerts(int limit) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, severity, condition, place, status, owner, raised_ms FROM alerts "
                      "ORDER BY raised_ms DESC, id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int(stmt, 1, limit);
    nlohmann::json rows = nlohmann::json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            {"id", column_text(stmt, 0)},
            {"severity", column_text(stmt, 1)},
            {"condition", column_text(stmt, 2)},
            {"place", column_text(stmt, 3)},
            {"status", column_text(stmt, 4)},
            {"owner", column_text(stmt, 5)},
            {"raisedMs", sqlite3_column_int64(stmt, 6)},
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

void EventStore::put_record(const std::string& key, const std::string& json_value) {
    run(db_, "INSERT OR REPLACE INTO app_records (key, value) VALUES (?, ?)", [&](sqlite3_stmt* s) {
        sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, json_value.c_str(), -1, SQLITE_TRANSIENT);
    });
}

std::optional<std::string> EventStore::get_record(const std::string& key) const {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM app_records WHERE key = ?", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> value;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = column_text(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

namespace {
constexpr std::int64_t kFootageRetentionMs = 7LL * 24 * 60 * 60 * 1000;
}

namespace {

// Camera ids: letters, digits, '_' and '-'. Folder stamps: digits only. Anything else never reaches the disk.
bool safe_cam_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

bool safe_stamp(const std::string& stamp) {
    if (stamp.empty() || stamp.size() > 20) return false;
    for (char c : stamp) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // namespace

void EventStore::set_footage_root(const std::string& root) {
    footage_root_ = root;
}

void EventStore::record_footage(const std::string& cam_id, std::int64_t timestamp_ms, int clip_count,
                                const std::string& path) {
    const std::string stamp = std::to_string(timestamp_ms);
    if (!safe_cam_id(cam_id) || !safe_stamp(stamp) || path != cam_id + "/" + stamp)
        throw std::runtime_error("invalid footage record");
    run(db_, "INSERT INTO footage (cam_id, ts_ms, clip_count, path, expires_ms) VALUES (?, ?, ?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, cam_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, timestamp_ms);
            sqlite3_bind_int(s, 3, clip_count);
            sqlite3_bind_text(s, 4, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 5, timestamp_ms + kFootageRetentionMs);
        });
}

nlohmann::json EventStore::list_footage(int limit) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, cam_id, ts_ms, clip_count, path, expires_ms FROM footage ORDER BY ts_ms DESC, id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    sqlite3_bind_int(stmt, 1, limit);
    nlohmann::json rows = nlohmann::json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            {"id", sqlite3_column_int64(stmt, 0)},
            {"camId", column_text(stmt, 1)},
            {"timestampMs", sqlite3_column_int64(stmt, 2)},
            {"clipCount", sqlite3_column_int(stmt, 3)},
            {"path", column_text(stmt, 4)},
            {"expiresMs", sqlite3_column_int64(stmt, 5)},
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

nlohmann::json EventStore::list_footage_files(const std::string& path) const {
    const auto slash = path.find('/');
    if (footage_root_.empty() || slash == std::string::npos || !safe_cam_id(path.substr(0, slash)) ||
        !safe_stamp(path.substr(slash + 1)))
        throw std::runtime_error("invalid footage path");
    const std::filesystem::path dir = std::filesystem::path(footage_root_) / path;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".mp4") files.push_back(entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    return {{"dir", dir.string()}, {"files", files}};
}

int EventStore::prune_footage(std::int64_t now) {
    std::vector<std::pair<std::int64_t, std::string>> expired;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, path FROM footage WHERE expires_ms < ?", -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
    sqlite3_bind_int64(stmt, 1, now);
    while (sqlite3_step(stmt) == SQLITE_ROW) expired.emplace_back(sqlite3_column_int64(stmt, 0), column_text(stmt, 1));
    sqlite3_finalize(stmt);

    for (const auto& [id, path] : expired) {
        // Only folders named <cam>/<stamp> directly under the footage root may be removed.
        const auto slash = path.find('/');
        if (!footage_root_.empty() && slash != std::string::npos && safe_cam_id(path.substr(0, slash)) &&
            safe_stamp(path.substr(slash + 1))) {
            std::error_code ignored;
            std::filesystem::remove_all(std::filesystem::path(footage_root_) / path, ignored);
        }
        run(db_, "DELETE FROM footage WHERE id = ?", [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, id); });
    }
    return static_cast<int>(expired.size());
}

nlohmann::json EventStore::list_records(const std::string& prefix) const {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT key, value FROM app_records WHERE key LIKE ? ORDER BY key", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    const std::string pattern = prefix + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    nlohmann::json rows = nlohmann::json::array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            {"key", column_text(stmt, 0)},
            {"value", nlohmann::json::parse(column_text(stmt, 1))},
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

} // namespace rigmind
