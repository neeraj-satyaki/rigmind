#include "audit_log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include "sqlite3.h"

namespace rigmind {

namespace {

void exec_or_throw(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "sqlite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

} // namespace

AuditLog::AuditLog(const std::string& db_path)
    : log_path_((std::filesystem::path(db_path).parent_path() / "audit.log").string()) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string message = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open audit database: " + message);
    }
    sqlite3_busy_timeout(db_, 5000); // shares the database with the bridge connection
    exec_or_throw(db_,
                  "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
                  "CREATE TABLE IF NOT EXISTS audit_log ("
                  " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                  " ts_ms INTEGER NOT NULL,"
                  " source TEXT NOT NULL,"
                  " action TEXT NOT NULL,"
                  " target TEXT NOT NULL,"
                  " detail TEXT NOT NULL);");
    worker_ = std::thread([this] { run(); });
}

AuditLog::~AuditLog() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (db_) sqlite3_close(db_);
}

void AuditLog::record(const std::string& source, const std::string& action, const std::string& target,
                      const std::string& detail, std::int64_t timestamp_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueued) {
            ++dropped_;
            return;
        }
        queue_.push_back({timestamp_ms, source, action, target, detail});
    }
    ready_.notify_one();
}

// Worker thread: waits for entries, takes the whole queue, writes it outside the lock.
void AuditLog::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty() || dropped_ > 0; });
        if (queue_.empty() && dropped_ == 0 && stopping_) break;

        std::deque<Entry> batch;
        batch.swap(queue_);
        if (dropped_ > 0) {
            // Record that entries were lost to the bound, so the trail shows the gap.
            batch.push_back({0, "backend", "audit.overflow", "", std::to_string(dropped_)});
            batch.back().timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            dropped_ = 0;
        }
        lock.unlock();
        write_batch(batch);
        lock.lock();
    }
}

void AuditLog::write_batch(const std::deque<Entry>& batch) {
    if (batch.empty()) return;
    try {
        exec_or_throw(db_, "BEGIN");
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "INSERT INTO audit_log (ts_ms, source, action, target, detail) VALUES (?, ?, ?, ?, ?)",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        for (const Entry& e : batch) {
            sqlite3_bind_int64(stmt, 1, e.timestamp_ms);
            sqlite3_bind_text(stmt, 2, e.source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, e.action.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, e.target.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, e.detail.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error(sqlite3_errmsg(db_));
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        exec_or_throw(db_, "COMMIT");

        std::ofstream log(log_path_, std::ios::app);
        for (const Entry& e : batch) {
            log << nlohmann::json{{"ts", e.timestamp_ms}, {"source", e.source}, {"action", e.action},
                                  {"target", e.target}, {"detail", e.detail}}
                       .dump()
                << '\n';
        }
    } catch (const std::exception& e) {
        // Never throw from the worker; a failed batch is reported and the backend keeps running.
        std::cerr << "audit write failed: " << e.what() << '\n';
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

} // namespace rigmind
