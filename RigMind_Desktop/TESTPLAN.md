# RigMind Windows Test Plan

Prompt for an AI agent (Claude) that runs on a Windows machine.
Written in simplified technical English (ASD-STE100 style).

## 1. Role and rules for the agent

1. You test RigMind on a real Windows machine.
2. Run one step at a time. Do not skip steps.
3. Record the evidence for each step. Evidence is the command output, the file content, or the screenshot path.
4. If a step fails, stop. Write the failure in `TEST_REPORT.md`. Do not continue.
5. Do not change source code. Only report.
6. Do not delete any file in `%LOCALAPPDATA%\RigMind\`. That folder holds user data.
7. Do not use the words "probably", "should", or "seems". State only what you observed.

## 2. Prerequisites

Check each item. Record yes or no.

| ID | Item | Check command or evidence |
|----|------|---------------------------|
| P-01 | Windows 10 (1809 or later) or Windows 11, x64 | `systeminfo \| findstr /B /C:"OS Name" /C:"System Type"` |
| P-02 | Visual Studio 2022, workload "Desktop development with C++" | `"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property displayName` |
| P-03 | CMake 3.20 or later | `cmake --version` |
| P-04 | NSIS 3.x on PATH | `makensis /VERSION` |
| P-05 | GStreamer MSVC 1.24.x, runtime and development packages | `echo %GSTREAMER_1_0_ROOT_MSVC_X86_64%` |
| P-06 | pkg-config on PATH | `pkg-config --version` |
| P-07 | Microsoft Edge WebView2 Runtime | Registry key `HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}` exists |
| P-08 | BitNet runtime built: `third_party\bitnet.cpp\build\bin\Release\llama-server.exe` | `dir` the path |
| P-09 | BitNet model: `third_party\bitnet-model\ggml-model-i2_s.gguf` | `dir` the path, size about 1.19 GB |
| P-10 | Test RTSP camera or RTSP test server reachable at `rtsp://127.0.0.1:8555/live` | Open the URI in VLC, or run `tests\rtsp\rtsp_test_server.c` |

If any prerequisite is "no", stop. Report the missing item.

## 3. Build

| ID | Action | Expected result |
|----|--------|-----------------|
| B-01 | `cd RigMind_Desktop` | Current folder is `RigMind_Desktop`. |
| B-02 | Run `scripts\package_windows.ps1` | Script ends without error. |
| B-03 | Check folder `slm-windows\` | Contains `llama-server.exe`, `ggml-model-i2_s.gguf`, and DLL files. |
| B-04 | Check file `build\Release\RigMind.exe` | File exists. Size greater than 100 KB. |
| B-05 | Check file `build\Release\rigmind-pipeline.exe` | File exists. |
| B-06 | Check file `build\Release\backend_test.exe` | File exists. |
| B-07 | Check the CMake log | No line starts with `error`. |
| B-08 | Check the build of `RigMind.exe` | `main.cpp`, `slm_client.cpp`, `pipeline_process.cpp` compile without error. |

If B-08 fails, copy the first compiler error line into the report. Stop.

## 4. Backend unit tests

| ID | Action | Expected result |
|----|--------|-----------------|
| U-01 | Run `build\Release\backend_test.exe` | Prints `backend tests passed`. Exit code 0. |
| U-02 | Check the test folder `%TEMP%\rigmind-audit-test` after the run | Folder does not exist. The test removed it. |
| U-03 | Check the test folder `%TEMP%\rigmind-footage-test` after the run | Folder does not exist. |

## 5. Installer

| ID | Action | Expected result |
|----|--------|-----------------|
| I-01 | `cd build` then run `cpack -C Release -G NSIS` | Creates `RigMind-1.0.0-win64.exe`. |
| I-02 | Run `RigMind-1.0.0-win64.exe` silently: `/S` | Installs without a window. Exit code 0. |
| I-03 | Check `%ProgramFiles%\RigMind\RigMind.exe` | File exists. |
| I-04 | Check `%ProgramFiles%\RigMind\rigmind-pipeline.exe` | File exists. |
| I-05 | Check `%ProgramFiles%\RigMind\slm\llama-server.exe` | File exists. |
| I-06 | Check `%ProgramFiles%\RigMind\slm\ggml-model-i2_s.gguf` | File exists. Size about 1.19 GB. |
| I-07 | Check the start menu shortcut `RigMind.lnk` | Shortcut exists and points to `RigMind.exe`. |

## 6. Application start

| ID | Action | Expected result |
|----|--------|-----------------|
| H-01 | Start `%ProgramFiles%\RigMind\RigMind.exe` | A window titled `RigMind` opens within 15 seconds. |
| H-02 | Wait 20 seconds. Check the window | The login or main page is shown. No error dialog. |
| H-03 | Check the process list: `tasklist /FI "IMAGENAME eq llama-server.exe"` | `llama-server.exe` is running. |
| H-04 | Check the process list: `tasklist /FI "IMAGENAME eq rigmind-pipeline.exe"` | Process runs only if a camera stream is open. Record the result. |
| H-05 | Check that no console window opens for `llama-server.exe` | No console window appears. |
| H-06 | Check the folder `%LOCALAPPDATA%\RigMind\` | Contains `rigmind.db`, `audit.log`, and `WebView2\`. |
| H-07 | Check `audit.log` (last line) | Contains `"action":"app.start"` or the launcher event. Record the line. |

If H-01 fails, collect `%LOCALAPPDATA%\RigMind\` and the Windows Event Viewer entries for `RigMind.exe`. Stop.

## 7. Storage (SQLite)

Use the `sqlite3.exe` command line tool. Put it in `PATH`. Run each query on `%LOCALAPPDATA%\RigMind\rigmind.db`.

| ID | Action | Expected result |
|----|--------|-----------------|
| S-01 | `sqlite3 rigmind.db "select count(*) from app_records;"` | Number is 0 or greater. |
| S-02 | `sqlite3 rigmind.db "select name from sqlite_master where type='table';"` | Lists `alarm_events`, `alerts`, `alert_responses`, `app_records`, `audit_log`, `footage`. |
| S-03 | `sqlite3 rigmind.db "pragma journal_mode;"` | Returns `wal`. |
| S-04 | `sqlite3 rigmind.db "select action, source from audit_log order by id desc limit 3;"` | Lists `app.start` with source `backend`. |

## 8. UI actions reach SQLite

Do each action in the app. Then run the query. Record the row count before and after.

| ID | Action in the app | Query after the action | Expected result |
|----|-------------------|------------------------|-----------------|
| A-01 | Open the Alerts page, click "Acknowledge" on one alert | `select id, status from alert_responses;` | One row with status `Acknowledged`. |
| A-02 | Click any button in the app | `select count(*) from audit_log where source='ui';` | Count increases by at least 1 within 3 seconds. |
| A-03 | Change page with the menu | `select target from audit_log where action='navigate' order by id desc limit 1;` | Target equals the new page path. |
| A-04 | Click the Dark theme button | `select value from app_records where key='preferences';` | Value contains `"dark":true`. |
| A-05 | Close the app, start it again | `select count(*) from alert_responses;` | Count is the same as before the close. Data survived the restart. |

## 9. Alerts, toast and sound

| ID | Action | Expected result |
|----|--------|-----------------|
| R-01 | Click "Notifications" (bell), then "Test notification" | Toast appears after about 2 seconds. Text includes `Critical`. |
| R-02 | Listen for sound after R-01 | Red alert tone plays. Record yes or no. Use a real speaker. |
| R-03 | Query `select severity from alerts order by id desc limit 3;` | Rows with `Advisory`, `Warning`, and `Critical`. |
| R-04 | Query `select action, target from audit_log where action='alarm.simulate' order by id desc limit 1;` | Row exists. |
| R-05 | Check the second toast | Plain sentence in the toast. Sentence names the camera and the place. |

## 10. Plain-language descriptions (BitNet)

| ID | Action | Expected result |
|----|--------|-----------------|
| L-01 | Run R-01 with `llama-server.exe` running | The toast text is one sentence. Length 220 characters or less. |
| L-02 | Stop `llama-server.exe`: `taskkill /IM llama-server.exe /F`. Run R-01 again | Toast shows the template sentence. The app does not crash. |
| L-03 | Check the audit row for the description | `select detail from audit_log where action='event.describe' order by id desc limit 1;` returns `slm` or `template`. |
| L-04 | Restart the app | `llama-server.exe` starts again within 60 seconds. |

## 11. Camera stream and recording

Requires the RTSP camera from P-10. Open the Live page.

| ID | Action | Expected result |
|----|--------|-----------------|
| V-01 | Open the Live page | The main tile shows video within 30 seconds. |
| V-02 | Check `%LOCALAPPDATA%\RigMind\cache\CAM-01\` (camera id from the data) | At least 2 `seg*.mp4` files after 40 seconds. Each file greater than 0 bytes. |
| V-03 | Run R-01 (red test) | Within 10 seconds a folder appears under `%LOCALAPPDATA%\RigMind\footage\CAM-01\`. |
| V-04 | `select cam_id, clip_count, path from footage;` | One row. `clip_count` is 3. Path has the form `CAM-01/<digits>`. |
| V-05 | Check the folder from V-03 | Contains 3 `seg*.mp4` files. |
| V-06 | `select expires_ms - ts_ms from footage;` | Value is 604800000 (7 days in ms). |

If V-01 fails, record the pipeline window output. Check the `rtsp://` address in `records` table key `stream.sources`.

## 12. Retention (7 days)

| ID | Action | Expected result |
|----|--------|-----------------|
| F-01 | Stop the app. Set `ts_ms` of one `footage` row to 8 days in the past: `update footage set ts_ms = ts_ms - 691200000, expires_ms = expires_ms - 691200000;` | Update runs. |
| F-02 | Start the app. Wait 10 seconds | The old row is removed from `footage`. |
| F-03 | Check the folder of the removed row | Folder is deleted. |
| F-04 | Check other footage rows | Rows newer than 7 days stay. |

## 13. Reinstall keeps data

| ID | Action | Expected result |
|----|--------|-----------------|
| D-01 | Before reinstall: `sqlite3 rigmind.db "select count(*) from alert_responses;"` | Record the number N. |
| D-02 | Run the installer again with `/S` | Exit code 0. |
| D-03 | Query the count again | Still N. Data kept. |
| D-04 | Check `audit.log` | Contains an `app.install` line from the installer. |

## 14. Uninstall

| ID | Action | Expected result |
|----|--------|-----------------|
| X-01 | Run `%ProgramFiles%\RigMind\Uninstall.exe /S` | Exit code 0. |
| X-02 | Check `%ProgramFiles%\RigMind\` | Folder removed. |
| X-03 | Check `%LOCALAPPDATA%\RigMind\rigmind.db` | File still exists. User data kept. |

## 15. Report

Create `TEST_REPORT.md` with:

1. One line per test ID: `ID | PASS or FAIL | evidence`.
2. Each FAIL with the first error line.
3. Build machine: Windows version, Visual Studio version, GStreamer version.
4. List of steps not run, and why.

Do not write "pass" unless you ran the step and saw the expected result.
