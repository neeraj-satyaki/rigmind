// RigMind native host: Win32 window + WebView2 hosting the built Rigmind_v2 SPA.
// The page talks to the C++ backend via window.chrome.webview.postMessage (see backend/bridge.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "backend/alarm_recorder.h"
#include "backend/audit_log.h"
#include "backend/bridge.h"
#include "backend/event_store.h"
#include "resource.h"
#include "slm/slm_client.h"
#include "stream/pipeline_process.h"
#include "stream/stream_router.h"

// Posted by the pipeline reader thread; lParam owns a heap std::string holding one JSON line.
constexpr UINT WM_APP_PIPE = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2; // notification-area icon: mouse events
constexpr UINT kTrayMenuOpen = 1, kTrayMenuExit = 2;

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"RigMindMainWindow";
constexpr wchar_t kAppHost[] = L"rigmind.app";
constexpr wchar_t kFootageHost[] = L"footage.rigmind.app";
constexpr wchar_t kStartUrl[] = L"https://rigmind.app/index.html";

HWND g_hwnd = nullptr;
HANDLE g_instance_lock = nullptr;
NOTIFYICONDATAW g_tray{};
bool g_tray_added = false;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<ICoreWebView2Environment> g_env;
std::unique_ptr<rigmind::EventStore> g_store;
std::unique_ptr<rigmind::AuditLog> g_audit;
std::unique_ptr<rigmind::AlarmRecorder> g_recorder;
std::unique_ptr<rigmind::PpeRecorder> g_ppe_recorder;
std::unique_ptr<rigmind::Bridge> g_bridge;
std::unique_ptr<PipelineProcess> g_pipeline;
std::unique_ptr<SlmClient> g_slm;
std::unique_ptr<StreamRouter> g_router;

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::filesystem::path exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path user_data_dir() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    const std::filesystem::path base = local ? std::filesystem::path(local) : exe_dir();
    const auto dir = base / L"RigMind";
    std::filesystem::create_directories(dir);
    return dir;
}

void show_fatal(const wchar_t* message) {
    MessageBoxW(g_hwnd, message, L"RigMind", MB_OK | MB_ICONERROR);
    PostQuitMessage(1);
}

void resize_webview() {
    if (!g_controller) return;
    RECT bounds{};
    GetClientRect(g_hwnd, &bounds);
    g_controller->put_Bounds(bounds);
}

// report.savePdf {name}: prints the open report page to a readable A4 PDF in Documents\RigMind Reports and opens it.
// (window.print() inside the embedded view gives no usable "save as PDF"; WebView2 PrintToPdf renders the same print layout.)
bool handle_report_pdf(const std::string& text) {
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!request.is_object() || request.value("method", std::string()) != "report.savePdf") return false;
    const auto id = request.value("id", 0);
    const auto fail = [id](const std::string& why) {
        g_webview->PostWebMessageAsJson(to_wide(nlohmann::json{{"id", id}, {"ok", false}, {"error", why}}.dump()).c_str());
    };
    ComPtr<ICoreWebView2_7> view7;
    ComPtr<ICoreWebView2Environment6> env6;
    if (!g_webview || FAILED(g_webview.As(&view7)) || !g_env || FAILED(g_env.As(&env6))) {
        if (g_webview) fail("This WebView2 runtime cannot save PDF files. Use Print instead.");
        return true;
    }
    std::string name = request.contains("params") && request["params"].is_object() ? request["params"].value("name", std::string("report")) : "report";
    if (g_audit) g_audit->record("backend", "report.pdf", name.substr(0, 64), "save requested", now_ms());
    std::string clean;
    for (char c : name) clean += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '-';
    if (clean.empty()) clean = "report";
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) || !documents) {
        fail("The Documents folder is not available.");
        return true;
    }
    std::filesystem::path folder = std::filesystem::path(documents) / L"RigMind Reports";
    CoTaskMemFree(documents);
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
    const std::filesystem::path file = folder / (L"RigMind-" + to_wide(clean) + L"-" + stamp + L".pdf");

    ComPtr<ICoreWebView2PrintSettings> settings;
    if (FAILED(env6->CreatePrintSettings(&settings)) || !settings) {
        fail("Print settings could not be created.");
        return true;
    }
    settings->put_Orientation(COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT);
    settings->put_ShouldPrintBackgrounds(TRUE);
    settings->put_ShouldPrintHeaderAndFooter(FALSE);
    const std::wstring path = file.wstring();
    const HRESULT hr = view7->PrintToPdf(
        path.c_str(), settings.Get(),
        Callback<ICoreWebView2PrintToPdfCompletedHandler>([id, path](HRESULT result, BOOL ok) -> HRESULT {
            if (SUCCEEDED(result) && ok) {
                g_webview->PostWebMessageAsJson(to_wide(nlohmann::json{{"id", id}, {"ok", true}, {"result", {{"path", to_utf8(path)}}}}.dump()).c_str());
                ShellExecuteW(g_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // shows it in the default PDF viewer
            } else {
                g_webview->PostWebMessageAsJson(to_wide(nlohmann::json{{"id", id}, {"ok", false}, {"error", "The PDF could not be written."}}.dump()).c_str());
            }
            return S_OK;
        }).Get());
    if (FAILED(hr)) fail("The PDF could not be started.");
    return true;
}

void on_web_message(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> message(raw, CoTaskMemFree);

    if (handle_report_pdf(to_utf8(message.get()))) return;
    const std::string reply = g_bridge->handle(to_utf8(message.get()));
    g_webview->PostWebMessageAsJson(to_wide(reply).c_str());
}

void configure_webview() {
    ComPtr<ICoreWebView2Settings> settings;
    g_webview->get_Settings(&settings);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    // Desktop app, not a browser: no F5/Ctrl+R (a reload of a client route has no file behind it) and no browser menu.
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);

    // The shell is served as /index.html, which is not a SPA route (it renders Not Found). Start the router at "/".
    g_webview->AddScriptToExecuteOnDocumentCreated(
        L"if (location.pathname === '/index.html') history.replaceState(null, '', '/' + location.search + location.hash);",
        nullptr);

    // Sample data is an install-time choice: the installer writes <exe>\demo-data.flag when it was ticked.
    {
        std::error_code ignored;
        const bool demo = std::filesystem::exists(exe_dir() / L"demo-data.flag", ignored);
        g_webview->AddScriptToExecuteOnDocumentCreated(
            demo ? L"window.__RIGMIND_CONFIG__ = { demoData: true };" : L"window.__RIGMIND_CONFIG__ = { demoData: false };", nullptr);
    }

    // Serve the bundled SPA from <exe>\web as https://rigmind.app, so routing and sql.js WASM work unchanged.
    const std::wstring web_dir = (exe_dir() / L"web").wstring();
    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(g_webview.As(&webview3))) {
        webview3->SetVirtualHostNameToFolderMapping(kAppHost, web_dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        // Saved alarm clips (<data>\footage\<cam>\<stamp>\seg*.mp4) play in the Evidence library from https://footage.rigmind.app/.
        const auto footage_dir = user_data_dir() / L"footage";
        std::error_code ignored;
        std::filesystem::create_directories(footage_dir, ignored);
        webview3->SetVirtualHostNameToFolderMapping(kFootageHost, footage_dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }

    EventRegistrationToken token{};
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                on_web_message(args);
                return S_OK;
            })
            .Get(),
        &token);

    resize_webview();
    g_webview->Navigate(kStartUrl);
}

void create_webview() {
    const auto data_dir = user_data_dir() / L"WebView2";
    // Alert tones fire from the backend, not from a click: allow audio without a user gesture.
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required");
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_dir.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT env_result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(env_result) || !env) {
                    show_fatal(L"Microsoft Edge WebView2 Runtime is missing or failed to start.");
                    return S_OK;
                }
                g_env = env;
                return env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT ctl_result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(ctl_result) || !controller) {
                                show_fatal(L"Could not create the RigMind view.");
                                return S_OK;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            configure_webview();
                            return S_OK;
                        })
                        .Get());
            })
            .Get());
    if (FAILED(hr)) throw std::runtime_error("WebView2 environment could not be created");
}

// App activity for the audit trail: camera state changes, pipeline errors, evidence clips, peer connections and the source of
// every plain-language summary. Detections and other high-rate lines are skipped before any parsing.
void audit_activity(const std::string& line) {
    if (!g_audit) return;
    static const char* kKinds[] = {"\"ev\":\"status\"", "\"ev\":\"error\"", "\"ev\":\"clip\"", "\"ev\":\"evidence\"",
                                   "\"ev\":\"peer\"", "\"ev\":\"description\""};
    bool wanted = false;
    for (const char* k : kKinds) wanted = wanted || line.find(k) != std::string::npos;
    if (!wanted || line.size() > 4096) return;
    try {
        const auto e = nlohmann::json::parse(line);
        const std::string kind = e.value("ev", std::string());
        const std::string cam = e.value("cam", std::string());
        static std::mutex mutex;
        static std::map<std::string, std::string> last;
        std::lock_guard<std::mutex> lock(mutex);
        if (kind == "status") {
            const std::string state = e.value("state", std::string());
            if (last[cam] == state) return; // only changes
            last[cam] = state;
            g_audit->record("backend", "camera.status", cam, state + (e.value("reason", std::string()).empty() ? "" : " | " + e.value("reason", std::string())), now_ms());
        } else if (kind == "error") {
            g_audit->record("backend", "app.error", cam, e.value("message", std::string()).substr(0, 400), now_ms());
        } else if (kind == "clip") {
            g_audit->record("backend", "evidence.saved", cam, e.value("stamp", std::string()) + " | " + std::to_string(e.value("count", 0)) + " files", now_ms());
        } else if (kind == "evidence") {
            g_audit->record("backend", "evidence.started", cam, e.value("stamp", std::string()), now_ms());
        } else if (kind == "peer") {
            const int connection = e.value("connection", -1);
            const std::string key = "peer:" + cam;
            const std::string state = std::to_string(connection);
            if (last[key] == state) return;
            last[key] = state;
            g_audit->record("backend", "viewer.connection", cam, "state " + state, now_ms());
        } else if (kind == "description") {
            g_audit->record("backend", "summary.ready", e.value("alertId", std::string()), e.value("source", std::string("template")) + (e.value("action_added", false) ? " | action added" : "") + (e.value("reason", std::string()).empty() ? "" : " | rejected: " + e.value("reason", std::string())), now_ms());
        }
    } catch (const std::exception&) {
    }
}

// Every host line (pipeline answers, clip reports, SLM descriptions) goes to the page through the window.
// Alarm lines are first recorded by the host (alarm_events, audit trail, the episode's single alert row), so the
// history is complete even when the page is closed. The enriched line (alertId, phase, escalation) goes on to the page.
void post_line(const std::string& line) {
    // Scene lines (objects, outlines, centroid paths) feed the summary prompt only; the page never needs them.
    if (line.find("\"ev\":\"scene\"") != std::string::npos && line.find("\"objects\"") != std::string::npos) {
        try {
            if (g_router) g_router->note_scene(nlohmann::json::parse(line));
        } catch (const std::exception&) {
        }
        return;
    }
    audit_activity(line);
    std::string forwarded;
    if (g_recorder && line.find("\"alarm\"") != std::string::npos) {
        try {
            const auto event = nlohmann::json::parse(line);
            if (event.is_object() && event.value("ev", std::string()) == "alarm")
                forwarded = g_recorder->handle(event, now_ms()).dump();
        } catch (const std::exception&) {
            forwarded.clear(); // not valid alarm JSON: forward the original line untouched
        }
    }
    // PPE compliance lines: one Advisory alert row per episode, audit rows 'ppe.transition'.
    if (g_ppe_recorder && line.find("\"ppe\"") != std::string::npos) {
        try {
            const auto event = nlohmann::json::parse(line);
            if (event.is_object() && event.value("ev", std::string()) == "ppe")
                forwarded = g_ppe_recorder->handle(event, now_ms()).dump();
        } catch (const std::exception&) {
            forwarded.clear();
        }
    }
    auto* message = new std::string(forwarded.empty() ? line : forwarded);
    if (!PostMessageW(g_hwnd, WM_APP_PIPE, 0, reinterpret_cast<LPARAM>(message))) delete message;
}

// {"ev":"clip","cam","stamp","count"} from the pipeline: the clip folder <data>ootage\<cam>\<stamp> is complete.
void record_clip_line(const std::string& line) {
    try {
        const auto event = nlohmann::json::parse(line);
        if (!event.is_object() || event.value("ev", std::string()) != "clip") return;
        const nlohmann::json request = {{"id", 0},
                                        {"method", "footage.record"},
                                        {"params", {{"cam", event.at("cam")}, {"stamp", event.at("stamp")}, {"count", event.at("count")}}}};
        g_bridge->handle(request.dump());
    } catch (const std::exception&) {
        // Malformed clip line: the page still receives it; no footage row.
    }
}

// An installed RigMind ships its own GStreamer under <exe>\gst. Without that folder (a development machine) the
// system GStreamer on PATH is used. The plugin registry is kept in the user data folder: Program Files is read-only.
void configure_gstreamer_runtime() {
    const auto gst = exe_dir() / L"gst";
    if (!std::filesystem::exists(gst)) return;
    const wchar_t* old_path = _wgetenv(L"PATH");
    SetEnvironmentVariableW(L"PATH", ((gst / L"bin").wstring() + L";" + (old_path ? old_path : L"")).c_str());
    const std::wstring plugins = (gst / L"lib" / L"gstreamer-1.0").wstring();
    SetEnvironmentVariableW(L"GST_PLUGIN_PATH_1_0", plugins.c_str());
    SetEnvironmentVariableW(L"GST_PLUGIN_SYSTEM_PATH_1_0", plugins.c_str());
    const auto scanner = gst / L"libexec" / L"gstreamer-1.0" / L"gst-plugin-scanner.exe";
    if (std::filesystem::exists(scanner)) SetEnvironmentVariableW(L"GST_PLUGIN_SCANNER_1_0", scanner.wstring().c_str());
    SetEnvironmentVariableW(L"GST_REGISTRY_1_0", (user_data_dir() / L"gst-registry.bin").wstring().c_str());
}

// Cameras saved by the Camera page are opened at start, so recording and alarms run without the Live page open.
void open_saved_cameras() {
    if (!g_pipeline) return;
    const auto saved = g_store->get_record("stream.sources");
    if (!saved) return;
    try {
        for (const auto& [cam, uri] : nlohmann::json::parse(*saved).items()) {
            if (uri.is_string()) g_pipeline->send({{"cmd", "open"}, {"cam", cam}, {"uri", uri.get<std::string>()}});
        }
    } catch (const std::exception&) {
        // A malformed record is left for the Camera page to overwrite.
    }
}

void init_backend() {
    const auto db_path = user_data_dir() / L"rigmind.db";
    g_store = std::make_unique<rigmind::EventStore>(to_utf8(db_path.wstring()));
    g_audit = std::make_unique<rigmind::AuditLog>(to_utf8(db_path.wstring()));
    g_store->set_footage_root(to_utf8((user_data_dir() / L"footage").wstring()));
    g_store->prune_footage(now_ms()); // 7-day clearance at every start
    g_audit->record("backend", "app.start", "", "windows-host", now_ms());
    g_recorder = std::make_unique<rigmind::AlarmRecorder>(*g_store, g_audit.get());
    g_ppe_recorder = std::make_unique<rigmind::PpeRecorder>(*g_store, g_audit.get(), g_recorder->boot());

    // Pipeline, SLM and router are optional: the app runs without them and reports them as unavailable.
    try {
        configure_gstreamer_runtime();
        g_pipeline = std::make_unique<PipelineProcess>((exe_dir() / L"rigmind-pipeline.exe").wstring(),
                                                       user_data_dir().wstring(),
                                                       (exe_dir() / L"librigmindews.dll").wstring(),
                                                       (exe_dir() / L"models").wstring(), post_line);
        open_saved_cameras();
        g_audit->record("backend", "pipeline.start", "", "rigmind-pipeline", now_ms());
    } catch (const std::exception&) {
        g_pipeline.reset();
    }
    // BitNet needs the server and the model (the installer downloads the model); without either, summaries use the template text.
    const auto slm_exe = exe_dir() / L"slm" / L"llama-server.exe";
    const auto slm_model = exe_dir() / L"slm" / L"ggml-model-i2_s.gguf";
    if (std::filesystem::exists(slm_exe) && std::filesystem::exists(slm_model)) {
        g_slm = std::make_unique<SlmClient>(slm_exe.wstring(), slm_model.wstring(), 8791, post_line);
        g_audit->record("backend", "slm.start", "bitnet", g_slm->running() ? "running" : "failed to start (template text)", now_ms());
        if (!g_slm->running()) g_slm.reset();
    } else {
        g_audit->record("backend", "slm.start", "bitnet", "not installed (template text)", now_ms());
    }
    g_router = std::make_unique<StreamRouter>(g_pipeline.get(), g_slm.get(), post_line);
    g_bridge = std::make_unique<rigmind::Bridge>(*g_store, g_router.get(), g_audit.get());
}

// ---- One session per user data folder ----------------------------------------------------------------------------------
// A second start brings the running window to the front and exits. The lock is keyed by the data folder, so a deliberately
// separate data folder (tests, another Windows user) is a separate session.
std::wstring instance_key() {
    std::wstring dir = user_data_dir().wstring();
    for (wchar_t& c : dir) c = static_cast<wchar_t>(towlower(c));
    return dir;
}

constexpr wchar_t kInstanceProp[] = L"RigMindInstance";

std::uintptr_t key_stamp(const std::wstring& key) {
    return static_cast<std::uintptr_t>(std::hash<std::wstring>{}(key)) | 1u;
}

bool acquire_single_instance(const std::wstring& key) {
    wchar_t name[48];
    swprintf(name, 48, L"Local\\RigMind-%016llx", static_cast<unsigned long long>(key_stamp(key)));
    g_instance_lock = CreateMutexW(nullptr, TRUE, name);
    return g_instance_lock && GetLastError() != ERROR_ALREADY_EXISTS;
}

void focus_running_instance(const std::wstring& key) {
    struct Search {
        std::uintptr_t stamp;
        HWND found;
    } search{key_stamp(key), nullptr};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            wchar_t name[64];
            if (!GetClassNameW(window, name, 64) || wcscmp(name, kWindowClass) != 0) return TRUE;
            if (reinterpret_cast<std::uintptr_t>(GetPropW(window, kInstanceProp)) == s->stamp) {
                s->found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.found) return;
    if (IsIconic(search.found)) ShowWindow(search.found, SW_RESTORE);
    ShowWindow(search.found, SW_SHOW);
    SetForegroundWindow(search.found);
    FLASHWINFO flash{sizeof(flash), search.found, FLASHW_ALL, 2, 0};
    FlashWindowEx(&flash);
}

// ---- Notification area (system tray) ------------------------------------------------------------------------------------
void tray_add(HINSTANCE instance) {
    g_tray = {};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_APP_TRAY;
    g_tray.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(g_tray.szTip, L"RigMind - monitoring");
    g_tray_added = Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;
}

void tray_remove() {
    if (g_tray_added) Shell_NotifyIconW(NIM_DELETE, &g_tray);
    g_tray_added = false;
}

// Windows notification (balloon / toast) from the tray icon. Level: 0 information, 1 warning, 2 critical.
void tray_notify(const std::wstring& title, const std::wstring& text, int level, bool quiet) {
    if (!g_tray_added) return;
    NOTIFYICONDATAW n = g_tray;
    n.uFlags = NIF_INFO | NIF_TIP;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = (level >= 2 ? NIIF_ERROR : level == 1 ? NIIF_WARNING : NIIF_INFO) | (quiet ? NIIF_NOSOUND : 0);
    wcsncpy_s(n.szTip, (L"RigMind - " + title).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void restore_window() {
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

// Orange and Red alarms and PPE advisories raise a Windows notification, even when RigMind is minimised or behind other windows.
void notify_from_line(const std::string& line) {
    const bool alarm = line.find("\"alarm\"") != std::string::npos, ppe = line.find("\"ppe\"") != std::string::npos;
    if (!alarm && !ppe) return;
    try {
        const auto event = nlohmann::json::parse(line);
        if (!event.is_object() || !event.value("changed", false)) return; // repeats change nothing
        const std::string kind = event.value("ev", std::string());
        const std::string cam = event.value("cam", std::string());
        const std::string place = event.value("place", std::string());
        std::string condition = event.value("condition", std::string());
        if (kind == "alarm") {
            const std::string state = event.value("state", std::string());
            if (state != "Orange" && state != "Red") return; // Yellow only says the app is monitoring
            if (condition.empty()) condition = "Safety condition detected";
            const std::wstring title = to_wide(state + (state == "Red" ? " alarm" : " warning") + (cam.empty() ? "" : " - " + cam));
            tray_notify(title, to_wide(condition + (place.empty() ? "" : " - " + place)), state == "Red" ? 2 : 1, false);
            if (g_audit) g_audit->record("backend", "notification.tray", cam, state + " | " + condition, now_ms());
        } else if (kind == "ppe") {
            if (event.value("phase", std::string()) != "Open") return;
            if (condition.empty()) condition = "PPE not detected";
            tray_notify(to_wide("Advisory" + (cam.empty() ? std::string() : " - " + cam)), to_wide(condition), 0, true);
            if (g_audit) g_audit->record("backend", "notification.tray", cam, "Advisory | " + condition, now_ms());
        }
    } catch (const std::exception&) {
        // Not valid JSON: nothing to announce.
    }
}

void tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayMenuOpen, L"Open RigMind");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit RigMind");
    POINT at{};
    GetCursorPos(&at);
    SetForegroundWindow(g_hwnd); // lets the menu close when the user clicks elsewhere
    const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    if (chosen == kTrayMenuOpen) restore_window();
    if (chosen == kTrayMenuExit) DestroyWindow(g_hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        resize_webview();
        return 0;
    case WM_APP_PIPE: {
        std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lparam));
        // A finished alarm clip becomes a footage row. This runs on the UI thread, where the bridge may be called.
        if (g_bridge && line->find("\"clip\"") != std::string::npos) record_clip_line(*line);
        notify_from_line(*line);
        if (g_webview) g_webview->PostWebMessageAsJson(to_wide(*line).c_str());
        return 0;
    }
    case WM_APP_TRAY:
        if (lparam == WM_LBUTTONDBLCLK || lparam == WM_LBUTTONUP || lparam == NIN_BALLOONUSERCLICK) restore_window();
        else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) tray_menu();
        return 0;
    case WM_DESTROY:
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Only one RigMind session per user data folder: a second start shows the running one.
    const std::wstring key = instance_key();
    if (!acquire_single_instance(key)) {
        focus_running_instance(key);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (wide form; UNICODE is not defined)
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWindowClass, L"RigMind", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;
    SetPropW(g_hwnd, kInstanceProp, reinterpret_cast<HANDLE>(key_stamp(key)));
    tray_add(instance);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    try {
        init_backend();
        create_webview();
    } catch (const std::exception& e) {
        const std::string text = std::string("RigMind could not start: ") + e.what();
        MessageBoxW(g_hwnd, to_wide(text).c_str(), L"RigMind", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_webview = nullptr;
    g_controller = nullptr;
    g_env = nullptr;
    if (g_audit) g_audit->record("backend", "app.stop", "", "windows-host", now_ms());
    g_bridge.reset();
    g_router.reset();
    g_slm.reset();
    g_pipeline.reset();
    g_ppe_recorder.reset();
    g_recorder.reset(); // after the pipeline reader thread has stopped
    g_audit.reset(); // drains the audit queue before the database closes
    g_store.reset();
    tray_remove();
    if (g_instance_lock) CloseHandle(g_instance_lock);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
