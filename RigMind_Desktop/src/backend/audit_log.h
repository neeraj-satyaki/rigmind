#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct sqlite3;

namespace rigmind {

// Audit trail for UI actions and backend actions.
// record() only queues (no I/O on the caller's thread). A worker thread writes queued entries in one
// transaction to the audit_log table and appends them as JSON lines to audit.log next to the database.
class AuditLog {
public:
    // Throws std::runtime_error when the database cannot be opened.
    explicit AuditLog(const std::string& db_path);
    // Drains the queue, then stops the worker and closes the database.
    ~AuditLog();

    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    void record(const std::string& source, const std::string& action, const std::string& target,
                const std::string& detail, std::int64_t timestamp_ms);

private:
    struct Entry {
        std::int64_t timestamp_ms;
        std::string source;
        std::string action;
        std::string target;
        std::string detail;
    };

    void run();
    void write_batch(const std::deque<Entry>& batch);

    static constexpr std::size_t kMaxQueued = 10000; // bounded memory on 4 GB machines

    sqlite3* db_ = nullptr;
    std::string log_path_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Entry> queue_;
    std::uint64_t dropped_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace rigmind
