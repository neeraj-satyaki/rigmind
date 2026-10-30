#pragma once

#include <optional>
#include <functional>
#include <string>

#include "alarm_state.h"
#include "audit_log.h"
#include "auth_service.h"
#include "chat_sessions.h"
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
    // Test mode of the bridge CLI: cheap password hashing for generated cases. Never used by the app.
    void set_auth_rounds(std::uint32_t rounds) { auth_.set_iterations(rounds); }
    // Called (on the calling thread) after a record was saved through records.set, with its key. The host uses it to push saved
    // zones to the pipeline.
    void set_record_hook(std::function<void(const std::string&)> hook) { record_hook_ = std::move(hook); }
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
    ChatSessions chats_;
    bool auth_required_ = false;
    std::function<void(const std::string&)> record_hook_;
    std::optional<Session> session_;
    static constexpr std::int64_t kSessionMs = 12LL * 3600 * 1000; // a session lasts one shift
    int next_event_ = 2000;
    int next_chat_ = 1;
};

} // namespace rigmind
