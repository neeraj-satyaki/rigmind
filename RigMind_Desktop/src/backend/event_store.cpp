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

bool has_column(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (column_text(stmt, 1) == column) found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// Databases created before episode recording have only (id, timestamp_ms, from_state, to_state).
// Each new column is added once, guarded by a column check, so old rows keep their data.
void migrate_alarm_events(sqlite3* db) {
    struct Column { const char* name; const char* ddl; };
    static const Column kColumns[] = {
        {"cam_id", "TEXT NOT NULL DEFAULT ''"},
        {"alert_id", "TEXT NOT NULL DEFAULT ''"},
        {"condition", "TEXT NOT NULL DEFAULT ''"},
        {"place", "TEXT NOT NULL DEFAULT ''"},
        {"frame", "INTEGER NOT NULL DEFAULT 0"},
        {"severity", "TEXT NOT NULL DEFAULT ''"},
    };
    for (const Column& col : kColumns) {
        if (has_column(db, "alarm_events", col.name)) continue;
        exec_or_throw(db, (std::string("ALTER TABLE alarm_events ADD COLUMN \"") + col.name + "\" " + col.ddl).c_str());
    }
}

// Footage rows written before image evidence have no kind: they are video clip sets.
void migrate_footage(sqlite3* db) {
    if (!has_column(db, "footage", "kind")) exec_or_throw(db, "ALTER TABLE footage ADD COLUMN kind TEXT NOT NULL DEFAULT 'video'");
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
    migrate_alarm_events(db_);
    migrate_footage(db_);
}

EventStore::~EventStore() {
    if (db_) sqlite3_close(db_);
}

void EventStore::record_transition(AlarmState from, AlarmState to, std::int64_t timestamp_ms, const TransitionDetail* detail) {
    if (detail) {
        run(db_,
            "INSERT INTO alarm_events (timestamp_ms, from_state, to_state, cam_id, alert_id, condition, place, severity) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_int64(s, 1, timestamp_ms);
                sqlite3_bind_text(s, 2, to_string(from), -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 3, to_string(to), -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 4, detail->cam_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 5, detail->alert_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 6, detail->condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 7, detail->place.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 8, detail->severity.c_str(), -1, SQLITE_TRANSIENT);
            });
        return;
    }
    run(db_, "INSERT INTO alarm_events (timestamp_ms, from_state, to_state) VALUES (?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_int64(s, 1, timestamp_ms);
            sqlite3_bind_text(s, 2, to_string(from), -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 3, to_string(to), -1, SQLITE_STATIC);
        });
}

std::int64_t EventStore::record_alarm_episode(const AlarmEpisodeRecord& r) {
    exec_or_throw(db_, "BEGIN IMMEDIATE");
    try {
        run(db_,
            "INSERT INTO alarm_events (timestamp_ms, from_state, to_state, cam_id, alert_id, condition, place, frame, severity) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_int64(s, 1, r.timestamp_ms);
                sqlite3_bind_text(s, 2, to_string(r.from), -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 3, to_string(r.to), -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 4, r.cam_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 5, r.alert_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 6, r.condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 7, r.place.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 8, r.frame);
                sqlite3_bind_text(s, 9, r.severity.c_str(), -1, SQLITE_TRANSIENT);
            });
        const std::int64_t row_id = sqlite3_last_insert_rowid(db_);
        // Cleared keeps the condition and place of the last escalated state when the event has none.
        run(db_,
            "INSERT INTO alerts (id, severity, condition, place, status, raised_ms, updated_ms) VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            " severity = excluded.severity,"
            " status = excluded.status,"
            " condition = CASE WHEN excluded.condition = '' THEN alerts.condition ELSE excluded.condition END,"
            " place = CASE WHEN excluded.place = '' THEN alerts.place ELSE excluded.place END,"
            " updated_ms = excluded.updated_ms",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, r.alert_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, r.severity.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, r.condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 4, r.place.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 5, r.status.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 6, r.timestamp_ms);
                sqlite3_bind_int64(s, 7, r.timestamp_ms);
            });
        exec_or_throw(db_, "COMMIT");
        return row_id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

nlohmann::json EventStore::recent_transitions(int limit) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, timestamp_ms, from_state, to_state, cam_id, alert_id, condition, place, frame, severity "
                      "FROM alarm_events ORDER BY id DESC LIMIT ?";
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
            {"camId", column_text(stmt, 4)},
            {"alertId", column_text(stmt, 5)},
            {"condition", column_text(stmt, 6)},
            {"place", column_text(stmt, 7)},
            {"frame", sqlite3_column_int64(stmt, 8)},
            {"severity", column_text(stmt, 9)},
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

void EventStore::upsert_alert(const std::string& id, const std::string& severity, const std::string& condition,
                              const std::string& place, const std::string& status, std::int64_t timestamp_ms) {
    run(db_,
        "INSERT INTO alerts (id, severity, condition, place, status, raised_ms, updated_ms) VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET severity = excluded.severity, status = excluded.status, condition = excluded.condition,"
        " place = excluded.place, updated_ms = excluded.updated_ms",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, severity.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, condition.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, place.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 6, timestamp_ms);
            sqlite3_bind_int64(s, 7, timestamp_ms);
        });
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
    // A person's response (Acknowledged, Escalated) shows until the alert changes again (escalation or clear).
    const char* sql = "SELECT a.id, a.severity, a.condition, a.place,"
                      " CASE WHEN r.id IS NOT NULL AND r.updated_ms >= a.updated_ms THEN r.status ELSE a.status END,"
                      " CASE WHEN r.id IS NOT NULL THEN r.owner ELSE a.owner END,"
                      " a.raised_ms, a.updated_ms, a.status FROM alerts a LEFT JOIN alert_responses r ON r.id = a.id "
                      "ORDER BY a.raised_ms DESC, a.id DESC LIMIT ?";
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
            {"updatedMs", sqlite3_column_int64(stmt, 7)},
            {"phase", column_text(stmt, 8)}, // lifecycle (Open | Monitoring | Escalated | Cleared), apart from a person's response
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

nlohmann::json EventStore::recent_audit(const std::string& action, int limit) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, ts_ms, source, action, target, detail FROM audit_log WHERE action = ? "
                      "ORDER BY id DESC LIMIT ?";
    nlohmann::json rows = nlohmann::json::array();
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return rows; // no audit_log table yet
    sqlite3_bind_text(stmt, 1, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({
            {"id", sqlite3_column_int64(stmt, 0)},
            {"tsMs", sqlite3_column_int64(stmt, 1)},
            {"source", column_text(stmt, 2)},
            {"action", column_text(stmt, 3)},
            {"target", column_text(stmt, 4)},
            {"detail", column_text(stmt, 5)},
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
                                const std::string& path, const std::string& kind) {
    const std::string stamp = std::to_string(timestamp_ms);
    if (!safe_cam_id(cam_id) || !safe_stamp(stamp) || path != cam_id + "/" + stamp || (kind != "video" && kind != "image"))
        throw std::runtime_error("invalid footage record");
    run(db_, "INSERT INTO footage (cam_id, ts_ms, clip_count, path, expires_ms, kind) VALUES (?, ?, ?, ?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, cam_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, timestamp_ms);
            sqlite3_bind_int(s, 3, clip_count);
            sqlite3_bind_text(s, 4, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 5, timestamp_ms + kFootageRetentionMs);
            sqlite3_bind_text(s, 6, kind.c_str(), -1, SQLITE_TRANSIENT);
        });
}

nlohmann::json EventStore::list_footage(int limit) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, cam_id, ts_ms, clip_count, path, expires_ms, kind FROM footage ORDER BY ts_ms DESC, id DESC LIMIT ?";
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
            {"kind", column_text(stmt, 6)},
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
    std::vector<std::string> files, images;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".mp4") files.push_back(entry.path().filename().string());
        else if (entry.path().extension() == ".jpg") images.push_back(entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    std::sort(images.begin(), images.end());
    return {{"dir", dir.string()}, {"files", files}, {"images", images}, {"kind", (images.empty() || !files.empty()) ? "video" : "image"}};
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
