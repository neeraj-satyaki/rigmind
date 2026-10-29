#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "alarm_state.h"

struct sqlite3;

namespace rigmind {

// Local SQLite file per device (%LOCALAPPDATA%\RigMind\rigmind.db).
// Holds alarm transitions, safety alerts with their responses, and the key/value records
// that back preferences, camera details, HSE decisions and report summaries.
class EventStore {
public:
    // Throws std::runtime_error when the database cannot be opened or initialised.
    explicit EventStore(const std::string& path);
    ~EventStore();

    EventStore(const EventStore&) = delete;
    EventStore& operator=(const EventStore&) = delete;

    // Alarm transitions
    void record_transition(AlarmState from, AlarmState to, std::int64_t timestamp_ms);
    nlohmann::json recent_transitions(int limit) const;

    // Safety alerts (toast, bell and push are presented by the UI from raised rows).
    // list_alerts() returns the stored responses (id, status, owner), which the UI merges into its alert list.
    void raise_alert(const std::string& id, const std::string& severity, const std::string& condition,
                     const std::string& place, std::int64_t timestamp_ms);
    void respond_alert(const std::string& id, const std::string& status, const std::string& owner,
                       std::int64_t timestamp_ms);
    nlohmann::json list_alerts() const;
    // Raised alerts with their current status and owner, newest first (the live alert feed).
    nlohmann::json list_raised_alerts(int limit) const;

    // Alarm footage: one row per saved clip set. Rows and their files expire after 7 days.
    void record_footage(const std::string& cam_id, std::int64_t timestamp_ms, int clip_count, const std::string& path);
    nlohmann::json list_footage(int limit) const;
    // Clip folder for playback: {"dir": full folder path, "files": [.mp4 names]}. Path is <cam_id>/<stamp>, validated.
    nlohmann::json list_footage_files(const std::string& path) const;
    // Deletes expired rows and their clip folders. Returns the number of rows removed.
    int prune_footage(std::int64_t now_ms);
    // Clip folders live under this root as <cam_id>/<stamp>. Paths are validated against it before any delete.
    void set_footage_root(const std::string& root);

    // Key/value records; values are JSON text
    void put_record(const std::string& key, const std::string& json_value);
    std::optional<std::string> get_record(const std::string& key) const;
    nlohmann::json list_records(const std::string& prefix) const;

private:
    sqlite3* db_ = nullptr;
    std::string footage_root_;
};

} // namespace rigmind
