# RigMind

Rig-safety monitoring for Windows. RigMind watches an RTSP camera, runs two ONNX models on the GPU (PPE and IDSS rig
segmentation), applies the IDSS early-warning (EWS) rules, raises Yellow / Orange / Red alarms, records evidence footage and
shows everything in a desktop app. Licensed to Satyaki Solutions Private Limited, 2026 (see `LICENSE`).

## What it does

* **Live monitoring** – WebRTC video (1280x720) with PPE and IDSS detections drawn on the frame, the platform region of interest and
  the IDSS masks.
* **Early warning** – the IDSS state machine: Yellow (monitoring), Orange (warning), Red (critical). One alert per episode, an audit
  row for every transition, coloured stacked toasts, a bell with timestamps, a tray notification and a tone for Orange and Red.
* **Event cycles** – Yellow>Orange>Yellow is a normal cycle; Yellow>Orange>Red>Orange>Yellow and Yellow>Orange>Red>Yellow are critical cycles. Each closed cycle is one alert (Cleared) and gets a one or two line plain-language summary (BitNet, with a fixed action sentence if the model omits one) built from the scene: class names, positions, mask contours and centroid paths. Also on Red, PPE advisories and the Alerts page "Plain words" button.
* **PPE compliance** – each person the IDSS model finds is checked for the required items (default helmet, vest, gloves); a missing
  item raises a quiet Advisory alert ("PPE not detected: vest"). ByteTrack removes box flicker and gives stable person ids.
* **Evidence** – a Red alarm saves a clip set (pre-roll and the segment holding the Red moment) that plays in the Evidence library;
  clips expire after 7 days.
* **Reports and exports** – printable reports with Download PDF (saved to `Documents\RigMind Reports`) and readable CSV.
* **Real data** – every page shows live data or an honest empty state. Sample (demo) data is an install-time option, off by default.
* **Desktop behaviour** – one session per user data folder, tray icon, password show/hide, Start menu and Desktop shortcuts.

## Install (Windows 10/11 x64, NVIDIA GPU recommended)

1. Download `RigMind-1.0.0-win64.exe` from the release page: https://github.com/neeraj-satyaki/rigmind/releases/tag/v1.0.1-win64
2. Run it. It asks whether to include sample data (default No) and downloads the NVIDIA GPU runtime
   (cuDNN and CUDA, about 1.1 GB, SHA-256 checked; internet access needed for this step).
   Without the GPU runtime RigMind runs on the CPU, which is slow.
3. Start RigMind from the Start menu or the Desktop shortcut. Sign in, then add the camera under Administration / Cameras
   (an `rtsp://` address; username and password are optional, use Ignore for cameras without a login) and validate it.

Data is kept in `%LOCALAPPDATA%\RigMind` (database, saved footage, audit log). The uninstaller removes the program folder only.

## Repository layout

| Path | Contents |
|---|---|
| `RigMind_Desktop/` | native host (Win32 + WebView2), GStreamer pipeline `rigmind-pipeline.exe`, EWS plugin, backend (SQLite, bridge), models, installer, tests |
| `Rigmind_v2/` | the React single-page app (TanStack Router), served inside WebView2 at `https://rigmind.app` |
| `docs/STRUCTURE.md` | detailed architecture, layout, alarm logic, bridge methods, installer and known limits |
| `docs/TESTING.md` | how to run every test by hand |
| `LICENSE` | proprietary licence |

Detection models (`ppe.onnx`, `idss_model.onnx`) are stored with Git LFS in the main repository and are inside the installer.
Third-party runtimes (GStreamer, ONNX Runtime, OpenCV, WebView2 SDK, CUDA/cuDNN) are fetched by the scripts in
`RigMind_Desktop/scripts` and are not committed.

## Build from source (Windows)

```powershell
# 1. UI bundle  (output goes to Rigmind_v2\.output\public)
cd Rigmind_v2
node node_modules\vite\bin\vite.js build --config vite.desktop.config.ts
# 2. stage it for the host and the installer
cd ..\RigMind_Desktop
pwsh scripts\stage_web.ps1
# 3. native host, pipeline, plugin and tests
cmake --build build-app --config Release
# 4. installer
cd build-app
cpack -C Release -G NSIS -D CPACK_NSIS_COMPRESSOR=zlib
```

First configure with the GStreamer, ONNX Runtime, OpenCV and WebView2 paths (see `RigMind_Desktop/README.md` and `scripts\`).
Runtime settings (confidence and IoU, platform scale, tracking, PPE check, output rate) live in `RigMind_Desktop/models/inference.json`;
`python scripts/inference_cfg.py encode` regenerates the encoded `inference.cfg` the app reads.

## Tests

C++ units (backend, ROI and facts, overlay, tracking, EWS plugin), 15,682 generated bridge cases, 116 UI tests, a leave-and-return stream
check, a performance validation harness and a visible install-to-uninstall test. Commands and expected results are in `docs/TESTING.md`.

## Status and known limits

* The test camera delivers about 3-6 frames per second in bursts; smoothness is limited by the source, and the inference gate
  trades frame rate for overlay accuracy.
* Saved zones are stored but do not drive detection or alarms yet.
* BitNet plain-language summaries are optional at install: the installer asks, then downloads the 1.19 GB model (about 1.2 GB of memory while RigMind runs; 7-40 s per summary on the CPU). Without it, summaries use a template sentence.
* The test footage reaches Yellow and Orange but not Red.
* The visible install test driver's wizard clicking has not been proven end to end.
