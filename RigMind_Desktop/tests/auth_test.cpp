// Local accounts: PBKDF2-HMAC-SHA256 known answers, first-run setup, sign-in, lockout, bridge methods and the audit trail.
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <string>

#include "backend/audit_log.h"
#include "backend/auth_service.h"
#include "backend/bridge.h"
#include "backend/event_store.h"
#include "backend/password_hash.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
using json = nlohmann::json;
rigmind::Bytes bytes(const std::string& s) { return rigmind::Bytes(s.begin(), s.end()); }

bool throws_with(const std::function<void()>& fn, const std::string& needle) {
    try {
        fn();
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}
} // namespace

static int run() {
    // ---- known answers
    check(rigmind::to_hex(rigmind::sha256(bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    check(rigmind::to_hex(rigmind::sha256(bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of empty");
    check(rigmind::to_hex(rigmind::hmac_sha256(bytes("key"), bytes("The quick brown fox jumps over the lazy dog"))) ==
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", "HMAC-SHA256 known answer");
    // RFC 7914 section 11: PBKDF2-HMAC-SHA-256, P="passwd", S="salt", c=1, dkLen=64
    check(rigmind::to_hex(rigmind::pbkdf2_sha256("passwd", bytes("salt"), 1, 64)) ==
              "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
          "PBKDF2-HMAC-SHA256 RFC 7914 vector");
    // RFC 6070-style vector for SHA-256 (c=4096): password/salt -> c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a
    check(rigmind::to_hex(rigmind::pbkdf2_sha256("password", bytes("salt"), 4096, 32)) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
          "PBKDF2-HMAC-SHA256 4096 rounds");
    {
        // cost check: the production setting (600,000 rounds) must stay well under a second or two on a normal PC
        const auto t0 = std::chrono::steady_clock::now();
        rigmind::pbkdf2_sha256("timing", bytes("salt-salt-salt-16"), 100000, 32);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "pbkdf2 100000 rounds: " << ms << " ms (600000 rounds is about " << ms * 6 << " ms)\n";
        check(ms * 6 < 6000, "600000 rounds finish in under 6 seconds");
    }
    check(rigmind::random_bytes(16) != rigmind::random_bytes(16), "random bytes differ");
    check(rigmind::from_hex(rigmind::to_hex(bytes("hello"))) == bytes("hello"), "hex round trip");
    check(throws_with([] { rigmind::from_hex("abc"); }, "odd"), "odd hex refused");

    // ---- policy
    check(rigmind::AuthService::password_problem("alice", "short1").find("10 characters") != std::string::npos, "short password refused");
    check(!rigmind::AuthService::password_problem("alice", "alice-is-here-1").empty(), "password containing the username refused");
    check(!rigmind::AuthService::password_problem("alice", "1234567890123").empty(), "digits only refused");
    check(rigmind::AuthService::password_problem("alice", "correct horse 7").empty(), "good password accepted");
    check(rigmind::AuthService::normalise_username("  Alice.Smith@Rig.Example ") == "alice.smith@rig.example", "username is trimmed and lower-cased");
    check(throws_with([] { rigmind::AuthService::normalise_username("a b"); }, "letters"), "spaces in a username refused");
    check(throws_with([] { rigmind::AuthService::normalise_username("ab"); }, "3 to 64"), "too short username refused");

    const auto dir = std::filesystem::temp_directory_path() / ("rigmind-auth-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    {
        rigmind::EventStore store(db);
        rigmind::AuditLog audit(db);
        rigmind::AuthService::Options fast;
        fast.iterations = 1000; // tests only
        rigmind::AuthService auth(store, &audit, fast);
        std::int64_t t = 1'000'000;

        check(auth.status()["hasUsers"] == false, "no account at first");
        check(throws_with([&] { auth.login("alice", "whatever-1234", t); }, "Wrong username or password"), "login with no accounts is refused");
        check(throws_with([&] { auth.setup("alice", "weak", t); }, "10 characters"), "setup enforces the policy");
        const json made = auth.setup("Alice", "correct horse 7", t);
        check(made["username"] == "alice" && made["role"] == "Administrator", "first account is the administrator");
        check(auth.status()["hasUsers"] == true, "account exists");
        check(throws_with([&] { auth.setup("bob", "another pass 8", t); }, "already exists"), "setup is refused once an account exists");

        // the stored row holds a salted hash, never the password
        const auto row = store.get_user("alice");
        check(row && row->hash_hex.size() == 64 && row->salt_hex.size() == 32 && row->iterations == 1000, "salted PBKDF2 hash stored");
        check(row && row->hash_hex.find("correct") == std::string::npos && row->salt_hex != row->hash_hex, "no plain password in the row");

        // sign-in
        check(auth.login(" ALICE ", "correct horse 7", t += 1000)["username"] == "alice", "right password signs in (username case and spaces ignored)");
        check(throws_with([&] { auth.login("alice", "correct horse 8", t += 1000); }, "Wrong username or password"), "wrong password refused");
        check(throws_with([&] { auth.login("nobody", "correct horse 7", t += 1000); }, "Wrong username or password"), "unknown user gets the same answer");
        check(throws_with([&] { auth.login("not valid!", "x", t += 1000); }, "Wrong username or password"), "impossible username gets the same answer");

        // lockout after 5 failures, then the right password is refused until the lock ends
        for (int i = 0; i < 4; ++i) throws_with([&] { auth.login("alice", "bad-bad-bad-" + std::to_string(i), t += 1000); }, "Wrong");
        check(throws_with([&] { auth.login("alice", "correct horse 7", t += 1000); }, "Try again in"), "locked after repeated failures, even with the right password");
        check(auth.login("alice", "correct horse 7", t += 31'000)["username"] == "alice", "unlocks after the lock time");
        // and a success clears the counter
        for (int i = 0; i < 3; ++i) throws_with([&] { auth.login("alice", "bad-again-" + std::to_string(i), t += 1000); }, "Wrong");
        check(auth.login("alice", "correct horse 7", t += 1000)["username"] == "alice", "still allowed under the limit");

        // bridge methods
        rigmind::Bridge bridge(store, nullptr, &audit);
        auto call = [&](const char* method, json params) { return json::parse(bridge.handle(json{{"id", 1}, {"method", method}, {"params", params}}.dump())); };
        check(call("auth.status", json::object())["result"]["hasUsers"] == true, "bridge: status");
        check(call("auth.login", {{"username", "alice"}, {"password", "nope-nope-nope"}})["ok"] == false, "bridge: wrong password is an error");
        check(call("auth.login", {{"username", "alice"}})["ok"] == false, "bridge: missing password is an error");
        check(call("auth.setup", {{"username", "eve"}, {"password", "evil password 9"}})["ok"] == false, "bridge: setup refused when an account exists");
        check(call("auth.logout", {{"username", "alice"}})["ok"] == true, "bridge: logout");
        // host-side enforcement: with sign-in required the bridge refuses everything but the sign-in calls until someone signs in
        rigmind::Bridge gated(store, nullptr, &audit);
        gated.set_auth_required(true);
        auto gcall = [&](const char* method, json params) { return json::parse(gated.handle(json{{"id", 1}, {"method", method}, {"params", params}}.dump())); };
        check(!gated.authenticated(), "gated: nobody is signed in at first");
        for (const char* blocked : {"alerts.feed", "alerts.list", "footage.list", "audit.recent", "events.recent", "stream.open", "alert.respond", "alarm.simulate", "records.list", "event.describe"}) {
            const json r = gcall(blocked, {{"limit", 5}});
            check(r["ok"] == false && r["error"] == "Sign in required.", "gated: method is refused before sign-in");
        }
        check(gcall("records.get", {{"key", "camera"}})["ok"] == false, "gated: other records are refused");
        check(gcall("records.set", {{"key", "stream.sources"}, {"value", json::object()}})["ok"] == false, "gated: records cannot be written");
        check(gcall("records.get", {{"key", "preferences"}})["ok"] == true, "gated: the saved theme is readable for the sign-in page");
        check(gcall("auth.status", json::object())["ok"] == true && gcall("app.version", json::object())["ok"] == true, "gated: status and version are open");
        check(gcall("auth.login", {{"username", "alice"}, {"password", "not the password"}})["ok"] == false && !gated.authenticated(), "gated: a wrong password opens nothing");
        check(gcall("alerts.feed", json::object())["ok"] == false, "gated: still refused after a failed sign-in");
        check(json::parse(gated.handle_trusted(json{{"id", 1}, {"method", "alerts.feed"}, {"params", json::object()}}.dump()))["ok"] == true, "gated: the host's own calls need no session");
        check(gcall("auth.login", {{"username", "alice"}, {"password", "correct horse 7"}})["ok"] == true && gated.authenticated(), "gated: right password signs in");
        check(gcall("alerts.feed", json::object())["ok"] == true && gcall("records.get", {{"key", "camera"}})["ok"] == true, "gated: methods work once signed in");
        check(gcall("auth.logout", json::object())["ok"] == true && !gated.authenticated(), "gated: logout closes the door");
        check(gcall("alerts.feed", json::object())["error"] == "Sign in required.", "gated: refused again after logout");
        check(gcall("auth.login", {{"username", "alice"}, {"password", "correct horse 7"}})["ok"] == true, "gated: can sign in again");
        check(call("alerts.feed", json::object())["ok"] == true, "an ungated bridge (tests, Linux host) is unchanged");

        const json hashed = call("records.list", {{"prefix", ""}});
        check(hashed.dump().find("hash") == std::string::npos, "accounts are not reachable through records.list");
    }
    // audit trail: setup, success, failures, lockout; never the password
    std::ifstream in(dir / "audit.log");
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(all.find("auth.setup") != std::string::npos && all.find("auth.login") != std::string::npos, "audit: setup and sign-in recorded");
    check(all.find("auth.login.failed") != std::string::npos && all.find("auth.lockout") != std::string::npos, "audit: failures and lockout recorded");
    check(all.find("correct horse") == std::string::npos && all.find("bad-bad") == std::string::npos, "audit never holds a password");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures) return 1;
    std::cout << "auth tests passed\n";
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
