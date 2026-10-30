# RigMind – structure and status

RigMind is a rig-safety monitoring application for Windows. A native host (C++/Win32 + WebView2) shows a React single-page app,
runs a GStreamer pipeline that decodes the camera, runs two ONNX models on the GPU, evaluates the IDSS early-warning (EWS) rules,
records alarm footage and raises alerts. It is licensed to Satyaki Solutions Private Limited (2026).

## 1. Runtime architecture

```
 RTSP camera (H.264, e.g. 2688x1512)                          Browser side (WebView2, https://rigmind.app)
        |                                                      ┌────────────────────────────────────────────┐
        v                                                      │ Rigmind_v2 SPA (React + TanStack Router)    │
 ┌─────────────────────── rigmind-pipeline.exe ────────────┐   │  pages, alerts, bell, toasts, reports       │
 │ rtspsrc → depay/parse → tee                              │   │  <video> ← WebRTC (H.264, 1280x720)         │
 │   ├─ recording: splitmuxsink 10 s segments (12 kept)     │   │  detections/alarm/ppe events ← host         │
 │   ├─ rigmindews (EWS element, alarm state machine)       │   └───────────────▲────────────────────────────┘
 │   └─ live: d3d11 decode → scale 1280x720 → pacer →       │                   │ postMessage JSON-RPC + events
 │        tee ├─ detection tap → ONNX PPE + IDSS (CUDA)     │   ┌───────────────┴────────────────────────────┐
 │            │     ByteTrack, platform ROI, EWS facts,     │   │ RigMind.exe (host)                          │
 │            │     PPE compliance                          │   │  window, tray icon, single session lock     │
 │            └─ gate → burn-in overlay → encoder →         │──►│  Bridge (JSON-RPC) + SQLite store + audit   │
 │               rtph264pay → webrtcbin (one viewer)        │   │  AlarmRecorder / PpeRecorder                │
 └──────────────────────────────────────────────────────────┘   │  footage.rigmind.app → <data>\footage       │
                                                                │  report.savePdf (WebView2 PrintToPdf)       │
                                                                └─────────────────────────────────────────────┘
```

* The pipeline is a separate process; the host talks to it over stdin/stdout JSON lines.
* Inference, EWS and recording run whether or not anyone watches. The WebRTC branch is attached only while the Live page is open.
* Data lives in `%LOCALAPPDATA%\RigMind` (SQLite `rigmind.db`, `footage\<cam>\<stamp>\seg*.mp4`, `audit.log`, WebView2 profile).

## 2. Repository layout

```
LICENSE                         proprietary licence (Satyaki Solutions Private Limited 2026)
docs/STRUCTURE.md               this file
RigMind_Desktop/                native host, pipeline, installer, tests
  CMakeLists.txt                targets RigMind, rigmind-pipeline, librigmindews, rigmind_backend, tests; CPack NSIS installer
  src/main.cpp                  Win32 window, WebView2, tray, single session, report PDF, footage virtual host, event routing
  src/backend/                  alarm_state (IDSS EWS state machine), event_store (SQLite), bridge (JSON-RPC),
                                alarm_recorder (+PpeRecorder), audit_log
  src/pipeline/                 pipeline_main.cpp (GStreamer, WebRTC, pacer, recording, evidence)
                                inference.cpp/.h (ONNX Runtime CUDA, ROI, facts, tracking, PPE check)
                                ews_facts.h, ppe_check.h, bytetrack.h, platform_overlay.*, overlay_draw.*
                                plugins/ews  (GStreamer element `rigmindews`)
  src/stream/                   pipeline process + router (host side)
  src/slm/                      optional BitNet/llama server client (not shipped in the Windows installer)
  src/host/bridge_cli.cpp       backend without UI (tests, Linux host)
  models/                       ppe.onnx, idss_model.onnx (Git LFS), inference.json (source) and inference.cfg (encoded settings)
  scripts/                      inference_cfg.py (encode/check), stage_web.ps1, stage_runtime.ps1, stage_gpu_runtime.ps1, packaging
  gpu-pack/                     tooling for the downloadable CUDA/cuDNN packs
  tests/                        see section 7
Rigmind_v2/                     React SPA (Lovable-connected project)
  src/routes/                   fleet, rig, live, alerts, events, evidence, analytics, reports, actions, admin/* (cameras, rules,
                                zones, users, integrations, license, health, audit), login, report/$reportId
  src/components/rig/           screens and shared UI; Real*.tsx are the real-data versions, others hold the optional sample data
  src/lib/                      native-bridge, camera-status, detections, alarm-alerts, ppe-alerts, alert-bus/-colors/-sound,
                                live-analytics, real-data, real-state, plain-summary, demo-mode, device-db, demo-export (CSV)
  src/reports/                  printable reports (templates, real-templates)
  src/data/mock.ts              the optional sample data set
```

## 3. Detection and alarm logic

* **Models**: PPE (Gloves, Vest, goggles, helmet, mask, safety_shoe), IDSS segmentation (actuator, derrickman, elevators, machine, platform).
  Both on CUDA (cuDNN 8.9, CUDA 11.8), conf/iou from `inference.cfg` (PPE 0.25/0.45, IDSS 0.30/0.50).
* **Platform ROI**: IDSS detections are filtered to the platform rectangle scaled by 1.2 (wide) or 1.4 (zoomed); held 30 frames.
* **EWS facts** (ported from IDSS `ews.cpp`): machine present, link-tilt angled (centroid angle in range), elevator near platform,
  elevator present, platform/elevator mask collision, derrickman out of safe zone, machine past right edge. The threshold set
  follows the measured platform width (zoomed or wide); persistence counters apply before a state changes.
* **State chain**: Yellow (machine + angled tilt) → Orange (+ elevator near platform) → Red (+ elevator present and one of collision,
  derrickman out of safe zone, machine past right edge). Yellow means "the app is monitoring" and is quiet; Orange and Red notify.
* **PPE compliance**: each tracked derrickman is checked for the required items (default helmet, vest, gloves); a missing item
  raises an Advisory alert after `missing_seconds` and clears after `clear_seconds`. The wording is "not detected", never "not worn".
* **Tracking**: ByteTrack (Kalman filter, two-stage association, per-class ids) removes display flicker and gives stable person ids.
  The EWS inputs are not smoothed.

## 4. Alerts, audit and evidence

* One alert per episode: Monitoring (Yellow) → Escalated (Orange/Red) → Cleared; response Acknowledged/Escalated by the operator.
* Every state change writes an `alarm_events` row and an audit row (`alarm.transition`, `ppe.transition`, `alert.respond`, ...).
* A Red episode saves a clip set (pre-roll + the segment holding the Red moment) under `footage\<cam>\<stamp>`; clips expire after 7 days.
* The UI shows a coloured toast per level (Yellow/Orange/Red), a bell row with timestamps, the Alerts inbox, a tray notification and a tone
  for Orange and Red (mute control on Live).

## 5. Bridge methods (JSON-RPC, `window.chrome.webview.postMessage`)

`alarm.step alarm.simulate alarm.reset alert.raise alert.respond alerts.list alerts.feed audit.write audit.recent events.recent
event.describe footage.list footage.files footage.record records.get records.set records.list stream.open stream.offer
stream.detach stream.close stream.status app.version`, plus `report.savePdf` handled in `main.cpp`.
Host → page events: `status stats answer peer detections inference facts latency alarm ppe ppe_status evidence clip description error`.

## 6. Installer and configuration

* CPack NSIS installer (`RigMind-1.0.0-win64.exe`, ~292 MB): app, models, GStreamer runtime, WebView2 bootstrapper, licence page,
  Start menu and Desktop shortcuts, uninstaller.
* The NVIDIA runtime (cuDNN 474 MB, CUDA 629 MB) is downloaded at install time from the public release, SHA-256 verified, into `<install>\gpu`.
* Sample data is an install-time question (default No); Yes writes `<install>\demo-data.flag`. Without the flag the app shows only real data
  or empty states.
* One RigMind session per user data folder; a second start brings the running window forward.
* Build: `vite build --config vite.desktop.config.ts` (output in `Rigmind_v2\.output\public`), then `RigMind_Desktop\scripts\stage_web.ps1`,
  `cmake --build build-app --config Release`, `cpack -G NSIS`.

## 7. Tests

| Suite | What it covers |
|---|---|
| `backend_test`, `roi_facts_test`, `platform_overlay_test`, `tracking_test`, `ews_plugin_test` | C++ units: bridge/SQLite/episodes, ROI and facts, overlay geometry, ByteTrack and PPE check, EWS plugin |
| `tests/cases` (`generate_cases.py`, `run_cases.py`) | 15,682 generated bridge cases (failed=0) |
| Vitest (`Rigmind_v2`) | 116 UI tests (alert bridge, PPE alerts, real-data pages, detections, analytics) |
| `tests/e2e/visible_e2e.py` | visible install → app → pages → live data → report → settings → uninstall (needs UAC) |
| `tests/validation/perf_validate.py`, `nav_resume_check.py`, `ppe_transitions.py` | inference/latency/bandwidth report, leave-and-return stream check, PPE episode summary |

## 8. Known limits and open items

* The camera feed delivers about 3–6 frames per second with bursts; smoothness is limited by the source, and the inference gate trades frame rate for overlay accuracy.
* Saved zones are stored but do not affect detection or alarms yet.
* BitNet summaries are not installed on Windows: plain summaries use a template sentence grounded in the real fields.
* Red depends on the IDSS mask-contour collision or the position rules; the footage used for testing never reaches Red.
* Acknowledgement times, rig names/regions and user accounts are not provided by the host (shown as "—" or "Operator").
* The visible e2e driver's wizard clicking was not proven end to end (a person clicked the wizard during the run); its settings-restart check disagrees with manual tests.
