#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

// Runs BitNet's llama-server.exe (slm\ folder next to RigMind.exe) and answers describe requests over HTTP.
// Each answer goes out as one {"ev":"description",...} JSON line through on_line, which the UI window forwards
// to the page. Summaries run on one dedicated worker thread (the server has a single slot, so requests are answered one after
// another from a bounded queue); alarm recording, toasts, tray notices and audit rows run on other threads and never wait on it.
class SlmClient {
public:
    using OnLine = std::function<void(const std::string&)>;

    SlmClient(const std::wstring& server_exe, const std::wstring& model, unsigned short port, OnLine on_line);
    ~SlmClient();

    SlmClient(const SlmClient&) = delete;
    SlmClient& operator=(const SlmClient&) = delete;

    // The server process was started and has not exited.
    bool running() const { return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }
    void describe(const nlohmann::json& command);

private:
    struct Job {
        nlohmann::json command;
        std::chrono::steady_clock::time_point queued;
    };
    void work();
    void answer_template(const nlohmann::json& command, const char* reason) const;

    static constexpr std::size_t kMaxQueue = 6;                        // older requests beyond this get the template text
    static constexpr std::chrono::seconds kMaxAge{240};                // a request that waited this long is not worth answering

    HANDLE process_ = nullptr;
    unsigned short port_;
    OnLine on_line_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    bool stop_ = false;
    std::thread worker_;
};
