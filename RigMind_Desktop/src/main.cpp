// RigMind native host: Win32 window + WebView2 hosting the built Rigmind_v2 SPA.
// The page talks to the C++ backend via window.chrome.webview.postMessage (see backend/bridge.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "backend/audit_log.h"
#include "backend/bridge.h"
#include "backend/event_store.h"
#include "resource.h"
#include "slm/slm_client.h"
#include "stream/pipeline_process.h"
#include "stream/stream_router.h"

// Posted by the pipeline reader thread; lParam owns a heap std::string holding one JSON line.
constexpr UINT WM_APP_PIPE = WM_APP + 1;

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"RigMindMainWindow";
constexpr wchar_t kAppHost[] = L"rigmind.app";
constexpr wchar_t kStartUrl[] = L"https://rigmind.app/index.html";

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
std::unique_ptr<rigmind::EventStore> g_store;
std::unique_ptr<rigmind::AuditLog> g_audit;
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

void on_web_message(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> message(raw, CoTaskMemFree);

    const std::string reply = g_bridge->handle(to_utf8(message.get()));
    g_webview->PostWebMessageAsJson(to_wide(reply).c_str());
}

void configure_webview() {
    ComPtr<ICoreWebView2Settings> settings;
    g_webview->get_Settings(&settings);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);

    // Serve the bundled SPA from <exe>\web as https://rigmind.app, so routing and sql.js WASM work unchanged.
    const std::wstring web_dir = (exe_dir() / L"web").wstring();
    g_webview->SetVirtualHostNameToFolderMapping(kAppHost, web_dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);

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

// Every host line (pipeline answers, clip reports, SLM descriptions) goes to the page through the window.
void post_line(const std::string& line) {
    auto* message = new std::string(line);
    if (!PostMessageW(g_hwnd, WM_APP_PIPE, 0, reinterpret_cast<LPARAM>(message))) delete message;
}

void init_backend() {
    const auto db_path = user_data_dir() / L"rigmind.db";
    g_store = std::make_unique<rigmind::EventStore>(to_utf8(db_path.wstring()));
    g_audit = std::make_unique<rigmind::AuditLog>(to_utf8(db_path.wstring()));
    g_audit->record("backend", "app.start", "", "windows-host", now_ms());

    // Pipeline, SLM and router are optional: the app runs without them and reports them as unavailable.
    try {
        g_pipeline = std::make_unique<PipelineProcess>((exe_dir() / L"rigmind-pipeline.exe").wstring(), post_line);
    } catch (const std::exception&) {
        g_pipeline.reset();
    }
    g_slm = std::make_unique<SlmClient>((exe_dir() / L"slm" / L"llama-server.exe").wstring(),
                                        (exe_dir() / L"slm" / L"ggml-model-i2_s.gguf").wstring(), 8791, post_line);
    if (!g_slm->running()) g_slm.reset();
    g_router = std::make_unique<StreamRouter>(g_pipeline.get(), g_slm.get(), post_line);
    g_bridge = std::make_unique<rigmind::Bridge>(*g_store, g_router.get(), g_audit.get());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        resize_webview();
        return 0;
    case WM_APP_PIPE: {
        std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lparam));
        if (g_webview) g_webview->PostWebMessageAsJson(to_wide(*line).c_str());
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWindowClass, L"RigMind", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;
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
    if (g_audit) g_audit->record("backend", "app.stop", "", "windows-host", now_ms());
    g_bridge.reset();
    g_router.reset();
    g_slm.reset();
    g_pipeline.reset();
    g_audit.reset(); // drains the audit queue before the database closes
    g_store.reset();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
