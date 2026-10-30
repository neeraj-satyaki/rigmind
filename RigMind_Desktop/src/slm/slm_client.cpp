#include "slm_client.h"

#include <winhttp.h>

#include <stdexcept>

#include "slm/slm_guard.h"

namespace {

// Same system message as scripts/host.py: one sentence and nothing else.
const char* kSystemMessage =
    "You write short plain-language safety summaries for rig supervisors. Reply with one or two short sentences and nothing else.";

std::string post_json(unsigned short port, const std::string& body) {
    HINTERNET session = WinHttpOpen(L"RigMind", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");
    WinHttpSetTimeouts(session, 60000, 60000, 60000, 300000); // requests queue in the server behind other summaries: wait up to 5 minutes for the answer
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
    std::string text, reason;
    bool action_added = false, context_added = false;
    try {
        const nlohmann::json body = {
            {"messages", {{{"role", "system"}, {"content", kSystemMessage}},
                          {{"role", "user"}, {"content", command.value("prompt", std::string())}}}},
            {"max_tokens", 110},
            {"temperature", 0},
        };
        const nlohmann::json reply = nlohmann::json::parse(post_json(port, body.dump()));
        const std::string sentence = rigmind::first_sentences(reply.at("choices").at(0).at("message").at("content").get<std::string>(), 2);
        std::string candidate = sentence;
        reason = rigmind::reject_reason(candidate, command);
        if (reason == "missing camera" || reason == "missing place") {
            candidate = rigmind::with_context(candidate, command); // the model left out the camera or place: add them in front
            context_added = candidate != sentence;
            reason = rigmind::reject_reason(candidate, command);
        }
        if (reason.empty()) text = rigmind::with_action(candidate, command.value("severity", std::string()), &action_added);
    } catch (const std::exception&) {
        reason = "model unavailable";
        text.clear(); // model down or answer unusable: the template text stands
    }
    on_line(nlohmann::json{{"ev", "description"}, {"alertId", command.value("alertId", std::string())},
                           {"text", text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text)},
                           {"source", text.empty() ? "template" : "slm"}, {"reason", reason}, {"action_added", action_added}, {"context_added", context_added}}
                .dump());
}

// Chat about evidence: the prompt carries the matching evidence and alerts; the model may use nothing else. Free text of a few
// sentences (no one-sentence guard, no action line), cleaned of markdown and capped. A failure answers with no text.
const char* kChatSystemMessage =
    "You answer questions about rig-safety evidence. Use only the data in the message. If the data does not answer the question, say so. "
    "Reply in plain sentences, at most four, no lists and no markdown.";

void ask_chat(unsigned short port, SlmClient::OnLine on_line, nlohmann::json command) {
    std::string text;
    try {
        const nlohmann::json body = {
            {"messages", {{{"role", "system"}, {"content", kChatSystemMessage}}, {{"role", "user"}, {"content", command.value("prompt", std::string())}}}},
            {"max_tokens", 220},
            {"temperature", 0},
        };
        const nlohmann::json reply = nlohmann::json::parse(post_json(port, body.dump()));
        std::string raw = reply.at("choices").at(0).at("message").at("content").get<std::string>();
        for (const char* mark : {"**", "##", "__"}) {
            for (std::size_t at; (at = raw.find(mark)) != std::string::npos;) raw.erase(at, 2);
        }
        // collapse whitespace, trim, cap at 900 characters on a character boundary
        std::string clean;
        bool space = false;
        for (unsigned char ch : raw) {
            if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
                space = !clean.empty();
                continue;
            }
            if (space) clean += ' ';
            space = false;
            clean += static_cast<char>(ch);
        }
        if (clean.size() > 900) {
            std::size_t cut = 900;
            while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) --cut;
            clean.resize(cut);
        }
        text = clean;
    } catch (const std::exception&) {
        text.clear();
    }
    on_line(nlohmann::json{{"ev", "chat"}, {"chatId", command.value("chatId", std::string())},
                           {"text", text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text)}}.dump());
}

} // namespace

SlmClient::SlmClient(const std::wstring& server_exe, const std::wstring& model, unsigned short port, OnLine on_line)
    : port_(port), on_line_(std::move(on_line)) {
    const std::wstring folder = server_exe.substr(0, server_exe.find_last_of(L"\\/"));
    std::wstring cmdline = L"\"" + server_exe + L"\" -m \"" + model + L"\" --port " + std::to_wstring(port) + L" -c 2560 -t 4";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // No console window; the GUI parent has no standard handles to pass on.
    if (CreateProcessW(server_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                       folder.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        worker_ = std::thread([this] { work(); });
    }
}

SlmClient::~SlmClient() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (process_) {
        TerminateProcess(process_, 0); // an answer in flight fails at once, so the worker can finish
    }
    if (worker_.joinable()) worker_.join();
    if (process_) CloseHandle(process_);
}

void SlmClient::answer_template(const nlohmann::json& command, const char* reason) const {
    if (command.value("cmd", std::string()) == "chat") { // a chat question that could not be answered: no text
        on_line_(nlohmann::json{{"ev", "chat"}, {"chatId", command.value("chatId", std::string())}, {"text", nullptr}, {"reason", reason}}.dump());
        return;
    }
    on_line_(nlohmann::json{{"ev", "description"}, {"alertId", command.value("alertId", std::string())},
                            {"text", nullptr}, {"source", "template"}, {"reason", reason}}.dump());
}

void SlmClient::describe(const nlohmann::json& command) {
    if (!running()) {
        answer_template(command, "model unavailable");
        return;
    }
    nlohmann::json dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({command, std::chrono::steady_clock::now()});
        if (queue_.size() > kMaxQueue) {
            dropped = queue_.front().command; // the oldest waits longest and is the least useful by now
            queue_.pop_front();
        }
    }
    wake_.notify_one();
    if (!dropped.is_null()) answer_template(dropped, "queue full");
}

// The one summary worker: takes requests in order, never blocks the alarm path.
void SlmClient::work() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (std::chrono::steady_clock::now() - job.queued > kMaxAge) {
            answer_template(job.command, "waited too long");
            continue;
        }
        if (job.command.value("cmd", std::string()) == "chat") ask_chat(port_, on_line_, job.command);
        else ask_model(port_, on_line_, job.command);
    }
}
