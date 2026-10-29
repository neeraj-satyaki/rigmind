#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "backend/bridge.h"

// Owns the rigmind-pipeline child process. Commands go to its stdin as JSON lines;
// every stdout line is passed to on_line on a reader thread.
class PipelineProcess : public rigmind::StreamSink {
public:
    using OnLine = std::function<void(const std::string&)>;

    // Throws std::runtime_error when the child cannot be started.
    PipelineProcess(const std::wstring& exe_path, OnLine on_line);
    ~PipelineProcess() override;

    PipelineProcess(const PipelineProcess&) = delete;
    PipelineProcess& operator=(const PipelineProcess&) = delete;

    void send(const nlohmann::json& command) override;

private:
    void read_loop();

    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    OnLine on_line_;
    std::mutex write_mutex_;
    std::thread reader_;
};
