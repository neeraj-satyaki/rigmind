#include "pipeline_process.h"

#include <stdexcept>

PipelineProcess::PipelineProcess(const std::wstring& exe_path, OnLine on_line) : on_line_(std::move(on_line)) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

    HANDLE child_stdin = nullptr;
    HANDLE child_stdout = nullptr;
    if (!CreatePipe(&child_stdin, &stdin_write_, &sa, 0)) throw std::runtime_error("CreatePipe (stdin) failed");
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdout_read_, &child_stdout, &sa, 0)) {
        CloseHandle(child_stdin);
        throw std::runtime_error("CreatePipe (stdout) failed");
    }
    SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

    // stderr goes to NUL so diagnostics cannot corrupt the JSON line protocol on stdout.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin;
    si.hStdOutput = child_stdout;
    si.hStdError = nul;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + exe_path + L"\"";
    const BOOL started = CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(child_stdin);
    CloseHandle(child_stdout);
    if (nul) CloseHandle(nul);

    if (!started) {
        CloseHandle(stdin_write_);
        CloseHandle(stdout_read_);
        stdin_write_ = stdout_read_ = nullptr;
        throw std::runtime_error("cannot start rigmind-pipeline.exe");
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    reader_ = std::thread([this] { read_loop(); });
}

PipelineProcess::~PipelineProcess() {
    if (stdin_write_) CloseHandle(stdin_write_); // child reads EOF and exits
    if (process_) {
        if (WaitForSingleObject(process_, 2000) != WAIT_OBJECT_0) TerminateProcess(process_, 1);
        CloseHandle(process_);
    }
    if (stdout_read_) CloseHandle(stdout_read_); // unblocks ReadFile in the reader thread
    if (reader_.joinable()) reader_.join();
}

void PipelineProcess::send(const nlohmann::json& command) {
    const std::string line = command.dump() + "\n";
    std::lock_guard<std::mutex> lock(write_mutex_);
    DWORD written = 0;
    WriteFile(stdin_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void PipelineProcess::read_loop() {
    std::string pending;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(stdout_read_, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        pending.append(buffer, got);
        std::string::size_type newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            on_line_(pending.substr(0, newline));
            pending.erase(0, newline + 1);
        }
    }
    // Child exited or pipe broke: tell the page so it can drop the session.
    on_line_(R"({"ev":"error","message":"pipeline stopped"})");
}
