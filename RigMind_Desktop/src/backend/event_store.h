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
    // Optional episode details (alert id, severity, condition, place, camera) are stored on the row when given, so a
    // transition can be joined to its alert. The scripted alarm.simulate path passes them; alarm.step does not.
    struct TransitionDetail {
        std::string alert_id, severity, condition, place, cam_id;
    };
    void record_transition(AlarmState from, AlarmState to, std::int64_t timestamp_ms, const TransitionDetail* detail = nullptr);
    nlohmann::json recent_transitions(int limit) const;

    // One EWS episode transition, recorded atomically: an alarm_events row (with camera, alert id, condition,
    // place, frame, severity) and the single alerts row of the episode, inserted on the first call for the
    // alert id and updated in place afterwards (severity never lowers within an episode; status is
    // Monitoring | Escalated | Cleared). Returns the id of the alarm_events row.
    struct AlarmEpisodeRecord {
        std::string cam_id;
        std::string alert_id;
        AlarmState from = AlarmState::None;
        AlarmState to = AlarmState::None;
        std::string severity;  // severity of the alert after this transition (Advisory | Warning | Critical)
        std::string status;    // Monitoring | Escalated | Cleared
        std::string condition;        // neutral label of this transition's state (alarm_events row)
        std::string alert_condition;  // neutral label of the alert row (the peak level); empty = use `condition`
        std::string place;
        std::int64_t frame = 0;
        std::int64_t timestamp_ms = 0;
    };
    std::int64_t record_alarm_episode(const AlarmEpisodeRecord& record);

    // Inserts or updates one alerts row by id (severity, condition, place, status); raised_ms stays from the first call.
    void upsert_alert(const std::string& id, const std::string& severity, const std::string& condition,
                      const std::string& place, const std::string& status, std::int64_t timestamp_ms);

    // Safety alerts (toast, bell and push are presented by the UI from raised rows).
    // list_alerts() returns the stored responses (id, status, owner), which the UI merges into its alert list.
    void raise_alert(const std::string& id, const std::string& severity, const std::string& condition,
                     const std::string& place, std::int64_t timestamp_ms);
    void respond_alert(const std::string& id, const std::string& status, const std::string& owner,
                       std::int64_t timestamp_ms);
    nlohmann::json list_alerts() const;
    // Raised alerts with their current status and owner, newest first (the live alert feed).
    // Rows carry `metadata` (facts JSON, see alert_metadata.h, or null) and `summary` / `summarySource` / `summaryAt`
    // (the SLM sentence; source 'slm', or 'template' when the model gave none; time; null while none was requested).
    nlohmann::json list_raised_alerts(int limit) const;
    // One row of the same shape, or null.
    nlohmann::json find_raised_alert(const std::string& id) const;

    // Alert metadata (facts only, JSON text) by alert id; replaced on every change of the episode.
    void put_alert_metadata(const std::string& alert_id, const std::string& json_text, std::int64_t timestamp_ms);
    std::optional<std::string> get_alert_metadata(const std::string& alert_id) const;
    // The SLM result for an alert. Empty text with source 'template' means the model gave nothing; it never overwrites
    // a sentence stored earlier. The next result for the same alert id replaces the stored one.
    void put_alert_summary(const std::string& alert_id, const std::string& text, const std::string& source, std::int64_t timestamp_ms);

    // Rows of the audit_log table (written by AuditLog) with this exact action, newest first. Empty when the
    // table does not exist yet. Rows still in the audit queue are not included.
    nlohmann::json recent_audit(const std::string& action, int limit) const;

    // Alarm footage: one row per saved clip set. Rows and their files expire after 7 days.
    // kind: "video" (Red clip set, .mp4 segments) or "image" (Orange snapshot set, orange-N.jpg). clip_count is the file count.
    void record_footage(const std::string& cam_id, std::int64_t timestamp_ms, int clip_count, const std::string& path,
                        const std::string& kind = "video");
    nlohmann::json list_footage(int limit) const; // rows carry "kind"; rows written before the column existed are "video"
    // Folder for playback: {"dir", "files": [.mp4 names], "images": [.jpg names], "kind"}. Path is <cam_id>/<stamp>, validated.
    nlohmann::json list_footage_files(const std::string& path) const;
    // Deletes expired rows and their clip folders. Returns the number of rows removed.
    int prune_footage(std::int64_t now_ms);
    // Clip folders live under this root as <cam_id>/<stamp>. Paths are validated against it before any delete.
    void set_footage_root(const std::string& root);

    // Local accounts (never returned to the page): username is lower case and unique; salt and hash are hex.
    struct UserRow {
        std::string username, display_name, salt_hex, hash_hex, role;
        std::uint32_t iterations = 0;
        std::int64_t created_ms = 0, last_login_ms = 0;
    };
    bool has_users() const;
    void create_user(const UserRow& user);
    std::optional<UserRow> get_user(const std::string& username) const;
    void touch_login(const std::string& username, std::int64_t timestamp_ms);

    // Key/value records; values are JSON text
    void put_record(const std::string& key, const std::string& json_value);
    std::optional<std::string> get_record(const std::string& key) const;
    nlohmann::json list_records(const std::string& prefix) const;

private:
    sqlite3* db_ = nullptr;
    std::string footage_root_;
};

} // namespace rigmind
