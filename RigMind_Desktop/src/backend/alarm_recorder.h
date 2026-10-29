#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "audit_log.h"
#include "event_store.h"

namespace rigmind {

// The host is the authority for alarm history. Every {"ev":"alarm"} line from the pipeline goes through
// handle() before it is forwarded to the page, whether or not the page is open.
//
// Episode model: an episode runs from the first non-None state of a camera until that camera returns to None.
// One alerts row represents the whole episode and changes in place:
//   Yellow -> Advisory, Monitoring   (quiet: the app is only watching)
//   Orange -> Warning,  Escalated
//   Red    -> Critical, Escalated
//   None   -> keeps the peak severity, Cleared
// Each state change writes one alarm_events row and one audit row ('alarm.transition'). A repeated state
// writes nothing. A new episode after None opens a new alert id.
//
// handle() returns the event enriched with:
//   alertId     "ALM-<cam>-<boot><n>" (32 chars max, stable for the episode, unique across restarts)
//   episode     n (per camera, starts at 1 after each restart)
//   phase       "Monitoring" | "Escalated" | "Cleared"
//   escalation  0 Yellow, 1 Orange, 2 Red (level of the current state)
//   peak        highest escalation level reached in the episode
//   changed     true when this line was a transition that was recorded
// A None line for a camera that never had an episode is returned unchanged (no alertId).
// Thread-safe: called from the pipeline reader thread.
class AlarmRecorder {
public:
    // boot: short id (letters and digits) that keeps alert ids apart across restarts. Empty derives one from
    // the clock. audit may be null.
    AlarmRecorder(EventStore& store, AuditLog* audit, std::string boot = {});

    nlohmann::json handle(const nlohmann::json& event, std::int64_t now_ms);

    const std::string& boot() const { return boot_; }

private:
    struct Camera {
        AlarmState state = AlarmState::None;
        int episode = 0;
        int peak = -1;            // -1 none, 0 Yellow, 1 Orange, 2 Red
        std::string alert_id;
        std::string condition;
        std::string place;
    };

    std::string make_alert_id(const std::string& cam, int episode) const;

    EventStore& store_;
    AuditLog* audit_;
    std::string boot_;
    std::mutex mutex_;
    std::map<std::string, Camera> cameras_;
};

// PPE compliance episodes ({"ev":"ppe"} lines from the pipeline: state "missing" with the items not detected, or "ok").
// One alerts row per episode, Advisory severity, status Open while something is missing and Cleared afterwards. The row
// is updated in place when the missing set changes. Each raise, update and clear writes one audit row ('ppe.transition').
// handle() returns the event enriched with alertId ("PPE-<cam>-<boot><n>"), episode, phase ("Open" | "Cleared"),
// condition ("PPE not detected: vest, gloves"), severity and changed. A repeat with the same missing set changes
// nothing; an "ok" with no open episode is returned unchanged. Thread-safe.
class PpeRecorder {
public:
    PpeRecorder(EventStore& store, AuditLog* audit, std::string boot = {});
    nlohmann::json handle(const nlohmann::json& event, std::int64_t now_ms);

private:
    struct Camera {
        bool open = false;
        int episode = 0;
        int persons = 0;
        std::string alert_id, condition;
        std::vector<std::string> missing;
    };
    EventStore& store_;
    AuditLog* audit_;
    std::string boot_;
    std::mutex mutex_;
    std::map<std::string, Camera> cameras_;
};

} // namespace rigmind
