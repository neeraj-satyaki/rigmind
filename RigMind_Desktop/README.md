# RigMind Desktop (native C++)

Win32 host + WebView2 hosting the built `Rigmind_v2` SPA, with a C++ backend
(EWS alarm state machine ported from IDSS `src/safety/ews.cpp`, SQLite event store).

## Layout

```
RigMind_Desktop/
  CMakeLists.txt
  src/main.cpp              Win32 window, WebView2 host, virtual host https://rigmind.app
  src/backend/              alarm_state, event_store (SQLite), bridge (JSON endpoint)
  tests/backend_test.cpp    backend unit tests (builds on Linux and Windows)
  resources/                rigmind.ico (from src/assets/rigmind-loader.png), app.rc
  third_party/webview2/     WebView2 SDK headers + WebView2LoaderStatic.lib (NuGet Microsoft.Web.WebView2 1.0.2903.40)
  third_party/sqlite3/      sqlite amalgamation (from IDSS)
  third_party/json/         nlohmann json 3.11.3
  web/                      SPA build of Rigmind_v2 (index.html + assets)
```

## Rebuild the web bundle

```
cd Rigmind_v2
bun install
bun --bun node_modules/vite/bin/vite.js build --config vite.desktop.config.ts
# copy .output/public/_shell.html -> RigMind_Desktop/web/index.html and assets/ -> RigMind_Desktop/web/assets/
```

Note: system Node 18 cannot run Vite 8; run the build under `bun --bun`.

## Build (Windows, MSVC)

Prerequisites: Visual Studio 2022 (C++ desktop workload), CMake 3.20+, WebView2 Runtime (preinstalled on Windows 11).

```
cmake -S RigMind_Desktop -B RigMind_Desktop/build -G "Visual Studio 17 2022" -A x64
cmake --build RigMind_Desktop/build --config Release
ctest --test-dir RigMind_Desktop/build -C Release
```

Run `RigMind_Desktop/build/Release/RigMind.exe`. Data lives in `%LOCALAPPDATA%\RigMind\` (`rigmind.db`, `WebView2\`).

## Installer

```
cd RigMind_Desktop/build
cpack -C Release -G NSIS
```

Requires NSIS on PATH. Output: `RigMind-1.0.0-win64.exe`.

## Status

- Verified on Linux (g++ 13, `-Wall -Wextra -Werror`): backend tests pass (`backend_test`).
- Not verified here: `src/main.cpp` (Win32/WebView2 host), `app.rc`, NSIS packaging. This box has no MSVC, mingw or Windows.
- Bridge methods: `alert.raise`, `alert.respond`, `alerts.list` (safety alerts), `records.get|set|list` (preferences, camera details, HSE decisions, report summaries), `alarm.step|reset`, `events.recent`.
- UI routes through the bridge when inside the host (`src/lib/native-bridge.ts` in Rigmind_v2): `raiseAlert`, alert respond, `readDevice`/`writeDevice`/`listDevice`. Browser runs keep the sql.js path. Toast, bell and push UI unchanged.
- Not yet: live alarm feed from the EWS port into the UI (`alarm.step` is not called by any screen yet).
