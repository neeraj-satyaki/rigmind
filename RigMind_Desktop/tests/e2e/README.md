# RigMind visible end-to-end test

One script drives the real installer and the real app in visible windows at human pace.

    python RigMind_Desktop\tests\e2e\visible_e2e.py RigMind_Desktop\release-out\RigMind-1.0.0-win64-r6.exe --demo no
    python ...\visible_e2e.py --no-install --no-uninstall --app-dir <dir> --port 9444 --phases 2,3,4,5   (UI phases on a scratch copy)
    python ...\visible_e2e.py --inspect-windows      (dump controls of an open Setup/Uninstall wizard)

Options: --demo yes|no, --no-install, --no-uninstall, --phases, --live-seconds (180), --evidence-wait, --pace (0.3 = fast), --inspect, --elevate.

Phases 1 and 9 drive an elevated NSIS wizard: run from an ELEVATED terminal (UIPI blocks a normal shell), or pass `--elevate` (one UAC prompt, output also in elevated-run.log).
Phase 9 only ever acts on C:\Program Files\RigMind and the test's own results\<run>\appdata folder.
The app runs with an isolated LOCALAPPDATA/APPDATA/TEMP under results\<run>\appdata and WebView2 debug port --port (default 9333).
Output: results\run-<timestamp>\screenshots, summary.json, reports\ (PDF/text). Exit code 1 if any check FAILs; NOTE = not observed / informational.
Do not run two copies against the same port. Do not kill the script mid-run: it leaves RigMind.exe running (stop it by hand).

Extra options: --max-cta N (controls exercised per page/dialog, default 60), --pages "Cameras,Reports" (phase 5 subset), --force-demo-flag yes|no (dry runs only: create/remove demo-data.flag in a scratch --app-dir).
Phase 5 sweeps every button/link/tab/switch/checkbox/select/input in each page, its dialogs and the top bar (Log out last, then re-login) and writes results\run-*\cta-coverage.md (CTA | type | action | observed result | PASS/FAIL/DEAD).
The app is started with the inherited environment and only LOCALAPPDATA redirected (a stripped environment stops WebView2 opening the debug port).
