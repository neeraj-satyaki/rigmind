#include "mcp/mcp_http.h"

#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace rigmind {

namespace {

constexpr std::size_t kMaxHeader = 64 * 1024;
constexpr std::size_t kMaxBody = 1024 * 1024;
constexpr int kMaxActive = 8;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool constant_time_equal(const std::string& a, const std::string& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < (std::min)(a.size(), b.size()); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

void send_all(SOCKET s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

void respond(SOCKET s, int status, const char* reason, const std::string& body, const std::string& extra = {}) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
        << "\r\nCache-Control: no-store\r\nConnection: close\r\n" << extra << "\r\n" << body;
    send_all(s, out.str());
}

std::string error_body(const std::string& message) { return nlohmann::json{{"error", message}}.dump(); }

} // namespace

std::string load_or_create_mcp_token(const std::filesystem::path& file) {
    {
        std::ifstream in(file);
        std::string token;
        if (in && std::getline(in, token)) {
            token = trim(token);
            if (token.size() >= 32) return token;
        }
    }
    unsigned char bytes[32];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    static const char* kHex = "0123456789abcdef";
    std::string token;
    for (unsigned char b : bytes) {
        token += kHex[b >> 4];
        token += kHex[b & 15];
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return {};
    out << token << "\n";
    return token;
}

bool McpHttp::start(unsigned short port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    for (int attempt = 0; attempt < (port == 0 ? 1 : 10); ++attempt) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port == 0 ? 0 : port + attempt));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // loopback only
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(s, 16) == 0) {
            int len = sizeof(addr);
            getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
            port_ = ntohs(addr.sin_port);
            listener_ = s;
            acceptor_ = std::thread([this] { accept_loop(); });
            return true;
        }
        closesocket(s);
    }
    return false;
}

void McpHttp::stop() {
    if (stopping_.exchange(true)) return;
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
    }
    if (acceptor_.joinable()) acceptor_.join();
    for (int i = 0; i < 100 && active_.load() > 0; ++i) Sleep(50); // let requests in flight finish
}

void McpHttp::accept_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return; // listener closed
        if (active_.load() >= kMaxActive) {
            respond(client, 503, "Service Unavailable", error_body("too many requests"), "Retry-After: 2\r\n");
            closesocket(client);
            continue;
        }
        ++active_;
        std::thread([this, client] {
            serve(client);
            closesocket(client);
            --active_;
        }).detach();
    }
}

void McpHttp::serve(SOCKET client) {
    DWORD timeout = 15000; // a client that stalls mid-request is dropped
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string data;
    std::size_t header_end = std::string::npos;
    char buf[4096];
    while (header_end == std::string::npos && data.size() < kMaxHeader) {
        const int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) return;
        data.append(buf, static_cast<std::size_t>(n));
        header_end = data.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) return respond(client, 431, "Request Header Fields Too Large", error_body("headers too large"));

    std::istringstream head(data.substr(0, header_end));
    std::string request_line;
    std::getline(head, request_line);
    std::istringstream rl(request_line);
    std::string method, path, version;
    rl >> method >> path >> version;
    std::map<std::string, std::string> headers;
    for (std::string line; std::getline(head, line);) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    // Browsers send Origin; this endpoint is for local programs only. A foreign Host is a DNS-rebinding attempt.
    if (headers.count("origin")) return respond(client, 403, "Forbidden", error_body("browser origins are not allowed"));
    const std::string host = lower(headers["host"]);
    const std::string port = std::to_string(port_);
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) return respond(client, 403, "Forbidden", error_body("invalid host"));

    const std::string auth = headers["authorization"];
    const std::string bearer = "Bearer ";
    if (token_.empty() || auth.compare(0, bearer.size(), bearer) != 0 || !constant_time_equal(auth.substr(bearer.size()), token_))
        return respond(client, 401, "Unauthorized", error_body("missing or wrong token (see mcp-token.txt in the RigMind data folder)"),
                       "WWW-Authenticate: Bearer realm=\"rigmind\"\r\n");

    const std::string target = path.substr(0, path.find('?'));
    if (target != "/mcp") return respond(client, 404, "Not Found", error_body("use /mcp"));
    if (method != "POST") return respond(client, 405, "Method Not Allowed", error_body("POST JSON-RPC to /mcp"), "Allow: POST\r\n");

    std::size_t length = 0;
    try {
        length = headers.count("content-length") ? static_cast<std::size_t>(std::stoull(headers["content-length"])) : 0;
    } catch (const std::exception&) {
        return respond(client, 400, "Bad Request", error_body("bad Content-Length"));
    }
    if (length == 0 || length > kMaxBody) return respond(client, 413, "Payload Too Large", error_body("body must be 1 byte to 1 MB"));
    std::string body = data.substr(header_end + 4);
    while (body.size() < length) {
        const int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) return;
        body.append(buf, static_cast<std::size_t>(n));
    }
    body.resize(length);

    nlohmann::json request;
    try {
        request = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        return respond(client, 400, "Bad Request",
                       nlohmann::json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}}.dump());
    }
    const nlohmann::json answer = server_.handle(request);
    if (answer.is_null()) return respond(client, 202, "Accepted", {}); // a notification: nothing to say
    respond(client, 200, "OK", answer.dump());
}

} // namespace rigmind
