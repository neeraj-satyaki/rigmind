#pragma once

#include <string>

#include "alarm_state.h"
#include "audit_log.h"
#include "event_store.h"

namespace rigmind {

// Forwards stream commands to the rigmind-pipeline child process.
class StreamSink {
public:
    virtual ~StreamSink() = default;
    virtual void send(const nlohmann::json& command) = 0;
    // True when describe commands reach a language model. False: the caller keeps the template text and the
    // audit row says "template".
    virtual bool can_describe() const { return true; }
};

// JSON request/reply endpoint behind window.chrome.webview.postMessage.
// Request:  {"id": 1, "method": "alarm.step", "params": {...}}
// Reply:    {"id": 1, "ok": true, "result": {...}} or {"id": 1, "ok": false, "error": "..."}
// Stream answers arrive asynchronously from the sink as {"ev": "answer", ...} without an id.
// Calls run on the UI thread only, so no locking.
class Bridge {
public:
    explicit Bridge(EventStore& store, StreamSink* stream = nullptr, AuditLog* audit = nullptr);

    std::string handle(const std::string& request_json);

private:
    nlohmann::json simulate(const nlohmann::json& params);
    void audit_event(const std::string& action, const std::string& target, const std::string& detail);

    EventStore& store_;
    StreamSink* stream_;
    AuditLog* audit_;
    AlarmStateMachine alarm_;
    int next_event_ = 2000;
};

} // namespace rigmind
