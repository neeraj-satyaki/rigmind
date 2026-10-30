# RigMind – running the tests by hand

Run each block in PowerShell from `D:\Projects\rigmind`, one at a time. Use the full Node path (Bun fails on the UI tests).
Build folder: `RigMind_Desktop\build-app\Release`. Rebuild first if needed:
`& "C:\Program Files\CMake\bin\cmake.exe" --build D:\Projects\rigmind\RigMind_Desktop\build-app --config Release`

## 1. UI tests and type check (about 1 minute)
```powershell
cd D:\Projects\rigmind\Rigmind_v2
& "C:\Program Files\nodejs\node.exe" node_modules\typescript\bin\tsc --noEmit          # prints nothing when clean
& "C:\Program Files\nodejs\node.exe" node_modules\vitest\vitest.mjs run                 # expect 16 files, 116 tests passed
```

## 2. C++ unit tests (about 30 seconds)
```powershell
cd D:\Projects\rigmind\RigMind_Desktop\build-app\Release
.\backend_test.exe            # "backend tests passed"
.\roi_facts_test.exe          # "all passed"
.\platform_overlay_test.exe   # "all passed"
.\tracking_test.exe           # "all passed"
.\ews_plugin_test.exe "$PWD\librigmindews.dll"   # "3 transitions match the state machine"
```

## 3. The 15,682 generated cases (about 45 seconds)
```powershell
cd D:\Projects\rigmind\RigMind_Desktop\tests\cases
python generate_cases.py
python run_cases.py ..\..\build-app\Release\rigmind-bridge-cli.exe $env:TEMP\results.csv   # expect total=15682 failed=0
```

## 4. Pipeline on the real camera, no app (about 2 minutes; the camera must be reachable)
```powershell
cd D:\Projects\rigmind\RigMind_Desktop
$env:RIGMIND_PROVIDER="cuda"
python tests\validation\run_pipeline.py 120 $env:TEMP\run.jsonl
```
Prints event counts, the EWS facts table and every alarm. On the test footage expect Yellow and Orange and no Red.

## 5. Leave-and-return stream check (about 3 minutes)
Use a private copy of the installed layout, never `C:\Program Files\RigMind` itself.
```powershell
Copy-Item "C:\Program Files\RigMind" D:\Projects\rigmind\tests_run\app_nav -Recurse
python tests\validation\nav_resume_check.py D:\Projects\rigmind\tests_run\app_nav 9660 5 $env:TEMP\nav.json
```
Exit code 1 means failure: the video must resume within 3 seconds and detections must keep arriving while on other pages.
Slow-model variant: add `RIGMIND_TEST_INFER_DELAY_MS=2500 MINDET=2`.

## 6. Performance and validation report (about 12 minutes; close other apps first)
```powershell
python tests\validation\perf_validate.py        # writes tests\validation\VALIDATION_REPORT.md
```
`--duration 60 --skip-wait` gives a short dry run.

## 7. Visible install, app and uninstall test (needs UAC; 25 minutes or more)
Open an **elevated** PowerShell (Run as administrator), then:
```powershell
cd D:\Projects\rigmind\RigMind_Desktop\tests\e2e
python -u visible_e2e.py ..\..\release-out\RigMind-1.0.0-win64-r11.exe --demo no          # all phases 1-9
python -u visible_e2e.py --no-install --no-uninstall --app-dir "C:\Program Files\RigMind" --demo no --phases 2,3,4,5,6,7,8   # app phases only
```
* Do not touch the mouse or keyboard while it drives the installer windows.
* The wizard clicking has not been proven end to end; if it fails, click through the installer yourself.
* Results go to `tests\e2e\results\run-<time>\` (`cta-coverage.md`, screenshots, `summary.json`).

## 8. Quick manual checks in the app
1. Sign in and open Live: the video plays and the PPE and IDSS boxes follow the picture.
2. Wait 15–30 seconds: an Orange alert gives a coloured toast, a tone, a bell row with a timestamp and a Windows notification. Click Acknowledge, then Open event investigation.
3. Go to Alerts, Fleet and back to Live: the video comes back within a few seconds.
4. Reports: Download PDF saves to `Documents\RigMind Reports`; Download CSV opens cleanly in Excel.
5. Start RigMind a second time: the first window comes forward and no second copy opens.
6. Administration / Cameras: use the password eye, Ignore and Validate, then save.

## One-file triggers: `RigMind_Desktop/scripts/rigmind.ps1`

Everything is visible; nothing runs hidden unless you pass `-Silent`.

| Command | What it does |
|---|---|
| `.\rigmind.ps1 install [-Installer path] [-Demo yes\|no] [-Bitnet yes\|no] [-Silent]` | Drives the installer wizard, then checks files, GPU runtime, BitNet model, shortcuts, uninstall entry (asks for UAC once). |
| `.\rigmind.ps1 uninstall [-ClearData -Yes]` | Drives the uninstaller and checks nothing is left; `-ClearData` also removes `%LOCALAPPDATA%\RigMind`. Reports in Documents are never touched. |
| `.\rigmind.ps1 test [-Suite unit,cases,ui,e2e,load,all] [-Build]` | Runs the chosen suites and writes `tests/run-results/run-<time>.md`. `e2e` needs a normal (non-admin) window and an installed app. |
| `.\rigmind.ps1 cycle [-KeepInstalled]` | Uninstall old, install, app test, uninstall. |
| `.\rigmind.ps1 hooks [-Remove]` | Git pre-push hook running unit, cases and ui. |
