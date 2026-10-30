#include "auth_service.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "password_hash.h"

namespace rigmind {

namespace {

constexpr int kFreeAttempts = 5;
constexpr std::int64_t kBaseLockMs = 30'000;
constexpr std::int64_t kMaxLockMs = 15 * 60'000;
constexpr std::size_t kMinPassword = 10, kMaxPassword = 128;

const char* kGeneric = "Wrong username or password.";

} // namespace

std::string AuthService::normalise_username(const std::string& raw) {
    std::size_t b = raw.find_first_not_of(" \t\r\n"), e = raw.find_last_not_of(" \t\r\n");
    std::string name = b == std::string::npos ? std::string() : raw.substr(b, e - b + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.size() < 3 || name.size() > 64) throw std::runtime_error("The username must be 3 to 64 characters.");
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '@' || c == '-'))
            throw std::runtime_error("The username may use letters, digits and . _ @ - only.");
    return name;
}

std::string AuthService::password_problem(const std::string& username, const std::string& password) {
    if (password.size() < kMinPassword) return "The password must be at least 10 characters.";
    if (password.size() > kMaxPassword) return "The password must be at most 128 characters.";
    std::string lower = password;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find(username) != std::string::npos) return "The password must not contain the username.";
    bool letter = false, other = false;
    for (unsigned char c : password) (std::isalpha(c) ? letter : other) = true;
    if (!letter || !other) return "Use letters and at least one digit or symbol in the password.";
    return {};
}

void AuthService::audit(const std::string& action, const std::string& target, const std::string& detail, std::int64_t now_ms) const {
    if (audit_) audit_->record("backend", action, target, detail, now_ms);
}

nlohmann::json AuthService::status() const { return {{"hasUsers", store_.has_users()}}; }

nlohmann::json AuthService::setup(const std::string& raw_username, const std::string& password, std::int64_t now_ms) {
    if (store_.has_users()) throw std::runtime_error("An account already exists. Sign in with it.");
    const std::string username = normalise_username(raw_username);
    if (const std::string problem = password_problem(username, password); !problem.empty()) throw std::runtime_error(problem);
    const Bytes salt = random_bytes(16);
    EventStore::UserRow user;
    user.username = username;
    user.display_name = username;
    user.salt_hex = to_hex(salt);
    user.iterations = options_.iterations;
    user.hash_hex = to_hex(pbkdf2_sha256(password, salt, options_.iterations, 32));
    user.role = "Administrator";
    user.created_ms = now_ms;
    store_.create_user(user);
    store_.touch_login(username, now_ms);
    audit("auth.setup", username, "first account created (Administrator)", now_ms);
    return {{"username", username}, {"displayName", user.display_name}, {"role", user.role}};
}

nlohmann::json AuthService::login(const std::string& raw_username, const std::string& password, std::int64_t now_ms) {
    std::string username;
    try {
        username = normalise_username(raw_username);
    } catch (const std::exception&) {
        throw std::runtime_error(kGeneric); // an impossible username is simply wrong
    }
    Failures& fail = failures_[username];
    if (fail.locked_until_ms > now_ms) {
        const std::int64_t wait_s = (fail.locked_until_ms - now_ms + 999) / 1000;
        audit("auth.login.failed", username, "locked, " + std::to_string(wait_s) + " s left", now_ms);
        throw std::runtime_error("Too many attempts. Try again in " + std::to_string(wait_s) + " seconds.");
    }
    const auto user = store_.get_user(username);
    // Same work whether or not the account exists, so timing does not reveal which usernames are real.
    const Bytes salt = user ? from_hex(user->salt_hex) : Bytes(16, 0);
    const std::uint32_t rounds = user ? user->iterations : options_.iterations;
    const Bytes expected = user ? from_hex(user->hash_hex) : Bytes(32, 0);
    const Bytes got = pbkdf2_sha256(password, salt, rounds, 32);
    if (!user || !constant_time_equal(got, expected)) {
        ++fail.count;
        std::string detail = "wrong credentials (attempt " + std::to_string(fail.count) + ")";
        if (fail.count >= kFreeAttempts) {
            std::int64_t lock = kBaseLockMs << std::min(fail.count - kFreeAttempts, 5);
            lock = std::min(lock, kMaxLockMs);
            fail.locked_until_ms = now_ms + lock;
            audit("auth.lockout", username, "locked for " + std::to_string(lock / 1000) + " s", now_ms);
            detail += ", locked";
        }
        audit("auth.login.failed", username, detail, now_ms);
        throw std::runtime_error(kGeneric);
    }
    fail = Failures{};
    store_.touch_login(username, now_ms);
    audit("auth.login", username, "signed in", now_ms);
    return {{"username", user->username}, {"displayName", user->display_name}, {"role", user->role}};
}

} // namespace rigmind
