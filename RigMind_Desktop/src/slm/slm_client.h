#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

// Runs BitNet's llama-server.exe (slm\ folder next to RigMind.exe) and answers describe requests over HTTP.
// Each answer goes out as one {"ev":"description",...} JSON line through on_line, which the UI window forwards
// to the page. Requests run on detached worker threads, so the UI thread never waits on the model.
class SlmClient {
public:
    using OnLine = std::function<void(const std::string&)>;

    SlmClient(const std::wstring& server_exe, const std::wstring& model, unsigned short port, OnLine on_line);
    ~SlmClient();

    SlmClient(const SlmClient&) = delete;
    SlmClient& operator=(const SlmClient&) = delete;

    bool running() const { return process_ != nullptr; }
    void describe(const nlohmann::json& command);

private:
    HANDLE process_ = nullptr;
    unsigned short port_;
    OnLine on_line_;
};
