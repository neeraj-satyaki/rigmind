// Local sign-in for the RigMind desktop app: the first start creates the first account (administrator); later starts ask for
// that username and password. Passwords are stored only as PBKDF2-HMAC-SHA256 hashes with a random salt per account.
//
//   status()          {hasUsers}
//   setup(u, p)       creates the first account when none exists; refused afterwards
//   login(u, p)       verifies; a wrong username and a wrong password give the same answer and take the same time
//   Lockout: 5 failed attempts for one username lock it for 30 s, doubling with each further failure up to 15 minutes
//   (counted in memory per username, existing or not, so an attacker learns nothing about which names exist).
//
// Every outcome is written to the audit log (auth.setup, auth.login, auth.login.failed, auth.lockout); a password is never logged.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "audit_log.h"
#include "event_store.h"

namespace rigmind {

class AuthService {
public:
    struct Options {
        std::uint32_t iterations = 600000; // PBKDF2 rounds for new accounts (tests use a small number)
    };
    AuthService(EventStore& store, AuditLog* audit, Options options = {}) : store_(store), audit_(audit), options_(options) {}

    // Rounds for accounts created from now on (the bridge CLI test mode only; the app always uses the default).
    void set_iterations(std::uint32_t rounds) { options_.iterations = rounds; }

    nlohmann::json status() const;
    // Throws std::runtime_error with a user-readable message (policy, existing account, lockout, wrong credentials).
    nlohmann::json setup(const std::string& username, const std::string& password, std::int64_t now_ms);
    nlohmann::json login(const std::string& username, const std::string& password, std::int64_t now_ms);

    static std::string normalise_username(const std::string& raw); // trims and lower-cases; throws when not 3-64 of [a-z0-9._@-]
    static std::string password_problem(const std::string& username, const std::string& password); // empty when acceptable

private:
    struct Failures {
        int count = 0;
        std::int64_t locked_until_ms = 0;
    };
    void audit(const std::string& action, const std::string& target, const std::string& detail, std::int64_t now_ms) const;

    EventStore& store_;
    AuditLog* audit_;
    Options options_;
    std::map<std::string, Failures> failures_;
};

} // namespace rigmind
