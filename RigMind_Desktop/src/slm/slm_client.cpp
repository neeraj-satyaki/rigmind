#include "slm_client.h"

#include <winhttp.h>

#include <stdexcept>
#include <thread>

#include "slm/slm_guard.h"

namespace {

// Same system message as scripts/host.py: one sentence and nothing else.
const char* kSystemMessage =
    "You write short plain-language safety summaries for rig supervisors. Reply with exactly one sentence and nothing else.";

std::string post_json(unsigned short port, const std::string& body) {
    HINTERNET session = WinHttpOpen(L"RigMind", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");
    WinHttpSetTimeouts(session, 60000, 60000, 60000, 60000);
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/v1/chat/completions", nullptr,
                                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                                   : nullptr;
    std::string out;
    if (request &&
        WinHttpAddRequestHeaders(request, L"Content-Type: application/json", static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(body.data()),
                           static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD available = 0;
        do {
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            out.append(chunk.data(), read);
        } while (available > 0);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (out.empty()) throw std::runtime_error("no answer from the model");
    return out;
}

// Worker: asks the model, applies the sentence rules, and sends the description event. Takes copies only.
void ask_model(unsigned short port, SlmClient::OnLine on_line, nlohmann::json command) {
    std::string text;
    try {
        const nlohmann::json body = {
            {"messages", {{{"role", "system"}, {"content", kSystemMessage}},
                          {{"role", "user"}, {"content", command.value("prompt", std::string())}}}},
            {"max_tokens", 80},
            {"temperature", 0},
        };
        const nlohmann::json reply = nlohmann::json::parse(post_json(port, body.dump()));
        const std::string sentence = rigmind::first_sentence(reply.at("choices").at(0).at("message").at("content").get<std::string>());
        if (rigmind::reject_reason(sentence, command).empty()) text = sentence;
    } catch (const std::exception&) {
        text.clear(); // model down or answer unusable: the template text stands
    }
    on_line(nlohmann::json{{"ev", "description"}, {"alertId", command.value("alertId", std::string())},
                           {"text", text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text)},
                           {"source", text.empty() ? "template" : "slm"}}
                .dump());
}

} // namespace

SlmClient::SlmClient(const std::wstring& server_exe, const std::wstring& model, unsigned short port, OnLine on_line)
    : port_(port), on_line_(std::move(on_line)) {
    const std::wstring folder = server_exe.substr(0, server_exe.find_last_of(L"\\/"));
    std::wstring cmdline = L"\"" + server_exe + L"\" -m \"" + model + L"\" --port " + std::to_wstring(port) + L" -c 512 -t 2";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // No console window; the GUI parent has no standard handles to pass on.
    if (CreateProcessW(server_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       folder.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
    }
}

SlmClient::~SlmClient() {
    if (process_) {
        TerminateProcess(process_, 0);
        CloseHandle(process_);
    }
}

void SlmClient::describe(const nlohmann::json& command) {
    if (!running()) {
        on_line_(nlohmann::json{{"ev", "description"}, {"alertId", command.value("alertId", std::string())},
                                {"text", nullptr}, {"source", "template"}}.dump());
        return;
    }
    std::thread(ask_model, port_, on_line_, command).detach();
}
