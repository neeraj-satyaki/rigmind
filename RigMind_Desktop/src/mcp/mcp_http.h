// Local HTTP transport of the RigMind MCP module (MCP "Streamable HTTP", JSON responses only): POST /mcp on 127.0.0.1.
//
// Safety: bound to the loopback address only; every request needs "Authorization: Bearer <token>" (the token lives in
// <data>\mcp-token.txt, created on first start); a request with an Origin header (a web page) or a Host that is not
// 127.0.0.1/localhost with this port (DNS rebinding) is refused; bodies are capped at 1 MB; at most 8 requests run at once.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "mcp/mcp_server.h"

namespace rigmind {

// Reads the token file, or creates it (32 random bytes as hex, from the system generator). Empty string when it cannot be made.
std::string load_or_create_mcp_token(const std::filesystem::path& file);

class McpHttp {
public:
    // port 0 = pick a free port (tests); otherwise `port` is tried, then the next 9.
    McpHttp(McpServer& server, std::string token) : server_(server), token_(std::move(token)) {}
    ~McpHttp() { stop(); }

    McpHttp(const McpHttp&) = delete;
    McpHttp& operator=(const McpHttp&) = delete;

    bool start(unsigned short port);
    void stop();
    unsigned short port() const { return port_; }

private:
    void accept_loop();
    void serve(SOCKET client);

    McpServer& server_;
    std::string token_;
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_{0};
    std::thread acceptor_;
};

} // namespace rigmind
