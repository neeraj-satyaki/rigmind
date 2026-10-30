#pragma once

#include <optional>
#include <string>

#include "alarm_state.h"
#include "audit_log.h"
#include "auth_service.h"
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

    // Page-facing entry. With sign-in required (set_auth_required), every method except the sign-in calls is refused with
    // "Sign in required." until auth.login / auth.setup has succeeded in this host process; auth.logout and an expired session
    // close the door again. The role and user come from the host's own session, never from anything the page sends.
    std::string handle(const std::string& request_json) { return handle_impl(request_json, false); }
    // The host's own internal calls (clip records, MCP summary requests): no session needed.
    std::string handle_trusted(const std::string& request_json) { return handle_impl(request_json, true); }

    void set_auth_required(bool required) { auth_required_ = required; }
    // True when sign-in is not required, or a session is active.
    bool authenticated() const;

private:
    std::string handle_impl(const std::string& request_json, bool trusted);
    static bool allowed_before_sign_in(const std::string& method, const nlohmann::json& params);
    struct Session {
        std::string username, role;
        std::int64_t expires_ms = 0;
    };
    void start_session(const nlohmann::json& user);

    nlohmann::json simulate(const nlohmann::json& params);
    void audit_event(const std::string& action, const std::string& target, const std::string& detail);

    EventStore& store_;
    StreamSink* stream_;
    AuditLog* audit_;
    AlarmStateMachine alarm_;
    AuthService auth_;
    bool auth_required_ = false;
    std::optional<Session> session_;
    static constexpr std::int64_t kSessionMs = 12LL * 3600 * 1000; // a session lasts one shift
    int next_event_ = 2000;
};

} // namespace rigmind
