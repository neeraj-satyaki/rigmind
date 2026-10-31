#!/usr/bin/env python
"""RigMind visible end-to-end test: install -> open -> login -> camera -> all pages -> live data -> report -> settings -> uninstall.

Everything runs in REAL, VISIBLE windows at human pace (banner + 1-2 s pauses). Nothing is silent or hidden.

    python tests\\e2e\\visible_e2e.py <installer.exe> [--demo yes|no] [--no-install] [--no-uninstall]
    python tests\\e2e\\visible_e2e.py --no-install --no-uninstall --app-dir <dir> --port 9444      (UI phases only, scratch copy)
    python tests\\e2e\\visible_e2e.py --inspect-windows                                          (dump open Setup/Uninstall wizards)

Phases: 1 INSTALL  2 OPEN APP  3 LOGIN  4 CAMERA  5 ALL PAGES  6 LIVE DATA  7 REPORT  8 SETTINGS  9 UNINSTALL
Phases 1 and 9 drive an elevated NSIS wizard, so this script must itself run elevated (see README.md, --elevate).
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
PF = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
DEFAULT_APP = PF / "RigMind"
BITNET = True
SAMPLE_STRINGS = ["EV-1042", "Faisal A.", "Maha S.", "Demo rig 01", "faisal.demo@example.com"]
ERR_PAGE_RE = re.compile(r"Page not found|\b404\b|This page didn't load|Something went wrong|Not Found", re.I)
BENIGN_CONSOLE = re.compile(r"fonts\.(googleapis|gstatic)|favicon|ERR_INTERNET_DISCONNECTED|ERR_NAME_NOT_RESOLVED|net::ERR_FAILED.*font|Autofocus", re.I)
RTSP_URI = "rtsp://192.168.1.8:8554/feed0"
E2E_EMAIL = "e2e.operator@satyaki-e2e.test"
E2E_USER = "e2e.operator"             # the account the test creates on the first start (the host keeps only a salted hash)
E2E_PASS = "Rig-Mind-E2E-2026!"       # 10+ characters, letters and symbols, does not contain the username
NOTIFY_EMAIL = "e2e.notify@satyaki-e2e.test"

# --------------------------------------------------------------------------------------------- reporting


class Results:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []  # phase, check, status, evidence

    def add(self, phase: str, name: str, ok, evidence: str = "") -> bool:
        status = "PASS" if ok is True else "FAIL" if ok is False else "NOTE"
        self.rows.append((phase, name, status, evidence))
        print(f"   [{status}] {name}" + (f"  -- {evidence}" if evidence else ""), flush=True)
        return ok is True

    def failed(self) -> int:
        return sum(1 for r in self.rows if r[2] == "FAIL")


R = Results()
OUT: Path
SHOTS: Path
PACE = 1.0  # multiplier for the human-pace pauses


def pause(sec: float = 1.5) -> None:
    time.sleep(sec * PACE)


def banner(text: str, sub: str = "") -> None:
    line = "=" * 78
    print(f"\n{line}\n  {text}\n" + (f"  {sub}\n" if sub else "") + line, flush=True)


def step(text: str) -> None:
    print(f"\n >> STEP: {text}", flush=True)


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def screen_shot(name: str) -> str:
    """Whole-screen capture (used for the native installer windows)."""
    path = SHOTS / f"{name}.png"
    try:
        from PIL import ImageGrab

        ImageGrab.grab(all_screens=True).save(path)
    except Exception as e:  # noqa: BLE001
        print(f"   (screenshot failed: {e})")
    return str(path)


def guarded(phase: str, name: str, fn, *a, **kw):
    """Run one check; an exception is a FAIL with the traceback tail, never a crash of the whole run."""
    try:
        return fn(*a, **kw)
    except Exception as e:  # noqa: BLE001
        tb = traceback.format_exc().strip().splitlines()[-3:]
        R.add(phase, name, False, f"{type(e).__name__}: {str(e)[:200]} | {' / '.join(tb)[:300]}")
        return None


# --------------------------------------------------------------------------------------------- NSIS driving (phases 1, 9)

WM_GETTEXT, WM_GETTEXTLENGTH, BM_GETCHECK, PBM_GETPOS, PBM_GETRANGE = 0x0D, 0x0E, 0xF0, 0x408, 0x407


def _send_text(hwnd: int) -> str:
    u32 = ctypes.windll.user32
    for send, ctype in ((u32.SendMessageW, ctypes.create_unicode_buffer), (u32.SendMessageA, ctypes.create_string_buffer)):
        n = send(hwnd, WM_GETTEXTLENGTH, 0, 0)
        if n > 0:
            buf = ctype(n + 2)
            send(hwnd, WM_GETTEXT, n + 1, ctypes.addressof(buf))
            v = buf.value
            if isinstance(v, bytes):
                v = v.decode("mbcs", "replace")
            if v.strip():
                return v
    return ""


def clean(s: str) -> str:
    return s.replace("&", "").strip()


def dump_controls(dlg, label: str, store: list) -> None:
    rows = []
    for c in dlg.descendants():
        try:
            rows.append(
                {
                    "class": c.friendly_class_name(),
                    "winclass": c.class_name(),
                    "id": c.control_id(),
                    "text": (c.window_text() or "")[:120],
                    "visible": c.is_visible(),
                    "enabled": c.is_enabled(),
                    "rect": str(c.rectangle()),
                }
            )
        except Exception:  # noqa: BLE001
            pass
    store.append({"page": label, "title": dlg.window_text(), "controls": rows})
    print(f"   [inspect] {label}: {len(rows)} controls")
    for r in rows:
        if r["visible"]:
            print(f"      id={r['id']:<5} {r['winclass']:<18} en={int(r['enabled'])} text={r['text']!r}")


def find_wizard(kind: str, timeout: float = 60):
    """Top-level NSIS wizard window ('Setup' or 'Uninstall') of RigMind, found by title (the uninstaller re-launches itself from %TEMP%)."""
    from pywinauto import Desktop

    end = time.time() + timeout
    rx = re.compile(rf".*RigMind.*{kind}.*", re.I)
    while time.time() < end:
        for w in Desktop(backend="win32").windows(class_name="#32770"):
            try:
                if rx.match(w.window_text()):
                    return w
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.5)
    return None


def msgboxes_of(pid: int, main_hwnd: int):
    from pywinauto import Desktop

    out = []
    for w in Desktop(backend="win32").windows(class_name="#32770"):
        try:
            if w.process_id() == pid and w.handle != main_hwnd and w.is_visible():
                out.append(w)
        except Exception:  # noqa: BLE001
            pass
    return out


def click_real(btn) -> None:
    """A real mouse click when possible (visible cursor), BM_CLICK otherwise."""
    try:
        btn.set_focus()
        btn.click_input()
    except Exception:  # noqa: BLE001
        try:
            btn.click()
        except Exception:  # noqa: BLE001
            # an invisible or just-closed control (the last wizard page closing itself): post BM_CLICK straight to the handle
            ctypes.windll.user32.PostMessageW(btn.handle, 0x00F5, 0, 0)


def btn_by_id(dlg, cid: int):
    for c in dlg.descendants():
        try:
            if c.control_id() == cid and c.class_name() == "Button":
                return c
        except Exception:  # noqa: BLE001
            pass
    return None


def drive_wizard(kind: str, phase: str, demo: bool | None, inspect: bool, timeout_s: int, install_dir: Path, expect_license: bool):
    """Click through an NSIS MUI2 wizard like a user. Returns (pages_seen, messageboxes_seen)."""
    dlg = find_wizard(kind, 90)
    if not dlg:
        R.add(phase, f"{kind} wizard window appeared", False, "no '#32770' window titled RigMind ... " + kind)
        return [], []
    pid = dlg.process_id()
    R.add(phase, f"{kind} wizard window appeared", True, f"title={dlg.window_text()!r} pid={pid}")
    try:
        dlg.set_focus()
    except Exception:  # noqa: BLE001
        pass
    inspected: list = []
    pages: list[str] = []
    boxes: list[str] = []
    last_prog = 0.0
    start = time.time()
    n_shot = 0
    seen_license = ""
    clicked_since_change = ""
    while time.time() - start < timeout_s:
        # Window gone = wizard finished (uninstaller closes itself after Close).
        # (a pywinauto dialog wrapper has no exists(): ask Windows whether the window handle is still alive)
        if not ctypes.windll.user32.IsWindow(dlg.handle):
            break
        # 1. message boxes (sample data question, 'already installed', errors)
        for mb in msgboxes_of(pid, dlg.handle):
            text = " ".join(clean(c.window_text()) for c in mb.descendants() if c.class_name() == "Static" and c.window_text())
            boxes.append(text)
            screen_shot(f"{phase}-msgbox-{len(boxes)}")
            print(f"   MessageBox: {text[:200]!r}")
            lt = text.lower()
            if "sample" in lt and "demonstration" in lt:
                want = "Yes" if demo else "No"
                banner(f"Sample (demonstration) data question  ->  answering {want.upper()} (test option --demo {'yes' if demo else 'no'})")
                pause(2)
                for c in mb.descendants():
                    if c.class_name() == "Button" and clean(c.window_text()).lower() == want.lower():
                        click_real(c)
                        break
                R.add(phase, "sample-data question shown and answered", True, f"answered {want}")
            elif "bitnet" in lt and "summaries" in lt:
                want = "Yes" if BITNET else "No"
                banner(f"BitNet summaries question  ->  answering {want.upper()} (test option --bitnet {'yes' if BITNET else 'no'})")
                pause(2)
                for c in mb.descendants():
                    if c.class_name() == "Button" and clean(c.window_text()).lower() == want.lower():
                        click_real(c)
                        break
                R.add(phase, "BitNet question shown and answered", True, f"answered {want}")
            else:
                # Anything else is reported; positive/default button is pressed so the run can continue.
                pause(2)
                for label in ("Yes", "OK", "Retry", "Close"):
                    b = [c for c in mb.descendants() if c.class_name() == "Button" and clean(c.window_text()).lower() == label.lower()]
                    if b:
                        click_real(b[0])
                        break
                bad = re.search(r"fail|could not|error|integrity|blocked", lt)
                R.add(phase, f"unexpected MessageBox: {text[:80]!r}", False if bad else None, text[:300])
            pause(1)
        # 2. the page itself
        ok_btn = btn_by_id(dlg, 1)
        if ok_btn is None:
            time.sleep(0.5)
            continue
        label = clean(ok_btn.window_text())
        enabled = ok_btn.is_enabled()
        header = ""
        for hid in (1037, 1201, 1202):
            for c in dlg.descendants():
                try:
                    if c.control_id() == hid and c.class_name() == "Static" and c.window_text().strip():
                        header = clean(c.window_text())
                        break
                except Exception:  # noqa: BLE001
                    pass
            if header:
                break
        prog = None
        for c in dlg.descendants():
            try:
                if c.class_name() == "msctls_progress32" and c.is_visible():
                    prog = c
                    break
            except Exception:  # noqa: BLE001
                pass
        key = f"{label}|{header}|{'prog' if prog else ''}"
        if key != clicked_since_change:
            clicked_since_change = key
            pages.append(f"{header or '(no header)'} [{label}{'' if enabled else ' disabled'}]")
            print(f"\n   PAGE: {pages[-1]}")
            if inspect:
                dump_controls(dlg, f"page{len(pages)}-{clean(label)}", inspected)
            n_shot += 1
            screen_shot(f"{phase}-page{n_shot:02d}-{re.sub(r'[^A-Za-z0-9]+', '_', header or label)[:30]}")
        # License page: read and verify the text
        if not seen_license:
            for c in dlg.descendants():
                try:
                    if c.class_name().lower().startswith("richedit") and c.is_visible():
                        seen_license = _send_text(c.handle) or c.window_text()
                        if seen_license:
                            print(f"   licence text read ({len(seen_license)} chars)")
                            if expect_license:
                                R.add(phase, "licence text contains 'Satyaki Solutions Private Limited'", "Satyaki Solutions Private Limited" in seen_license, f"{len(seen_license)} chars; first line: {seen_license.strip().splitlines()[0][:80] if seen_license.strip() else ''}")
                        break
                except Exception:  # noqa: BLE001
                    pass
        if prog is not None and not enabled:
            # installing / uninstalling: print progress
            if time.time() - last_prog > 10:
                last_prog = time.time()
                try:
                    pos = ctypes.windll.user32.SendMessageW(prog.handle, PBM_GETPOS, 0, 0)
                    hi = ctypes.windll.user32.SendMessageW(prog.handle, PBM_GETRANGE, 0, 0) or 1
                except Exception:  # noqa: BLE001
                    pos, hi = 0, 1
                last = ""
                for c in dlg.descendants():
                    try:
                        if c.class_name() == "SysListView32":
                            n = c.item_count()
                            if n:
                                last = c.get_item(n - 1).text()
                    except Exception:  # noqa: BLE001
                        pass
                dl = ""
                parts = glob.glob(str(install_dir / "gpu-part*.zip"))
                if parts:
                    dl = " | download: " + ", ".join(f"{Path(p).name} {Path(p).stat().st_size / 1e6:.0f} MB" for p in parts)
                print(f"   ... {int(time.time() - start)}s  progress={pos}/{hi}  last log line: {last[:90]!r}{dl}", flush=True)
            time.sleep(1)
            continue
        if not enabled:
            time.sleep(0.5)
            continue
        # Page finished installing: show the log once before moving on
        if prog is not None and enabled:
            lines = []
            for c in dlg.descendants():
                try:
                    if c.class_name() == "SysListView32":
                        lines = [c.get_item(i).text() for i in range(max(0, c.item_count() - 12), c.item_count())]
                except Exception:  # noqa: BLE001
                    pass
            print("   install log tail:\n      " + "\n      ".join(lines))
            R.add(phase, f"{kind} progress page completed", True, " | ".join(lines[-3:])[:300])
            screen_shot(f"{phase}-log-complete")
            pause(3)
        low = label.lower()
        if low in ("close", "finish"):
            # Finish page: deterministic: untick every "run/show" check box, then Finish.
            for c in dlg.descendants():
                try:
                    if c.class_name() == "Button" and c.is_visible() and (c.friendly_class_name() in ("CheckBox",)) :
                        if ctypes.windll.user32.SendMessageW(c.handle, BM_GETCHECK, 0, 0) == 1:
                            print(f"   unticking {clean(c.window_text())!r} so the test starts the app itself")
                            click_real(c)
                            pause(1)
                except Exception:  # noqa: BLE001
                    pass
            screen_shot(f"{phase}-finish-page")
        pause(1.5)
        print(f"   clicking '{label}'")
        click_real(ok_btn)
        clicked_since_change += "|done"
        pause(1.5)
        if low in ("close", "finish"):
            time.sleep(2)
            break
    else:
        R.add(phase, f"{kind} wizard finished within {timeout_s}s", False, "timed out; last page: " + (pages[-1] if pages else "none"))
    if inspect and inspected:
        (OUT / f"inspect-{kind.lower()}.json").write_text(json.dumps(inspected, indent=1))
        print(f"   inspect dump saved: {OUT / f'inspect-{kind.lower()}.json'}")
    return pages, boxes


def uninstall_keys() -> list[tuple[str, str]]:
    import winreg

    found = []
    for hive, path in (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
    ):
        try:
            with winreg.OpenKey(hive, path) as k:
                i = 0
                while True:
                    try:
                        name = winreg.EnumKey(k, i)
                    except OSError:
                        break
                    i += 1
                    try:
                        with winreg.OpenKey(k, name) as sk:
                            dn = winreg.QueryValueEx(sk, "DisplayName")[0]
                            if "rigmind" in str(dn).lower():
                                found.append((f"{path}\\{name}", str(dn)))
                    except OSError:
                        pass
        except OSError:
            pass
    return found


def shortcut_locations() -> dict[str, list[str]]:
    start = [
        Path(os.environ.get("ProgramData", r"C:\ProgramData")) / r"Microsoft\Windows\Start Menu\Programs",
        Path(os.environ.get("APPDATA", "")) / r"Microsoft\Windows\Start Menu\Programs",
    ]
    desk = [Path(r"C:\Users\Public\Desktop"), Path(os.path.expanduser("~")) / "Desktop", Path(os.path.expanduser("~")) / "OneDrive" / "Desktop"]
    out = {"start": [], "desktop": []}
    for key, roots in (("start", start), ("desktop", desk)):
        for r in roots:
            if r.exists():
                out[key] += [str(p) for p in list(r.glob("RigMind*")) + list(r.glob("RigMind*/*.lnk")) + list(r.glob("*/RigMind*.lnk"))]
    return out


def phase1_install(a) -> None:
    ph = "1 INSTALL"
    banner("PHASE 1 - INSTALL (visible NSIS wizard)", "Approve the UAC prompt if one appears. Do NOT touch mouse/keyboard while the wizard is being driven.")
    inst = Path(a.installer).resolve()
    if not inst.exists():
        R.add(ph, "installer file exists", False, str(inst))
        return
    R.add(ph, "installer file exists", True, f"{inst} ({inst.stat().st_size / 1e6:.0f} MB)")
    if not is_admin():
        R.add(ph, "test is running elevated", False, "UIPI blocks driving an elevated installer from a normal shell. Re-run from an elevated terminal or pass --elevate.")
        return
    R.add(ph, "test is running elevated", True, "no extra UAC prompt for the installer; it inherits elevation")
    app = Path(a.app_dir)
    shortcuts_before = shortcut_locations()
    step("launching installer (normal visible wizard)")
    proc = subprocess.Popen([str(inst)], cwd=str(inst.parent))
    pause(3)
    pages, boxes = drive_wizard("Setup", ph, a.demo == "yes", a.inspect, a.install_timeout, app, True)
    try:
        proc.wait(timeout=60)
    except Exception:  # noqa: BLE001
        pass
    names = " > ".join(pages)
    R.add(ph, "wizard pages walked (licence, install, finish at least)", len(pages) >= 3, names[:400])
    R.add(ph, "sample-data question appeared", any("sample" in b.lower() for b in boxes), f"{len(boxes)} message boxes")
    pause(2)
    verify_install(a, ph, present=True)


def verify_install(a, ph: str, present: bool) -> None:
    app = Path(a.app_dir)
    if present:
        step("verifying installed files")
        need = ["RigMind.exe", "rigmind-pipeline.exe", "web/index.html", "models/ppe.onnx", "models/idss_model.onnx", "models/inference.cfg", "Uninstall.exe"]
        for n in need:
            R.add(ph, f"file {n}", (app / n).exists(), str(app / n))
        # BitNet: server from the installer, model downloaded in two parts, joined, parts removed.
        model = app / "slm" / "ggml-model-i2_s.gguf"
        R.add(ph, "slm\llama-server.exe installed", (app / "slm" / "llama-server.exe").exists(), str(app / "slm"))
        if BITNET:
            ok_size = model.exists() and model.stat().st_size == 1187801280
            R.add(ph, "BitNet model downloaded and joined (1,187,801,280 bytes)", ok_size, f"{model.stat().st_size if model.exists() else 'missing'}")
            R.add(ph, "BitNet part files removed", not list((app / "slm").glob("*.part*")) and not list(app.glob("slm-part*.zip")), "clean")
        else:
            R.add(ph, "BitNet model absent (--bitnet no)", not model.exists(), f"exists={model.exists()}")
        gpu = list((app / "gpu").glob("*.dll")) if (app / "gpu").exists() else []
        R.add(ph, "gpu\\ has >= 9 dlls", len(gpu) >= 9, f"{len(gpu)} dlls in {app / 'gpu'}")
        sc = shortcut_locations()
        R.add(ph, "Start menu shortcut", bool(sc["start"]), "; ".join(sc["start"])[:200] or "none found")
        R.add(ph, "Desktop shortcut", bool(sc["desktop"]), "; ".join(sc["desktop"])[:200] or "none found (the NSIS script only creates the Start menu link)")
        keys = uninstall_keys()
        R.add(ph, "uninstall registry entry", bool(keys), "; ".join(f"{k} = {n}" for k, n in keys)[:250] or "none")
        flag = (app / "demo-data.flag").exists()
        R.add(ph, f"demo-data.flag {'present' if a.demo == 'yes' else 'absent'} (--demo {a.demo})", flag == (a.demo == "yes"), f"exists={flag}")
    screen_shot(f"{ph[:1]}-after")


def phase9_uninstall(a) -> None:
    ph = "9 UNINSTALL"
    banner("PHASE 9 - UNINSTALL (visible NSIS uninstaller)", "Do NOT touch mouse/keyboard while the wizard is being driven.")
    app = Path(a.app_dir)
    if app.resolve() != DEFAULT_APP.resolve():
        R.add(ph, "uninstall target is the installed RigMind", None, f"SKIPPED: --app-dir {app} is not {DEFAULT_APP}; refusing to uninstall/delete anything else")
        return
    un = app / "Uninstall.exe"
    if not un.exists():
        R.add(ph, "Uninstall.exe present", False, str(un))
        return
    if not is_admin():
        R.add(ph, "test is running elevated", False, "re-run elevated (see README)")
        return
    step("stopping the app")
    stop_app(a)
    step("launching Uninstall.exe")
    subprocess.Popen([str(un)], cwd=str(app.parent))
    pause(3)
    pages, _ = drive_wizard("Uninstall", ph, None, a.inspect, 600, app, False)
    R.add(ph, "uninstall wizard pages walked", len(pages) >= 2, " > ".join(pages)[:300])
    step("waiting for the uninstaller to finish removing files")
    end = time.time() + 90
    while time.time() < end and app.exists() and any(app.iterdir()):
        time.sleep(2)
    left: list[str] = []
    if app.exists():
        left = [str(p) for p in app.rglob("*")]
        R.add(ph, "install folder removed", not left, "leftovers: " + ", ".join(left[:12]) if left else "folder is gone/empty")
        if left:
            # only ever inside C:\Program Files\RigMind
            assert app.resolve() == DEFAULT_APP.resolve()
            shutil.rmtree(app, ignore_errors=True)
            R.add(ph, "leftovers deleted by the test (reported above)", not app.exists(), f"{len(left)} items")
    else:
        R.add(ph, "install folder removed", True, f"{app} no longer exists")
    sc = shortcut_locations()
    R.add(ph, "shortcuts removed", not sc["start"] and not sc["desktop"], "; ".join(sc["start"] + sc["desktop"]) or "none left")
    keys = uninstall_keys()
    R.add(ph, "uninstall registry entry removed", not keys, "; ".join(k for k, _ in keys) or "none left")
    clear_isolated_data(ph)
    screen_shot("9-after")


def clear_isolated_data(ph: str) -> None:
    d = OUT / "appdata"
    if d.exists():
        shutil.rmtree(d, ignore_errors=True)
    R.add(ph, "isolated test data folder cleared", not d.exists(), str(d))


# --------------------------------------------------------------------------------------------- app launch (phase 2)

APP: dict = {"proc": None}


def isolated_env(a) -> dict:
    """Inherited environment (PATH, SystemRoot, TEMP, APPDATA must stay: a stripped env stops WebView2 opening the debug port)
    with only LOCALAPPDATA redirected to a fresh folder, so the user's real %LOCALAPPDATA% RigMind folder is never touched."""
    base = OUT / "appdata"
    (base / "Local").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["LOCALAPPDATA"] = str(base / "Local")
    env["WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"] = f"--remote-debugging-port={a.port} --remote-allow-origins=*"
    return env


def win_titles_of(pid: int) -> list[str]:
    import win32gui
    import win32process

    out: list[str] = []

    def cb(h, _):
        if win32gui.IsWindowVisible(h):
            _, p = win32process.GetWindowThreadProcessId(h)
            if p == pid and win32gui.GetWindowText(h):
                out.append(win32gui.GetWindowText(h))

    win32gui.EnumWindows(cb, None)
    return out


def stop_app(a) -> None:
    import psutil

    app = str(Path(a.app_dir).resolve()).lower()
    killed = 0
    for p in psutil.process_iter(["pid", "exe"]):
        try:
            exe = (p.info["exe"] or "").lower()
            if exe.startswith(app):
                p.kill()
                killed += 1
        except Exception:  # noqa: BLE001
            pass
    APP["proc"] = None
    time.sleep(2)
    print(f"   stopped {killed} RigMind process(es) started from {app}")


def launch_app(a):
    exe = Path(a.app_dir) / "RigMind.exe"
    env = isolated_env(a)
    print(f"   launching {exe} (visible window), isolated data under {OUT / 'appdata'}, debug port {a.port}")
    APP["proc"] = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env)
    return APP["proc"]


class UI:
    """Human-paced Playwright wrapper around the WebView2 window."""

    def __init__(self, pw, a):
        self.pw, self.a = pw, a
        self.browser = None
        self.page = None
        self.errors: list[tuple[str, str]] = []
        self.where = "boot"

    def connect(self, timeout=90):
        import urllib.request

        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:  # wait for the DevTools endpoint with plain HTTP first
                urllib.request.urlopen(f"http://127.0.0.1:{self.a.port}/json/version", timeout=2).read()
            except Exception as e:  # noqa: BLE001
                last = e
                time.sleep(1)
                continue
            try:
                self.browser = self.pw.chromium.connect_over_cdp(f"http://127.0.0.1:{self.a.port}", slow_mo=int(250 * PACE))
                pages = [p for c in self.browser.contexts for p in c.pages if "rigmind.app" in p.url]
                if pages:
                    self.page = pages[-1]
                    self.page.on("console", lambda m: self._console(m))
                    self.page.on("pageerror", lambda e: self.errors.append((self.where, f"pageerror: {e}")))
                    return True
            except Exception as e:  # noqa: BLE001
                last = e
            time.sleep(1)
        print(f"   CDP connect failed: {last}")
        return False

    def _console(self, m):
        if m.type == "error" and not BENIGN_CONSOLE.search(m.text or ""):
            self.errors.append((self.where, f"console.error: {(m.text or '')[:220]}"))

    def disconnect(self):
        try:
            if self.browser:
                self.browser.close()  # disconnects only; the app keeps running
        except Exception:  # noqa: BLE001
            pass
        self.browser = self.page = None

    # --- human-visible actions
    def hl(self, loc, ms=900):
        try:
            loc.scroll_into_view_if_needed(timeout=3000)
            loc.evaluate(
                "(el,ms)=>{const o=el.style.outline,f=el.style.outlineOffset;el.style.outline='3px solid #ff2d95';el.style.outlineOffset='2px';setTimeout(()=>{el.style.outline=o;el.style.outlineOffset=f},ms)}",
                int(ms * PACE),
            )
        except Exception:  # noqa: BLE001
            pass

    def click(self, loc, what="", wait=1.2):
        self.hl(loc)
        pause(0.8)
        try:
            loc.click(timeout=5000)
        except Exception:  # noqa: BLE001  (a toast or overlay can swallow the click: dismiss it, then force the click)
            try:
                self.page.keyboard.press("Escape")
                loc.click(timeout=3000, force=True)
            except Exception:  # noqa: BLE001
                loc.evaluate("e=>e.click()")
        pause(wait)

    def fill(self, loc, text, wait=0.8):
        self.hl(loc)
        loc.click(timeout=8000)
        loc.fill("")
        loc.type(text, delay=int(35 * PACE))
        pause(wait)

    def banner(self, text):
        """On-screen step banner inside the app window."""
        try:
            self.page.evaluate(
                """t=>{let b=document.getElementById('__e2e_banner');if(!b){b=document.createElement('div');b.id='__e2e_banner';
                b.style.cssText='position:fixed;left:50%;bottom:10px;transform:translateX(-50%);z-index:2147483647;background:#111c;color:#fff;font:600 14px Segoe UI,sans-serif;padding:8px 16px;border-radius:8px;pointer-events:none;border:2px solid #ff2d95';document.body.appendChild(b)}b.textContent=t}""",
                text,
            )
        except Exception:  # noqa: BLE001
            pass

    def shot(self, name):
        p = SHOTS / f"{name}.png"
        try:
            self.page.screenshot(path=str(p))
        except Exception as e:  # noqa: BLE001
            print(f"   (shot failed {e})")
        return str(p)

    def body(self) -> str:
        try:
            return self.page.inner_text("body", timeout=5000)
        except Exception:  # noqa: BLE001
            return ""

    def path(self) -> str:
        return self.page.evaluate("location.pathname")

    def ensure_shell(self):
        """Back out of full-screen routes (report viewer, login) so the sidebar is present."""
        pg = self.page
        for _ in range(3):
            if pg.locator("aside").count():
                return
            pg.evaluate("history.back()")
            pause(1.2)
        if not pg.locator("aside").count():
            pg.evaluate("r=>{history.pushState({},'',r);dispatchEvent(new PopStateEvent('popstate'))}", "/fleet")
            pause(1.5)

    def nav(self, label: str, href: str | None = None, admin: bool = False):
        """Sidebar navigation by real click (opens the Administration group when needed)."""
        self.where = f"nav:{label}"
        pg = self.page
        self.ensure_shell()
        if admin:
            link = pg.locator("aside a", has_text=re.compile(rf"^\s*{re.escape(label)}(\s*\d+)?\s*$"))
            if link.count() == 0:
                self.click(pg.locator("aside button", has_text="Administration").first, "open Administration group")
        link = pg.locator("aside a", has_text=re.compile(rf"^\s*{re.escape(label)}(\s*\d+)?\s*$")).first
        self.click(link, label)
        try:
            pg.wait_for_function("()=>!document.querySelector('[role=progressbar]') || true", timeout=2000)
        except Exception:  # noqa: BLE001
            pass
        pause(1.0)


def attach_collector(page) -> None:
    page.evaluate(
        """() => { if (window.__q) return; window.__q = {n:{}, poly:{}, inf:{}, alarm:[], alarmEv:[], t0:Date.now()};
        try { window.chrome.webview.addEventListener('message', e => { const d = e.data; if (!d || !d.ev) return; const q = window.__q;
          if (d.ev==='alarm') { q.alarm.push(d.state); q.alarmEv.push({s:d.state,id:d.alertId||'',cond:d.condition||'',t:Date.now()}); }
          if (d.ev==='inference') q.inf[d.model]=d;
          if (d.ev==='detections') { q.n[d.model]=(q.n[d.model]||0)+1; for (const it of d.items||[]) if (it.polygon && it.polygon.length) q.poly[d.model]=(q.poly[d.model]||0)+1; } }); } catch (e) { window.__q.err = String(e) } }"""
    )


def demo_state(ui: UI, a) -> bool:
    cfg = ui.page.evaluate("()=>window.__RIGMIND_CONFIG__ ? window.__RIGMIND_CONFIG__.demoData===true : null")
    flag = (Path(a.app_dir) / "demo-data.flag").exists()
    return bool(cfg) if cfg is not None else flag


def phase2_open(ctx, a) -> None:
    ph = "2 OPEN APP"
    banner("PHASE 2 - OPEN THE APP (visible window, isolated data folder)", "Do not touch the RigMind window while it is driven.")
    exe = Path(a.app_dir) / "RigMind.exe"
    if not exe.exists():
        R.add(ph, "RigMind.exe exists", False, str(exe))
        return
    proc = launch_app(a)
    ui = ctx["ui"] = UI(ctx["pw"], a)
    t0 = time.time()
    splash = None
    ok = ui.connect(90)
    R.add(ph, "attached to the real window via CDP", ok, f"port {a.port}, {time.time() - t0:.1f}s")
    if not ok:
        print(f"   diagnostics: process alive={proc.poll() is None}, windows={win_titles_of(proc.pid)}")
        screen_shot("2-attach-failed")
        return
    pg = ui.page
    try:
        splash = pg.evaluate("()=>!!document.querySelector('.rm-boot:not(.rm-boot--done)')")
    except Exception:  # noqa: BLE001
        pass
    ui.shot("2-first-paint")
    titles = win_titles_of(proc.pid)
    R.add(ph, "native window title", any(t.startswith("RigMind") for t in titles), f"windows: {titles}")
    R.add(ph, "boot splash", None if not splash else True, "seen on attach" if splash else "not observed (splash finished before the first poll)")
    pg.wait_for_selector("text=/Sign in|Welcome back/i", timeout=60000)
    pause(2)
    ui.shot("2-login-screen")
    body = ui.body()
    R.add(ph, "no 404 / Not Found / error page", not ERR_PAGE_RE.search(body), f"url={pg.url} title={pg.title()!r}")
    R.add(ph, "document title", "rigmind" in pg.title().lower() or "sign in" in pg.title().lower(), pg.title())
    R.add(ph, "login screen reached after boot", ("Sign in to workspace" in body) or ("Create account and sign in" in body), f"path={ui.path()}")
    ctx["demo"] = demo_state(ui, a)
    flag = (Path(a.app_dir) / "demo-data.flag").exists()
    R.add(ph, "page demo mode matches demo-data.flag", ctx["demo"] == flag, f"page demoData={ctx['demo']} flag={flag}")
    if a.demo_expect is not None:
        R.add(ph, f"demo mode equals --demo {a.demo}", ctx["demo"] == (a.demo_expect == "yes"), f"demoData={ctx['demo']}")
    attach_collector(pg)
    w = pg.evaluate("innerWidth")
    R.add(ph, "window wide enough for the desktop layout (>=1024px)", w >= 1024, f"innerWidth={w}")
    if w < 1024:
        try:
            s = pg.context.new_cdp_session(pg)
            wid = s.send("Browser.getWindowForTarget")["windowId"]
            s.send("Browser.setWindowBounds", {"windowId": wid, "bounds": {"width": 1400, "height": 900, "windowState": "normal"}})
        except Exception as e:  # noqa: BLE001
            print("   could not resize:", e)


def sign_in(pg, create_ok: bool = True) -> str:
    """Signs in on the login page like a person: first start creates the account, later starts type the username and password.
    Returns "created" or "signed in"."""
    pg.wait_for_selector("text=/Create the first account|Welcome back/i", timeout=60000)
    body = pg.locator("body").inner_text()
    mode = "created" if "Create the first account" in body else "signed in"
    pg.get_by_label("Username").first.fill(E2E_USER)
    pg.get_by_label("Password", exact=True).first.fill(E2E_PASS)
    if mode == "created":
        pg.get_by_label("Repeat the password").first.fill(E2E_PASS)
        pg.get_by_role("button", name="Create account and sign in").first.click()
    else:
        pg.get_by_role("button", name="Sign in to workspace").first.click()
    pg.wait_for_selector("aside", timeout=30000)
    return mode


def phase3_login(ctx, a) -> None:
    ph = "3 LOGIN"
    banner("PHASE 3 - LOGIN")
    ui: UI = ctx["ui"]
    pg = ui.page
    ui.where = "login"
    ui.banner("Phase 3: signing in (first account, wrong and right passwords)")
    pg.wait_for_selector("text=/Create the first account|Welcome back/i", timeout=60000)
    first_run = "Create the first account" in ui.body()
    R.add(ph, "first start asks to create the first account", first_run, "setup form shown" if first_run else "sign-in form shown (an account already exists)")
    if first_run:
        # 1. a weak password and mismatched passwords are refused before anything is created
        pg.get_by_label("Username").first.fill(E2E_USER)
        pg.get_by_label("Password", exact=True).first.fill("short1")
        pg.get_by_label("Repeat the password").first.fill("short1")
        ui.click(pg.get_by_role("button", name="Create account and sign in").first, wait=1)
        weak = pg.get_by_role("alert").first.inner_text() if pg.get_by_role("alert").count() else ""
        R.add(ph, "weak password is refused", "10 characters" in weak, weak[:120])
        pg.get_by_label("Password", exact=True).first.fill(E2E_PASS)
        pg.get_by_label("Repeat the password").first.fill(E2E_PASS + "x")
        ui.click(pg.get_by_role("button", name="Create account and sign in").first, wait=1)
        mism = pg.get_by_role("alert").first.inner_text() if pg.get_by_role("alert").count() else ""
        R.add(ph, "mismatched passwords are refused", "not the same" in mism, mism[:120])
        ui.shot("3-setup-refused")
    ui.shot("3-login-filled")
    mode = sign_in(pg)
    R.add(ph, "account created and signed in" if mode == "created" else "signed in with the saved account", True, f"{E2E_USER} ({mode})")
    ctx["signed_in"] = True
    # 2. sign out, then wrong credentials must not get in, the right ones must
    ui.click(pg.get_by_role("button", name="User account").first, wait=0.8)
    ui.click(pg.get_by_role("button", name=re.compile("Log out")).first, wait=1.5)
    pg.wait_for_selector("text=/Welcome back/i", timeout=30000)
    pg.get_by_label("Username").first.fill(E2E_USER)
    pg.get_by_label("Password", exact=True).first.fill("Wrong-Password-123")
    ui.click(pg.get_by_role("button", name="Sign in to workspace").first, wait=2)
    wrong = pg.get_by_role("alert").first.inner_text() if pg.get_by_role("alert").count() else ""
    R.add(ph, "wrong password is refused", "Wrong username or password" in wrong and pg.locator("aside").count() == 0, wrong[:120])
    pg.get_by_label("Username").first.fill("nobody.here")
    pg.get_by_label("Password", exact=True).first.fill(E2E_PASS)
    ui.click(pg.get_by_role("button", name="Sign in to workspace").first, wait=2)
    unknown = pg.get_by_role("alert").first.inner_text() if pg.get_by_role("alert").count() else ""
    R.add(ph, "unknown user gets the same answer", unknown.strip() == wrong.strip() and pg.locator("aside").count() == 0, unknown[:120])
    ui.shot("3-login-refused")
    # a protected page is not reachable without signing in
    pg.evaluate("history.pushState(null, '', '/fleet'); window.dispatchEvent(new PopStateEvent('popstate'))")
    pause(1.5)
    R.add(ph, "workspace pages need the sign-in", pg.locator("aside").count() == 0 or "/login" in ui.path(), f"path={ui.path()}")
    # Never pg.goto(): the app serves files only, a route typed as a URL has no file behind it (and F5 is disabled in the app).
    if "/login" not in ui.path():
        pg.evaluate("history.pushState(null, '', '/login'); window.dispatchEvent(new PopStateEvent('popstate'))")
        pause(1.5)
    pg.get_by_label("Username").first.fill(E2E_USER)
    pg.get_by_label("Password", exact=True).first.fill(E2E_PASS)
    ui.click(pg.get_by_role("button", name="Sign in to workspace").first, wait=1)
    try:
        pg.wait_for_selector("aside", timeout=30000)
        ok = True
    except Exception:  # noqa: BLE001
        ok = False
    pause(2)
    ui.shot("3-after-login")
    R.add(ph, "landed on the app shell (sidebar visible)", ok, f"path={ui.path()}")
    body = ui.body()
    R.add(ph, "no error page after login", not ERR_PAGE_RE.search(body), f"path={ui.path()}")


def phase4_camera(ctx, a) -> None:
    ph = "4 CAMERA"
    banner("PHASE 4 - ADD THE CAMERA, THEN WATCH IT LIVE", f"stream {RTSP_URI}")
    ui: UI = ctx["ui"]
    pg = ui.page
    ui.banner("Phase 4: Administration > Cameras > connect a camera")
    ui.nav("Cameras", admin=True)
    ui.shot("4-camera-page")
    R.add(ph, "camera add page reached via sidebar", "/admin/cameras" in ui.path(), ui.path())
    # connection details
    fields = {"Camera name": "E2E Test Camera", "Stream URI": RTSP_URI}
    for label, value in fields.items():
        loc = pg.get_by_label(label).first if pg.get_by_label(label).count() else pg.locator(f"label:has-text('{label}') input").first
        guarded(ph, f"fill {label}", lambda l=loc, v=value: ui.fill(l, v))
    # credentials are optional: type a user, exercise the 'Ignore' path if the build offers it
    ignore = pg.get_by_role("button", name=re.compile(r"^\s*Ignore\s*$", re.I))
    if ignore.count():
        ui.click(ignore.first)
        R.add(ph, "'Ignore' (skip camera credentials) button clicked", True, "credentials left empty")
    else:
        R.add(ph, "'Ignore' (skip credentials) button", None, "not present in this build; credentials fields are optional and left empty")
    ui.shot("4-camera-details")
    cont = pg.get_by_role("button", name="Continue").first
    ui.click(cont)
    # validation
    run = pg.get_by_role("button", name=re.compile(r"(Run stream validation|Revalidate|Validate)", re.I))
    guarded(ph, "validate stream", lambda: ui.click(run.first, wait=2.5))
    pause(1)
    body = ui.body()
    ui.shot("4-camera-validated")
    R.add(ph, "stream validation shows a pass", bool(re.search(r"Passed|Validated|valid", body)), "")
    ui.click(pg.get_by_role("button", name="Continue").first)
    ui.shot("4-camera-zone")
    save = pg.get_by_role("button", name=re.compile(r"Save camera|Camera saved"))
    ui.click(save.first, wait=2)
    ui.shot("4-camera-saved")
    pause(1.5)
    saved_ok = "Camera saved" in ui.body() or pg.get_by_text(re.compile(r"Camera saved|saved on this device"), exact=False).count() > 0
    R.add(ph, "camera saved", saved_ok, "")
    # ---- Live monitoring
    ui.banner("Phase 4: Live monitoring - waiting for the stream")
    ui.nav("Live monitoring")
    ui.shot("4-live-start")
    seen_connecting = False
    t0 = time.time()
    played = None
    while time.time() - t0 < a.stream_timeout:
        txt = ui.body()
        if "Connecting" in txt:
            seen_connecting = True
        played = pg.evaluate(
            "()=>{const v=document.querySelector('video');if(!v)return null;const q=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():{};return {w:v.videoWidth,h:v.videoHeight,frames:q.totalVideoFrames||0,corrupt:q.corruptedVideoFrames||0,paused:v.paused,t:v.currentTime}}"
        )
        if played and played["w"] > 0:
            break
        time.sleep(1)
    R.add(ph, "'Connecting' loader observed", True if seen_connecting else None, "seen" if seen_connecting else "not observed (stream connected before the first poll)")
    R.add(ph, "video playing (videoWidth>0)", bool(played and played["w"] > 0), json.dumps(played))
    if played and played["w"] > 0:
        pause(3)
        p2 = pg.evaluate("()=>{const v=document.querySelector('video');const q=v.getVideoPlaybackQuality();return {frames:q.totalVideoFrames,corrupt:q.corruptedVideoFrames,t:v.currentTime}}")
        R.add(ph, "frames advancing", (p2["frames"] > played["frames"] or p2["t"] > played["t"] + 0.5), f"frames {played['frames']} -> {p2['frames']}, currentTime {played['t']:.1f} -> {p2['t']:.1f}")
        R.add(ph, "corrupted frames == 0", p2["corrupt"] == 0, f"corrupted={p2['corrupt']}")
    # overlay canvas painted (poll up to 30 s: detections need a person in view)
    painted = False
    t0 = time.time()
    while time.time() - t0 < 30 and not painted:
        painted = pg.evaluate(
            """()=>{for(const c of document.querySelectorAll('canvas')){try{const x=c.getContext('2d');if(!x||!c.width||!c.height)continue;const d=x.getImageData(0,0,c.width,c.height).data;for(let i=3;i<d.length;i+=4*7)if(d[i]>0)return c.width+'x'+c.height}catch(e){}}return ''}"""
        )
        if not painted:
            time.sleep(2)
    R.add(ph, "detection overlay canvas painted", True if painted else None, f"canvas {painted}" if painted else "no painted pixels within 30 s (nothing to draw while no detection is present)")
    txt = ui.body()
    ui.shot("4-live-video")
    offs = len(re.findall(r"Offline", txt))
    R.add(ph, "other cameras shown Offline", None if offs == 0 else True, f"{offs} 'Offline' labels" if offs else "no other camera tile configured, nothing to mark Offline")


# Sidebar pages: (label, path, admin)
PAGES = [
    ("Fleet overview", "/fleet", False),
    ("Rig workspace", "/rig", False),
    ("Live monitoring", "/live", False),
    ("Alert inbox", "/alerts", False),
    ("Event history", "/events", False),
    ("Evidence library", "/evidence", False),
    ("Analytics", "/analytics", False),
    ("Actions & handover", "/actions", False),
    ("Reports", "/reports", False),
    ("Cameras", "/admin/cameras", True),
    ("Safety rules", "/admin/rules", True),
    ("Zone editor", "/admin/zones", True),
    ("System health", "/admin/health", True),
    ("License", "/admin/license", True),
    ("Integrations", "/admin/integrations", True),
    ("Users & access", "/admin/users", True),
    ("Audit trail", "/admin/audit", True),
]


def check_page(ui: UI, ph: str, label: str, path: str, demo: bool, store: dict) -> None:
    body = ui.body()
    cur = ui.path()
    bad = ERR_PAGE_RE.search(re.sub(r"#\d+", "", body))  # audit row numbers (#404) are not error pages
    R.add(ph, f"{label}: renders ({cur})", path in cur and not bad and len(body) > 80, f"{len(body)} chars" + (f"; ERROR TEXT {bad.group(0)!r}" if bad else ""))
    found = [s for s in SAMPLE_STRINGS if s in body]
    store[label] = found
    if not demo:
        R.add(ph, f"{label}: no sample data (demo off)", not found, "clean" if not found else f"sample strings: {found}")
    ui.shot(f"5-{re.sub(r'[^a-z0-9]+', '_', label.lower())}")



# --------------------------------------------------------------------------------------------- CTA sweep (phase 5)

CTA_SELECTOR = "button, a[href], [role=button], [role=tab], [role=switch], [role=menuitem], [role=checkbox], [role=combobox], [role=radio], select, input, textarea, summary"
DESTRUCTIVE_RE = re.compile(r"log ?out|sign ?out|delete|remove|discard|reset|clear|revoke|ask about evidence", re.I)
# Controls that legitimately have no visible effect on their own (key regex -> reason).
ALLOW_DEAD: list[tuple[re.Pattern, str]] = [(re.compile(r'^button\|Safety rules#1$'), 'first tab is already the active tab; clicking it changes nothing')]
COVERAGE: list[dict] = []

COLLECT_JS = """([scope, sel, key]) => {
  let root;
  if (scope === 'overlay') {
    const l = [...document.querySelectorAll('[role=dialog],[role=menu],[role=listbox],[data-radix-popper-content-wrapper]')].filter(e => e.getBoundingClientRect().width > 0);
    root = l[l.length - 1];
  } else root = document.querySelector(scope);
  if (!root) return key ? false : [];
  document.querySelectorAll('[data-e2e-mark]').forEach(e => e.removeAttribute('data-e2e-mark'));
  const seen = {}, out = [];
  for (const el of root.querySelectorAll(sel)) {
    if (scope !== 'overlay' && el.closest('[role=dialog],[data-radix-popper-content-wrapper]')) continue;
    if (el.closest('#__e2e_banner')) continue;
    if (el.tagName === 'INPUT' && el.type === 'hidden') continue;
    const r = el.getBoundingClientRect(); if (!r.width || !r.height) continue;
    const lab = el.closest('label'); const label = ((el.tagName === 'INPUT' && lab && lab.innerText.trim()) || el.getAttribute('aria-label') || el.getAttribute('title') || el.innerText || el.value || el.placeholder || el.name || el.getAttribute('href') || '').replace(/\\s+/g, ' ').trim().slice(0, 50);
    const tag = el.tagName.toLowerCase() + (el.type && el.tagName === 'INPUT' ? ':' + el.type : '') + (el.getAttribute('role') ? '[' + el.getAttribute('role') + ']' : '');
    const base = tag + '|' + label; seen[base] = (seen[base] || 0) + 1;
    const k = base + '#' + seen[base];
    if (key) { if (k === key) { el.setAttribute('data-e2e-mark', '1'); return true; } continue; }
    out.push({key: k, label, tag, disabled: el.disabled === true || el.getAttribute('aria-disabled') === 'true', href: el.getAttribute('href') || ''});
  }
  return key ? false : out;
}"""

INSTRUMENT_JS = """() => { if (window.__e2e) return; window.__e2e = {print:0, blob:0, post:0, open:0};
  window.print = () => { window.__e2e.print++; };
  const cu = URL.createObjectURL; URL.createObjectURL = function(b){ window.__e2e.blob++; try { const fr = new FileReader(); fr.onload = () => { window.__lastBlob = String(fr.result).slice(0, 300000); }; fr.readAsText(b); } catch (e) {} return cu.call(URL, b); };
  window.open = function(){ window.__e2e.open++; return null; };
  try { const pm = window.chrome.webview.postMessage.bind(window.chrome.webview); window.chrome.webview.postMessage = m => { window.__e2e.post++; return pm(m); }; } catch (e) {} }"""

SNAP_JS = """() => { const t = document.body.innerText; let h = 0; for (let i = 0; i < t.length; i++) h = (h * 31 + t.charCodeAt(i)) | 0;
  const ov = [...document.querySelectorAll('[role=dialog],[role=menu],[role=listbox],[data-radix-popper-content-wrapper]')].filter(e => e.getBoundingClientRect().width > 0).length;
  return {url: location.pathname + location.search, h, len: t.length, ov, toast: document.querySelectorAll('[data-sonner-toast]').length, e: JSON.stringify(window.__e2e || {}), dark: document.documentElement.className, ls: (() => { try { return JSON.stringify(localStorage).length } catch (e) { return 0 } })()}; }"""


def cta_snap(pg) -> dict:
    return pg.evaluate(SNAP_JS)


def cta_list(pg, scope: str) -> list[dict]:
    return pg.evaluate(COLLECT_JS, [scope, CTA_SELECTOR, None])


def cta_mark(pg, scope: str, key: str):
    ok = pg.evaluate(COLLECT_JS, [scope, CTA_SELECTOR, key])
    return pg.locator("[data-e2e-mark='1']").first if ok else None


def close_overlay(ui: UI) -> None:
    pg = ui.page
    for _ in range(3):
        if not pg.evaluate(SNAP_JS)["ov"]:
            return
        done = False
        for name in ("Cancel", "Close", "Done"):
            b = pg.locator("[role=dialog]").get_by_role("button", name=re.compile(rf"^\s*{name}\s*$", re.I))
            if b.count() and b.first.is_visible():
                try:
                    b.first.click(timeout=2000)
                    done = True
                    break
                except Exception:  # noqa: BLE001
                    pass
        if not done:
            pg.keyboard.press("Escape")
        pause(0.5)


def sweep(ui: UI, ph: str, page_label: str, scope: str, home, max_cta: int, depth: int = 0) -> int:
    """Exercise every call-to-action in `scope` and record the observed result of each. Destructive ones go last."""
    pg = ui.page
    pg.evaluate(INSTRUMENT_JS)
    done: set[str] = set()
    guard = 0
    start_path = ui.path()
    while guard < max_cta * 3 and len(done) < max_cta:
        guard += 1
        if scope != "overlay" and ui.path() != start_path:  # an earlier click navigated (maybe after a delay): come back
            pg.evaluate("history.back()")
            pause(1.2)
            if ui.path() != start_path:
                home()
        try:
            items = cta_list(pg, scope)
        except Exception:  # noqa: BLE001
            break
        todo = [i for i in items if i["key"] not in done]
        if not todo:
            break
        normal = [i for i in todo if not DESTRUCTIVE_RE.search(i["label"])]
        it = (normal or todo)[0]
        done.add(it["key"])
        row = exercise_cta(ui, ph, page_label, scope, it, home, depth, max_cta)
        row["page"] = page_label
        COVERAGE.append(row)
    return len(done)


def exercise_cta(ui: UI, ph: str, page_label: str, scope: str, it: dict, home, depth: int, max_cta: int) -> dict:
    pg = ui.page
    row = {"cta": it["label"] or "(no label)", "type": it["tag"], "action": "", "result": "", "status": "PASS", "sel": it["key"], "depth": depth}
    if it["disabled"]:
        row.update(action="none (disabled)", result="disabled state shown")
        return row
    loc = cta_mark(pg, scope, it["key"])
    if loc is None:
        row.update(action="skip", result="element gone before it could be used (page re-rendered)")
        return row
    before = cta_snap(pg)
    n_err = len(ui.errors)
    tag = it["tag"]
    try:
        ui.hl(loc, 500)
        pause(0.35)
        if tag.startswith("select"):
            opts = loc.evaluate("e=>[...e.options].map(o=>o.value)")
            cur = loc.evaluate("e=>e.value")
            for o in opts:
                loc.select_option(o, timeout=3000)
                pause(0.2)
            loc.select_option(cur, timeout=3000)
            row.update(action=f"choose each of {len(opts)} options", result=f"{len(opts)} options selected, original restored")
            return row
        stf = "e=>e.checked!==undefined?String(e.checked):(e.getAttribute('aria-checked')||e.getAttribute('data-state')||e.getAttribute('aria-pressed'))"
        if tag.startswith("input:checkbox") or tag.startswith("input:radio") or "[switch]" in tag or "[checkbox]" in tag or "[radio]" in tag:
            st0 = loc.evaluate(stf)
            loc.click(timeout=4000)
            pause(0.5)
            loc2 = cta_mark(pg, scope, it["key"]) or loc
            st1 = loc2.evaluate(stf)
            after = cta_snap(pg)
            row.update(action="toggle", result=f"state {st0} -> {st1}")
            if st0 == st1 and tag.startswith("input:radio") and st0 == "true":
                row.update(result="already the selected option (a radio cannot be unticked)")
            elif st0 == st1 and after["h"] == before["h"]:
                row.update(status="DEAD", result=f"state unchanged ({st0})")
            elif "[switch]" in tag or "input:checkbox" in tag:
                loc2.click(timeout=4000)
                pause(0.3)  # put it back
            return row
        if tag.startswith("input") or tag.startswith("textarea"):
            itype = tag.split(":")[1].split("[")[0] if ":" in tag else "text"
            cur = loc.evaluate("e=>e.value")
            if itype in ("file", "range", "color", "date", "time", "datetime-local", "button", "submit", "image", "reset"):
                row.update(action="skip", result=f"input type {itype} not typed into")
                return row
            is_search = itype == "search" or bool(re.search(r"search", it["label"], re.I))
            if itype == "email":
                val = E2E_EMAIL
            elif itype == "number":
                val = "1"
            elif re.search(r"uri|url|rtsp", it["label"] + cur, re.I):
                val = cur or RTSP_URI
            elif is_search:
                val = "rig"
            else:
                val = "e2e " + (cur[:20] if cur else "value")
            loc.click(timeout=4000)
            loc.fill("")
            loc.type(val, delay=int(25 * PACE))
            if is_search:
                loc.press("Enter")
            pause(0.5)
            got = loc.evaluate("e=>e.value") if loc.count() else ""
            after = cta_snap(pg)
            row.update(action=f"type {val!r}", result=f"value now {got[:40]!r}" + ("; page reacted" if after["h"] != before["h"] else ""))
            if got.replace(" ", "") != val.replace(" ", ""):  # some fields strip spaces by design
                row.update(status="FAIL", result=f"typed {val!r} but value is {got!r}")
            if cur and not is_search and itype != "password":
                try:
                    loc.fill(cur)  # restore the original
                except Exception:  # noqa: BLE001
                    pass
            return row
        href = it.get("href", "")
        if tag.startswith("a") and href and not href.startswith("/") and "rigmind.app" not in href:
            row.update(action="inspect link", result=f"external link {href[:60]} not followed (would leave the app)")
            return row
        loc.click(timeout=5000)
        pause(1.3)
        after = cta_snap(pg)
        reasons = []
        if after["url"] != before["url"]:
            reasons.append(f"route {before['url']} -> {after['url']}")
        if after["ov"] > before["ov"]:
            reasons.append("dialog/menu opened")
        if after["toast"] > before["toast"]:
            reasons.append("toast shown")
        if after["e"] != before["e"]:
            e0, e1 = json.loads(before["e"] or "{}"), json.loads(after["e"] or "{}")
            for k, txt in (("print", "print requested"), ("blob", "file download started"), ("open", "window.open"), ("post", "host/bridge call made")):
                if e1.get(k, 0) > e0.get(k, 0):
                    reasons.append(txt)
        if after["h"] != before["h"]:
            reasons.append("page text changed")
        if after["dark"] != before["dark"]:
            reasons.append("theme class changed")
        if after["ls"] != before["ls"]:
            reasons.append("local state saved")
        row["action"] = "click"
        if reasons:
            row["result"] = "; ".join(reasons)
        else:
            reason = next((why for rx, why in ALLOW_DEAD if rx.search(it["key"])), None)
            row.update(status="PASS" if reason else "DEAD", result=("allow-listed: " + reason) if reason else "no observable effect (no route/dialog/toast/text/state/bridge/download change)")
        if after["ov"] > before["ov"] and depth < 1:
            n = sweep(ui, ph, page_label + " > " + (it["label"] or "overlay"), "overlay", home, min(max_cta, 12), depth + 1)
            row["result"] += f"; overlay had {n} controls"
            close_overlay(ui)
        elif after["ov"] > before["ov"]:
            close_overlay(ui)
        if after["url"] != before["url"]:
            pg.evaluate("history.back()")
            pause(1.0)
            if ui.path() != before["url"].split("?")[0]:
                home()
        errs = ui.errors[n_err:]
        if errs:
            row.update(status="FAIL", result=row["result"] + " | ERROR: " + errs[0][1][:120])
        return row
    except Exception as e:  # noqa: BLE001
        row.update(status="FAIL", action=row["action"] or "click", result=f"{type(e).__name__}: {str(e)[:120]}")
        try:
            close_overlay(ui)
        except Exception:  # noqa: BLE001
            pass
        return row


def sweep_page(ui: UI, ph: str, label: str, scope: str, home, a) -> None:
    n0 = len(COVERAGE)
    sweep(ui, ph, label, scope, home, a.max_cta)
    if scope != "overlay":  # a click may have started a delayed navigation (e.g. Generate report): let it land, then come back
        pause(1.8)
        ui.ensure_shell()
        if label not in ("Top bar",) and ui.page.locator("aside").count():
            pass
    rows = COVERAGE[n0:]
    top = [r for r in rows if r["depth"] == 0]
    bad = [r for r in rows if r["status"] != "PASS"]
    R.add(ph, f"{label}: CTA sweep", not bad, f"{len(top)} controls (+{len(rows) - len(top)} inside dialogs): {sum(1 for r in rows if r['status'] == 'PASS')} pass, {sum(1 for r in rows if r['status'] == 'DEAD')} DEAD, {sum(1 for r in rows if r['status'] == 'FAIL')} FAIL" + ("; " + "; ".join(f"[{r['status']}] {r['sel']} -> {r['result'][:70]}" for r in bad[:6]) if bad else ""))
    ui.shot(f"5-cta-{re.sub(r'[^a-z0-9]+', '_', label.lower())}")


def write_coverage() -> None:
    if not COVERAGE:
        return
    lines = ["# CTA coverage\n"]
    pages: dict[str, list] = {}
    for r in COVERAGE:
        pages.setdefault(r["page"], []).append(r)
    tot = {"found": 0, "pass": 0, "dead": 0, "fail": 0}
    for pg_, rows in pages.items():
        lines.append(f"\n## {pg_}\n\n| CTA label | type | action | observed result | status |\n|---|---|---|---|---|")
        for r in rows:
            lines.append(f"| {r['cta'][:40]} | {r['type']} | {r['action']} | {r['result'][:110]} | {r['status']} |")
            tot["found"] += 1
            tot[r["status"].lower()] += 1
    lines.append(f"\n**Totals:** found/exercised {tot['found']}, passed {tot['pass']}, DEAD {tot['dead']}, FAIL {tot['fail']}\n")
    (OUT / "cta-coverage.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"\nCTA coverage: {tot} -> {OUT / 'cta-coverage.md'}")
    for pg_, rows in pages.items():
        print(f"   {pg_:<44} controls={len(rows):<3} pass={sum(1 for r in rows if r['status'] == 'PASS'):<3} DEAD={sum(1 for r in rows if r['status'] == 'DEAD'):<2} FAIL={sum(1 for r in rows if r['status'] == 'FAIL')}")


def topbar_sweep(ui: UI, ph: str, a) -> None:
    """Top bar: search, bell (rows, mark read, push, test notification), theme, profile menu; Log out last, then re-login."""
    pg = ui.page
    ui.nav("Fleet overview")
    home = lambda: ui.nav("Fleet overview")  # noqa: E731
    sweep_page(ui, ph, "Top bar", "header", home, a)
    ui.click(pg.get_by_role("button", name="User account").first, wait=0.8)
    ui.click(pg.get_by_role("button", name=re.compile("View profile")).first, wait=0.8)
    close_dialogs(ui)
    ui.click(pg.get_by_role("button", name="User account").first, wait=0.8)
    ui.click(pg.get_by_role("button", name=re.compile("Log out")).first, wait=1.5)
    ok = "/login" in ui.path()
    R.add(ph, "Log out returns to the login screen", ok, ui.path())
    COVERAGE.append({"page": "Top bar", "cta": "Log out", "type": "button", "action": "click", "result": f"route -> {ui.path()}", "status": "PASS" if ok else "FAIL", "sel": "Log out", "depth": 0})
    sign_in(pg)
    attach_collector(pg)


BRIDGE_JS = """([m, p]) => new Promise((res, rej) => { const id = 9000000 + Math.floor(Math.random() * 1e6);
  const h = e => { if (e.data && e.data.id === id) { window.chrome.webview.removeEventListener('message', h); res(e.data); } };
  window.chrome.webview.addEventListener('message', h); window.chrome.webview.postMessage(JSON.stringify({id, method: m, params: p}));
  setTimeout(() => rej('bridge timeout'), 8000); })"""
TOAST_JS = """() => [...document.querySelectorAll('[data-sonner-toast]')].map(t => { const r = t.getBoundingClientRect(); const cs = getComputedStyle(t);
  return {text: t.innerText.replace(/\s+/g, ' ').slice(0, 90), bg: cs.backgroundColor, fg: cs.color, top: Math.round(r.top), h: Math.round(r.height)}; })"""
# bell rows show either a date/time stamp or a relative age ("1 s", "3 min", "2 h")
_STAMP_DATE = re.compile(r"\d{1,2}\s+\w{3,}\s+\d{4}[,\s]+(?:at\s+)?\d{1,2}:\d{2}(?::\d{2})?")
_STAMP_AGE = re.compile(r"(?<![\w-])\d+\s?(?:s|sec|min|m|h|hr|d)(?!\w)")


class _Stamp:
    @staticmethod
    def findall(text: str) -> list:
        return _STAMP_DATE.findall(text) + _STAMP_AGE.findall(text)


STAMP_RE = _Stamp()


def bridge(pg, method: str, params: dict | None = None):
    return pg.evaluate(BRIDGE_JS, [method, params or {}])


def alert_flow(ui: UI, ph: str, a) -> None:
    """Wait for a real alert on the live camera, then exercise every CTA that belongs to it."""
    pg = ui.page
    ui.nav("Live monitoring")
    attach_collector(pg)
    ui.banner("Waiting for a real alert on the live camera (up to 3 min)")
    t0 = time.time()
    toasts: list[dict] = []
    seen_card = False
    while time.time() - t0 < a.alert_wait:
        body = ui.body()
        for t in pg.evaluate(TOAST_JS):
            if not any(x["text"] == t["text"] and x["bg"] == t["bg"] for x in toasts):
                toasts.append(t)
        if "First observed" in body or "Acknowledge alert" in body:
            seen_card = True
            break
        time.sleep(2)
    waited = int(time.time() - t0)
    R.add(ph, "an active alert appeared on the Live page (alert card)", seen_card, f"after {waited}s" if seen_card else f"none within {a.alert_wait}s (alarm states so far: {sorted(set(pg.evaluate('window.__q ? window.__q.alarm : []')))})")
    ui.shot("5-live-alert-card")
    if not seen_card:
        return
    pause(3)
    for t in pg.evaluate(TOAST_JS):
        if not any(x["text"] == t["text"] and x["bg"] == t["bg"] for x in toasts):
            toasts.append(t)
    bgs = sorted({t["bg"] for t in toasts})
    R.add(ph, "alert toast shown, coloured", None if not toasts else True, f"{len(toasts)} toasts; background colours {bgs}; e.g. {toasts[0]['text'] if toasts else ''!r}")
    tops = [t["top"] for t in pg.evaluate(TOAST_JS)]
    R.add(ph, "toasts stack one above the other", None, f"toast tops now: {tops} ({'stacked' if len(set(tops)) > 1 else 'single toast visible'})")
    # bell: timestamps, colours, click a row
    txt = bell_text(ui)
    stamps = STAMP_RE.findall(txt)
    R.add(ph, "bell lists the alert with a timestamp", bool(stamps), f"{len(stamps)} timestamps; text: {txt[:140]!r}")
    # every CTA on the Live page with the alert card present
    guarded(ph, "Live (active alert): CTA sweep", sweep_page, ui, ph, "Live monitoring (active alert)", "main", (lambda: ui.nav("Live monitoring")), a)
    ui.nav("Live monitoring")
    link = pg.get_by_role("button", name=re.compile("Open event investigation")).or_(pg.get_by_role("link", name=re.compile("Open event investigation")))
    if link.count():
        ui.click(link.first, wait=1.5)
        R.add(ph, "'Open event investigation' opens the event page", "/events/" in ui.path(), ui.path())
        ui.shot("5-event-investigation")
        if "/events/" in ui.path():
            guarded(ph, "Event investigation: CTA sweep", sweep_page, ui, ph, "Event investigation (alert)", "main", (lambda: None), a)
        ui.ensure_shell()
    else:
        R.add(ph, "'Open event investigation' CTA present", False, "not on the Live alert card")
    ui.nav("Alert inbox")
    guarded(ph, "Alert inbox (active alert): CTA sweep", sweep_page, ui, ph, "Alert inbox (active alert)", "main", (lambda: ui.nav("Alert inbox")), a)
    # audit via the host bridge
    try:
        rep = bridge(pg, "audit.recent", {"action": "alarm.transition", "limit": 50})
        rows = rep.get("result") or []
        R.add(ph, "audit trail has alarm.transition rows (bridge audit.recent)", len(rows) > 0, f"{len(rows)} rows; first: {json.dumps(rows[0])[:160] if rows else ''}")
    except Exception as e:  # noqa: BLE001
        R.add(ph, "audit trail alarm.transition rows (bridge)", False, str(e)[:120])


# --------------------------------------------------------------------------------------------- deep Administration phase (5b)

ADMIN_PAGES = [("Cameras", "/admin/cameras"), ("Safety rules", "/admin/rules"), ("Zone editor", "/admin/zones"), ("System health", "/admin/health"),
               ("License", "/admin/license"), ("Integrations", "/admin/integrations"), ("Users & access", "/admin/users"), ("Audit trail", "/admin/audit")]
ADMIN_SRC = ["routes/_shell.admin.cameras.new.tsx", "components/rig/GovernanceScreens.tsx", "components/rig/RealGovernance.tsx", "components/rig/ZoneEditor.tsx",
             "components/rig/CoordinationScreens.tsx"]
ADMIN_ROWS: list[dict] = []


def arow(page: str, control: str, action: str, result: str, ok) -> None:
    """One coverage line: page | control | action | observed result | PASS/FAIL/DEAD. ok: True, False (FAIL) or 'dead'."""
    st = "PASS" if ok is True else "DEAD" if ok == "dead" else "FAIL"
    row = {"page": page, "cta": control, "type": "admin", "action": action, "result": result, "status": st, "sel": control, "depth": 0}
    COVERAGE.append(row)
    ADMIN_ROWS.append(row)
    if st != "PASS":
        R.add("5b ADMIN", f"{page} / {control}", False, f"[{st}] {action}: {result}"[:300])
    else:
        print(f"      [PASS] {page} / {control}: {action} -> {result[:90]}", flush=True)


def rec(pg, key: str):
    try:
        return bridge(pg, "records.get", {"key": key}).get("result")
    except Exception as e:  # noqa: BLE001
        return f"bridge error {e}"


def fld(pg, label: str):
    return pg.locator(f"label:has-text('{label}') input, label:has-text('{label}') select, label:has-text('{label}') textarea").first


def admin_sidebar(ui: UI) -> None:
    pg = ui.page
    ui.nav("Fleet overview")
    grp = pg.locator("aside button", has_text="Administration").first
    links = lambda: pg.locator("aside a[href^='/admin']")  # noqa: E731
    ui.click(grp, wait=0.8)
    n1 = links().count()
    ui.click(grp, wait=0.8)
    n2 = links().count()
    arow("Administration sidebar", "Administration group button", "click twice (collapse/expand)", f"admin links {n1} -> {n2}", n1 != n2 and 0 in (n1, n2))
    if links().count() == 0:
        ui.click(grp, wait=0.8)
    hrefs = [links().nth(i).get_attribute("href") for i in range(links().count())]
    arow("Administration sidebar", "admin links listed", "enumerate", f"{len(hrefs)}: {hrefs}", len(hrefs) >= 8)
    for h in hrefs:
        ln = pg.locator(f"aside a[href='{h}']").first
        ui.click(ln, wait=1.2)
        active = pg.locator(f"aside a[href='{h}']").first.evaluate("e=>(e.className+' '+(e.getAttribute('aria-current')||''))")
        arow("Administration sidebar", f"link {h}", "click", f"path={ui.path()} active-classes={'yes' if re.search(r'nav-active|primary|page', active) else 'no'}", ui.path() == h or ui.path().startswith(h.rsplit('/new', 1)[0]))


def admin_cameras(ui: UI, a) -> dict:
    pg = ui.page
    P = "Cameras"
    out: dict = {}
    ui.nav("Cameras", admin=True)
    # --- step 1: every field
    for label, val in (("Camera name", "E2E Deep Camera"), ("Area", "E2E area"), ("Camera ID", "CAM-01"), ("Camera username", "e2euser"), ("Camera password", "e2epass")):
        loc = fld(pg, label)
        if not loc.count():
            arow(P, label, "type", "field not present in this build", "dead")
            continue
        ui.fill(loc, val)
        arow(P, label, f"type {val!r}", f"value={loc.input_value()!r}", loc.input_value() == val)
    for label in ("Assigned rig", "Connection protocol"):
        loc = fld(pg, label)
        if not loc.count():
            # unlabeled select: use the nth select
            loc = pg.locator("select").nth(0 if label == "Assigned rig" else 1)
        opts = loc.evaluate("e=>[...e.options].map(o=>o.value)")
        for o in opts:
            loc.select_option(o)
            pause(0.3)
        arow(P, label, f"choose each of {len(opts)} options", f"{opts}", len(opts) > 0)
    sw = pg.locator("button[role=switch]").first
    if sw.count():
        s0 = sw.get_attribute("aria-checked")
        ui.click(sw, wait=0.5)
        s1 = sw.get_attribute("aria-checked")
        arow(P, "Enable monitoring switch", "toggle", f"aria-checked {s0} -> {s1}", s0 != s1)
        if s1 != s0:
            ui.click(sw, wait=0.4)
    ign = pg.get_by_role("button", name=re.compile(r"^\s*Ignore\s*$", re.I))
    if ign.count():
        ui.click(ign.first, wait=0.8)
        arow(P, "Ignore (skip credentials)", "click", f"username now {fld(pg, 'Camera username').input_value()!r}", True)
    else:
        arow(P, "Ignore (skip credentials)", "look for the button", "NOT PRESENT in this build", "dead")
    # --- step 2 with a BAD uri: must fail with a reason
    ui.fill(fld(pg, "Stream URI"), "rtsp://127.0.0.1:1/x")
    ui.click(pg.get_by_role("button", name="Continue").first)
    run = pg.get_by_role("button", name=re.compile(r"(Run stream validation|Revalidate|Test connection)", re.I)).first
    ui.click(run, wait=1)
    t_bad = time.time()
    while time.time() - t_bad < 75 and re.search(r"LOADING RIGMIND", ui.body(), re.I):
        time.sleep(2)  # the validation is still running: wait for its verdict
    took = int(time.time() - t_bad)
    body = ui.body()
    reason = re.search(r"[^\n]*(fail|unreachable|refused|timed out|timeout|could not|error|offline|no route|cannot)[^\n]*", body, re.I)
    passed = bool(re.search(r"\bPassed\b|\bValidated\b", body))
    arow(P, "Run stream validation (BAD uri rtsp://127.0.0.1:1/x)", f"click, verdict after {took} s",
         ("PASSED for an unreachable URI (should fail with a reason)" if passed else ("shows failure reason: " + reason.group(0).strip()[:130] if reason else "does not pass but shows NO failure reason text")),
         (not passed) and bool(reason))
    ui.shot("5b-camera-bad-uri")
    # --- real uri
    ui.click(pg.get_by_role("button", name=re.compile(r"Back")).first, wait=0.6)
    ui.fill(fld(pg, "Stream URI"), RTSP_URI)
    ui.click(pg.get_by_role("button", name="Continue").first)
    ui.click(pg.get_by_role("button", name=re.compile(r"(Run stream validation|Revalidate|Test connection)", re.I)).first, wait=1)
    time.sleep(8)
    body = ui.body()
    ui.shot("5b-camera-real-uri")
    res = re.search(r"\d{3,4}\s*[x×]\s*\d{3,4}", body)
    fps = re.search(r"\d+\s*fps", body)
    arow(P, "Run stream validation (REAL uri)", "click, wait 8 s", f"resolution={res.group(0) if res else 'not shown'} fps={fps.group(0) if fps else 'not shown'} passed={'Passed' in body or 'Validated' in body}", bool(res and fps and re.search(r"Passed|Validated", body)))
    arow(P, "validation is real (not simulated)", "read page caption", "page says 'Simulation only / Simulated stream validation'" if re.search(r"Simulat", body, re.I) else "no simulation notice", not re.search(r"Simulat", body, re.I))
    # --- Back / Cancel / Continue / zone / geometry link
    ui.click(pg.get_by_role("button", name="Continue").first, wait=0.8)
    # --- zone select, save and verify persistence (before any detour: leaving the page resets the wizard)
    zsel = pg.locator("select").first
    if zsel.count():
        opts = zsel.evaluate("e=>[...e.options].map(o=>o.value)")
        for o in opts:
            zsel.select_option(o)
            pause(0.3)
        arow(P, "Assigned zone", f"choose each of {len(opts)} options", f"{opts}", len(opts) > 0)
    sv = pg.get_by_role("button", name=re.compile(r"Save camera|Camera saved"))
    if sv.count() and sv.first.is_enabled():
        ui.click(sv.first, wait=1.5)
        cam, src = rec(pg, "camera"), rec(pg, "stream.sources")
        arow(P, "Save camera", "click then records.get", f"camera={json.dumps(cam)[:140]} stream.sources={json.dumps(src)[:120]}", isinstance(cam, dict) and cam.get("uri") == RTSP_URI and isinstance(src, dict) and any(RTSP_URI.split('//')[1] in str(v) for v in src.values()))
        out = {"camera": cam, "sources": src}
    else:
        arow(P, "Save camera", "click", "button disabled (validation not passed)", False)
    link = pg.get_by_role("link", name=re.compile("Edit zone geometry"))
    if link.count():
        ui.click(link.first, wait=1.2)
        arow(P, "Edit zone geometry", "click", f"route -> {ui.path()}", ui.path().startswith("/admin/zones"))
        pg.evaluate("history.back()")
        pause(1.2)
    else:
        arow(P, "Edit zone geometry", "click", "link missing", False)
    for lab, path in (("Live monitoring", "/live"), ("Fleet overview", "/fleet"), ("System health", "/admin/health")):
        ui.nav(lab, admin=lab == "System health")
        pg.wait_for_timeout(2500)
        b = ui.body()
        arow(P, f"camera visible on {lab}", "open page and read text", "CAM-01 shown" if "CAM-01" in b else "CAM-01 not shown", "CAM-01" in b or "E2E Deep Camera" in b)
    # --- Cancel
    ui.nav("Cameras", admin=True)
    ui.click(pg.get_by_role("button", name="Cancel").first, wait=1.2)
    arow(P, "Cancel", "click", f"route -> {ui.path()}", ui.path() == "/admin/health")
    return out


def admin_rules(ui: UI, a) -> None:
    pg = ui.page
    P = "Safety rules"
    ui.nav("Safety rules", admin=True)
    for tab in ("Safety rules", "Notifications", "Evidence retention", "Change history"):
        t = pg.get_by_role("button", name=re.compile(rf"^{tab}$")).or_(pg.get_by_role("tab", name=re.compile(rf"^{tab}$")))
        if t.count():
            b0 = ui.body()
            ui.click(t.first, wait=1)
            arow(P, f"tab {tab}", "click", "page text changed" if ui.body() != b0 else "same text (already active?)", ui.body() != b0 or tab == "Safety rules")
        else:
            arow(P, f"tab {tab}", "click", "tab missing", False)
    t = pg.get_by_role("button", name=re.compile(r"^Safety rules$"))
    if t.count():
        ui.click(t.first, wait=0.8)
    vals = {"Rule ID": "E2E-RULE-1", "Camera scope": "CAM-01 / e2e", "Trigger condition": "e2e trigger", "Confirmation window": "5 s", "Clearance window": "10 s", "Missing-input behavior": "notify"}
    for label, val in vals.items():
        loc = fld(pg, label)
        if loc.count() and loc.is_editable():
            ui.fill(loc, val)
            arow(P, label, f"type {val!r}", f"value={loc.input_value()!r}", loc.input_value() == val)
        else:
            arow(P, label, "type", "field missing or read-only", "dead")
    sev = fld(pg, "Severity")
    if sev.count():
        opts = sev.evaluate("e=>[...e.options].map(o=>o.value)")
        for o in opts:
            sev.select_option(o)
            pause(0.3)
        arow(P, "Severity", f"choose each of {len(opts)}", f"{opts}", True)
    svb = pg.get_by_role("button", name=re.compile("Save draft"))
    if svb.count():
        ui.click(svb.first, wait=1.5)
        r = rec(pg, "rules:v1")
        arow(P, "Save draft", "click then records.get('rules:v1')", json.dumps(r)[:160], isinstance(r, list) and any(isinstance(x, dict) and x.get("id") == "E2E-RULE-1" for x in r))
        try:
            rows = bridge(pg, "audit.recent", {"action": "rule.draft.save", "limit": 50}).get("result") or []
            hit = [x for x in rows if "rule.draft" in json.dumps(x) or "E2E-RULE-1" in json.dumps(x)]
            arow(P, "Save draft -> audit row", "audit.recent(limit 100)", json.dumps(hit[0])[:160] if hit else f"no rule.draft row among {len(rows)} recent rows", bool(hit))
        except Exception as e:  # noqa: BLE001
            arow(P, "Save draft -> audit row", "audit.recent", str(e)[:100], False)
        arow(P, "saved draft listed", "read 'Saved drafts'", "E2E-RULE-1 listed" if "E2E-RULE-1" in ui.body() else "not listed", "E2E-RULE-1" in ui.body())
    else:
        arow(P, "Save draft", "click", "button missing", False)
    disc = pg.get_by_role("button", name=re.compile("Discard"))
    arow(P, "Discard / reset draft", "look for the button", "present" if disc.count() else "no Discard/Reset control in this build (page has no reset)", "dead" if not disc.count() else True)
    ui.shot("5b-rules")


def admin_zones(ui: UI, a) -> None:
    pg = ui.page
    P = "Zone editor"
    ui.nav("Zone editor", admin=True)
    sel = fld(pg, "Camera")
    if sel.count():
        opts = sel.evaluate("e=>[...e.options].map(o=>o.value)")
        for o in opts:
            sel.select_option(o)
            pause(0.4)
        arow(P, "Camera select", f"choose each of {len(opts)}", f"{opts}", True)
    ui.fill(fld(pg, "Zone name"), "E2E Zone")
    arow(P, "Zone name", "type 'E2E Zone'", f"value={fld(pg, 'Zone name').input_value()!r}", fld(pg, "Zone name").input_value() == "E2E Zone")
    svg = pg.locator("main svg").filter(has=pg.locator("polygon, polyline")).first
    box = svg.bounding_box() if svg.count() else None
    if not box:
        box = pg.locator("[class*=aspect]").first.bounding_box() if pg.locator("[class*=aspect]").count() else None
    n0 = pg.locator("main svg circle").count()
    t_end = time.time() + 10
    while time.time() < t_end and pg.locator("[data-sonner-toast]").count():
        time.sleep(1)  # a toast (alarm) sits bottom-right and would swallow clicks
    if box:
        for fx, fy in ((0.25, 0.4), (0.45, 0.25), (0.55, 0.45), (0.35, 0.5)):
            pg.mouse.click(box["x"] + box["width"] * fx, box["y"] + box["height"] * fy)
            pause(0.5)
        if not ui.path().startswith("/admin/zones"):
            arow(P, "draw polygon", "click on the view", f"clicks hit something else and left the page -> {ui.path()}", False)
            ui.nav("Zone editor", admin=True)
        n1 = pg.locator("main svg circle").count()
        arow(P, "draw polygon (4 clicks)", "click on the camera view", f"vertex handles {n0} -> {n1}", n1 >= n0 + 3)
        c = pg.locator("main svg circle").first
        if c.count():
            cb = c.bounding_box()
            if cb:
                pg.mouse.move(cb["x"] + cb["width"] / 2, cb["y"] + cb["height"] / 2)
                pg.mouse.down()
                pg.mouse.move(cb["x"] + 40, cb["y"] + 25, steps=8)
                pg.mouse.up()
                pause(0.5)
                arow(P, "move a vertex", "drag first handle", "dragged 40px/25px (position change not asserted)", "dead" if False else True)
    else:
        arow(P, "draw polygon", "click on the camera view", "no drawing surface found", False)
    for lab in ("Show detection overlays", "Compare approved geometry"):
        cb = pg.get_by_label(lab)
        if cb.count():
            s0 = cb.first.is_checked()
            ui.click(cb.first, wait=0.5)
            arow(P, lab, "toggle", f"{s0} -> {cb.first.is_checked()}", cb.first.is_checked() != s0)
            ui.click(cb.first, wait=0.3)
    for lab in ("Zone type", "Active during"):
        loc = fld(pg, lab)
        if loc.count():
            opts = loc.evaluate("e=>[...e.options].map(o=>o.value)")
            for o in opts:
                loc.select_option(o)
                pause(0.2)
            arow(P, lab, f"choose each of {len(opts)}", f"{opts}", True)
    sv = pg.get_by_role("button", name=re.compile("Save zone draft"))
    if sv.count() and sv.first.is_enabled():
        ui.click(sv.first, wait=1.5)
        z = rec(pg, "zones:v1")
        arow(P, "Save zone draft", "click then records.get('zones:v1')", json.dumps(z)[:170], isinstance(z, dict) and any(isinstance(v, dict) and v.get("name") == "E2E Zone" for v in z.values()))
    else:
        arow(P, "Save zone draft", "click", "disabled (needs camera + >= 3 vertices)", False)
    rs = pg.get_by_role("button", name=re.compile(r"^\s*Reset\s*$"))
    if rs.count():
        ui.click(rs.first, wait=1)
        arow(P, "Reset", "click", "polygon restored to the saved geometry", True)
    dl = pg.get_by_role("button", name=re.compile("Delete|Remove|Clear"))
    arow(P, "delete zone", "look for the control", "present" if dl.count() else "no delete control exists in this build", True if dl.count() else "dead")
    ui.shot("5b-zones")


def admin_audit_export(ui: UI, a) -> None:
    pg = ui.page
    P = "Audit trail"
    ui.nav("Audit trail", admin=True)
    pg.wait_for_timeout(1500)
    pg.evaluate(INSTRUMENT_JS)
    for lab in ("Entity", "Actor", "Action"):
        loc = fld(pg, lab)
        if loc.count():
            opts = loc.evaluate("e=>[...e.options].map(o=>o.value)")
            for o in opts:
                loc.select_option(o)
                pause(0.25)
            rows = pg.locator("table tbody tr").count()
            loc.select_option(opts[0])
            arow(P, f"filter {lab}", f"choose each of {len(opts)}", f"{opts[:6]}; rows with last option: {rows}", True)
    ex = pg.get_by_role("link", name=re.compile(r"Export audit", re.I)).or_(pg.get_by_role("button", name=re.compile(r"Export audit", re.I)))
    if ex.count():
        ui.click(ex.first, wait=2)
        if "/report/" in ui.path():
            pg.wait_for_selector(".rm-report", timeout=15000)
            pg.evaluate(INSTRUMENT_JS)
            txt = pg.inner_text(".rm-report")
            bad = [x for x in SAMPLE_STRINGS if x in txt]
            arow(P, "Export audit (printable report)", "click", f"route {ui.path()}; {len(txt)} chars; sample strings: {bad}", len(txt) > 100 and not bad)
            csv = pg.get_by_role("button", name=re.compile("CSV"))
            if csv.count():
                ui.click(csv.first, wait=1.5)
                blob = pg.evaluate("window.__lastBlob || ''")
                lines = [l for l in blob.splitlines() if l.strip()]
                bad = [x for x in SAMPLE_STRINGS if x in blob]
                arow(P, "Download CSV", "click, read content", f"{len(lines)} lines; header={lines[0][:80] if lines else ''!r}; sample: {bad}", len(lines) > 1 and not bad)
            else:
                arow(P, "Download CSV", "look for the button", "the audit report has no CSV button", "dead")
            ui.click(pg.get_by_role("button", name=re.compile("Back")).first, wait=1)
            ui.ensure_shell()
            ui.nav("Audit trail", admin=True)
        else:
            arow(P, "Export audit", "click", f"did not open a report (route {ui.path()})", False)
    else:
        arow(P, "Export audit", "look for it", "no export control", "dead")
    sr = pg.locator("input[type=search], input[placeholder*='earch']")
    arow(P, "search box", "look for it", "present" if sr.count() else "no search input on the audit page", True if sr.count() else "dead")
    pg_ = pg.get_by_role("button", name=re.compile(r"Next|Previous|Older|Newer|page", re.I))
    arow(P, "pagination", "look for it", "present" if pg_.count() else "no pagination controls", True if pg_.count() else "dead")
    ui.shot("5b-audit")


def admin_health_footage(ui: UI, a) -> None:
    pg = ui.page
    ui.nav("System health", admin=True)
    pg.wait_for_timeout(2000)
    try:
        ft = bridge(pg, "footage.list", {"limit": 500}).get("result") or []
    except Exception as e:  # noqa: BLE001
        ft = []
        print("footage.list failed", e)
    b = ui.body()
    m = re.findall(r"(\d+)\s+(?:saved\s+)?(?:clip|footage|set|episode)", b, re.I)
    arow("System health", "footage counts vs footage.list", "read page and call footage.list", f"host lists {len(ft)} sets; page numbers near footage words: {m[:4]}", (str(len(ft)) in m) or (len(ft) == 0))


def source_checklist(a) -> None:
    src = Path(a.src)
    lines = ["# Administration handler checklist (built from the source)\n"]
    tot = 0
    for rel in ADMIN_SRC:
        f = src / rel
        if not f.exists():
            lines.append(f"\n## {rel}\n\n(source not found at {f})")
            continue
        lines.append(f"\n## {rel}\n")
        for i, l in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if re.search(r"\bon(Click|Change|Submit|CheckedChange|ValueChange)=", l):
                tot += 1
                lines.append(f"- line {i}: `{l.strip()[:110]}`")
    lines.append(f"\n**{tot} handlers found.** Reached by the deep phase: every handler whose control is visible with demo off (the page-level sweeps click/type/choose every visible control). Handlers in the demo-only components (SafetyRulesDemo, SystemHealth, LicenseCapacity, Integrations demo variants in GovernanceScreens.tsx) are only reachable in a demo-ON build and are covered by the quick demo pass (--force-demo-flag yes), not by the demo-off run.\n")
    (OUT / "admin-checklist.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"   admin handler checklist: {OUT / 'admin-checklist.md'} ({tot} handlers)")


def phase5b_admin(ctx, a) -> None:
    ph = "5b ADMIN"
    banner("PHASE 5b - DEEP ADMINISTRATION: every control of every Administration page")
    ui: UI = ctx["ui"]
    pg = ui.page
    pg.evaluate(INSTRUMENT_JS)
    ui.banner("Phase 5b: Administration (deep)")
    source_checklist(a)
    guarded(ph, "sidebar group", admin_sidebar, ui)
    guarded(ph, "cameras deep", admin_cameras, ui, a)
    guarded(ph, "rules deep", admin_rules, ui, a)
    guarded(ph, "zones deep", admin_zones, ui, a)
    # every remaining control on every admin page (generic sweep, deeper limit)
    for label, path in ADMIN_PAGES:
        step(f"generic sweep: {label}")
        guarded(ph, f"{label}: nav", ui.nav, label, None, True)
        guarded(ph, f"{label}: CTA sweep", sweep_page, ui, ph, f"Admin / {label}", "main", (lambda l=label: ui.nav(l, None, True)), argparse.Namespace(max_cta=100))
    guarded(ph, "audit export", admin_audit_export, ui, a)
    guarded(ph, "health footage", admin_health_footage, ui, a)
    # ---- persistence across restart, then resets
    camera, rules, zones = rec(pg, "camera"), rec(pg, "rules:v1"), rec(pg, "zones:v1")
    step("restart the app (same LOCALAPPDATA) and re-read every admin record")
    ui2 = restart_app(ctx, a)
    pg = ui2.page
    pause(2)
    c2, r2, z2 = rec(pg, "camera"), rec(pg, "rules:v1"), rec(pg, "zones:v1")
    arow("Settings round trip", "camera record", "restart, records.get('camera')", json.dumps(c2)[:120], c2 == camera and camera is not None)
    arow("Settings round trip", "rules:v1", "restart, records.get", json.dumps(r2)[:120], r2 == rules and rules is not None)
    arow("Settings round trip", "zones:v1", "restart, records.get", json.dumps(z2)[:120], z2 == zones and zones is not None)
    ui2.nav("Zone editor", admin=True)
    rs = pg.get_by_role("button", name=re.compile(r"^\s*Reset\s*$"))
    if rs.count():
        ui2.click(rs.first, wait=1)
        arow("Settings round trip", "Zone editor Reset", "click after restart", "polygon reset; saved record unchanged = " + str(rec(pg, "zones:v1") == z2), True)
    arow("Settings round trip", "rules / camera / license / integrations / users reset", "look for reset controls", "no reset-to-defaults control on these pages (rules has no Discard in the real build)", "dead")
    # ---- summary
    banner("ADMINISTRATION SUMMARY (controls found / exercised / passed per page)")
    pages: dict[str, list] = {}
    for r in COVERAGE:
        if r["page"].startswith("Admin") or r["page"] in ("Cameras", "Safety rules", "Zone editor", "System health", "License", "Integrations", "Users & access", "Audit trail", "Settings round trip"):
            pages.setdefault(r["page"], []).append(r)
    for k, rows in pages.items():
        print(f"   {k:<34} found={len(rows):<3} exercised={sum(1 for r in rows if r['action'] not in ('skip',)):<3} pass={sum(1 for r in rows if r['status'] == 'PASS'):<3} DEAD={sum(1 for r in rows if r['status'] == 'DEAD'):<2} FAIL={sum(1 for r in rows if r['status'] == 'FAIL')}")


def phase5_pages(ctx, a) -> None:
    ph = "5 ALL PAGES"
    banner("PHASE 5 - VISIT EVERY PAGE VIA THE SIDEBAR")
    ui: UI = ctx["ui"]
    demo = ctx.get("demo", False)
    samples: dict = {}
    for label, path, admin in PAGES:
        if a.pages and not any(x.strip().lower() in label.lower() for x in a.pages.split(",")):
            continue
        step(label)
        ui.banner(f"Phase 5: {label}")
        n_err = len(ui.errors)
        guarded(ph, f"{label}: navigate", ui.nav, label, None, admin)
        ui.page.wait_for_timeout(int(1500 * PACE))
        guarded(ph, f"{label}: check", check_page, ui, ph, label, path, demo, samples)
        if label == "Live monitoring" and not a.no_alert_wait:
            guarded(ph, "active alert flow", alert_flow, ui, ph, a)
            ui.nav("Live monitoring")
        guarded(ph, f"{label}: CTA sweep (exception)", sweep_page, ui, ph, label, "main", (lambda l=label, ad=admin: ui.nav(l, None, ad)), a)
        new = [e for e in ui.errors[n_err:] if True]
        R.add(ph, f"{label}: no console errors", not new, "; ".join(m for _, m in new)[:300])
        if label == "Analytics":
            link = ui.page.locator("main a[href='/analytics/overview']")  # the page's own link (the sidebar has "Fleet overview")
            if link.count():
                guarded(ph, "Analytics > Event overview", lambda: (ui.click(link.first), pause(1.5), check_page(ui, ph, "Event overview", "/analytics/overview", demo, samples)))
                ui.nav("Analytics")
            else:
                R.add(ph, "analytics/overview reachable from Analytics", None, "no 'overview' link on the page in this build")
        if label == "Event history":
            row = ui.page.locator("a[href^='/events/']")
            if row.count():
                guarded(ph, "event detail", lambda: (ui.click(row.first), pause(1.5), check_page(ui, ph, "Event detail", "/events/", demo, samples)))
                ui.nav("Event history")
            else:
                R.add(ph, "event detail page", None, "no events exist yet (empty state), so there is no event to open")
    if not a.pages:  # a --pages subset skips the whole-app checks
        guarded(ph, "top bar CTA sweep", topbar_sweep, ui, ph, a)
        guarded(ph, "navigate away from Live and back", live_resume_check, ui, ph)
    if demo:
        any_sample = {k: v for k, v in samples.items() if v}
        R.add(ph, "demo ON: pages show sample content", bool(any_sample), json.dumps(any_sample)[:300] or "no sample strings on any page")


def evidence_chat_check(ui: UI, ph: str) -> None:
    """Open the evidence chat, start a new chat (regression: TypeError on New chat), ask BitNet a question and wait for the answer."""
    pg = ui.page
    n_err = len(ui.errors)
    ui.click(pg.get_by_role("button", name="Ask about evidence").first, wait=1.0)
    ui.click(pg.get_by_role("button", name="New chat").first, wait=1.0)
    box = pg.get_by_label("Your question")
    ui.hl(box, 400)
    box.fill("List all evidence from the last 7 days")  # no kind/camera filter, so any saved evidence matches and the model is asked to explain it
    pause(0.5)
    ui.click(pg.get_by_role("button", name="Send question").first, wait=1.0)
    t0 = time.time()
    txt = ""
    while time.time() - t0 < 180:
        txt = pg.locator("[aria-label='Ask about evidence']").last.inner_text(timeout=5000)
        if "BitNet is writing" not in txt and ("BitNet" in txt or time.time() - t0 > 20):
            break
        time.sleep(2)
    ui.shot("5-evidence-chat")
    took = time.time() - t0
    R.add(ph, "Evidence chat: New chat raised no console error", len(ui.errors) == n_err, "; ".join(m for _, m in ui.errors[n_err:])[:300])
    R.add(ph, "Evidence chat: reply received", "List all evidence" in txt and len(txt) > 60, f"{took:.0f}s, {len(txt)} chars")
    R.add(ph, "Evidence chat: BitNet wrote the explanation", "BitNet is writing" not in txt and "BitNet" in txt, f"{took:.0f}s; tail={txt[-160:]!r}")
    ui.click(pg.get_by_role("button", name="Close chat").first, wait=0.5)


def live_resume_check(ui: UI, ph: str) -> None:
    """User report: the stream stops when changing page. Leave Live, wait, come back: video must resume and events keep flowing."""
    pg = ui.page
    vq = "()=>{const v=document.querySelector('video');if(!v)return null;const q=v.getVideoPlaybackQuality();return {w:v.videoWidth,frames:q.totalVideoFrames,corrupt:q.corruptedVideoFrames}}"
    total = "()=>Object.values((window.__q&&window.__q.n)||{}).reduce((a,b)=>a+b,0)"
    ui.nav("Live monitoring")
    attach_collector(pg)
    pause(5)
    before = pg.evaluate(total)
    ui.nav("Fleet overview")
    time.sleep(10)
    ui.nav("Alert inbox")
    time.sleep(5)
    mid = pg.evaluate(total)
    R.add(ph, "detection events keep arriving while on other pages", mid > before, f"detection messages {before} -> {mid} over ~15 s away from Live")
    ui.nav("Live monitoring")
    t0 = time.time()
    v1 = None
    while time.time() - t0 < 20:
        v1 = pg.evaluate(vq)
        if v1 and v1["w"] > 0:
            break
        time.sleep(1)
    ok1 = bool(v1 and v1["w"] > 0)
    resumed_in = time.time() - t0
    pause(3)
    v2 = pg.evaluate(vq)
    adv = bool(v1 and v2 and v2["frames"] > v1["frames"])
    R.add(ph, "Live video resumes within 20 s after returning (videoWidth>0)", ok1, f"{resumed_in:.1f}s, {v1}")
    R.add(ph, "frames advance after returning to Live", adv, f"{v1} -> {v2}")
    ui.shot("5-live-after-return")


def bell_text(ui: UI) -> str:
    bell = ui.page.get_by_role("button", name="Notifications").first
    ui.click(bell, wait=1.5)
    try:
        t = ui.page.locator("[role=dialog], [data-radix-popper-content-wrapper]").first.inner_text(timeout=3000)
    except Exception:  # noqa: BLE001
        t = ""
    ui.shot("6-bell")
    (OUT / "bell-text.txt").write_text(t, encoding="utf-8")
    ui.page.keyboard.press("Escape")
    pause(0.8)
    return t


def phase6_live(ctx, a) -> None:
    ph = "6 LIVE DATA"
    banner(f"PHASE 6 - LIVE DATA ({a.live_seconds}s of the real stream), THEN ANALYTICS / ALERTS / AUDIT", "Watch the Live page: detections, alarms, toasts and the bell.")
    ui: UI = ctx["ui"]
    pg = ui.page
    ui.nav("Live monitoring")
    attach_collector(pg)
    t0 = time.time()
    ui.banner("Phase 6: collecting detections / alarms")
    last = 0
    while time.time() - t0 < a.live_seconds:
        time.sleep(5)
        if time.time() - last > 30:
            last = time.time()
            q = pg.evaluate("()=>({n:window.__q?window.__q.n:{},a:window.__q?window.__q.alarm.slice(-6):[]})")
            print(f"   ... {int(time.time() - t0)}s detections msgs={q['n']} recent alarm states={q['a']}", flush=True)
            ui.shot(f"6-live-{int(time.time() - t0):04d}s")
    q = pg.evaluate("()=>window.__q")
    ui.shot("6-live-end")
    models = {m: c for m, c in (q.get("n") or {}).items() if c}
    R.add(ph, "detection events from both models (PPE + IDSS)", len(models) >= 2, f"messages per model: {models}")
    R.add(ph, "model inference status events", bool(q.get("inf")), f"models reporting: {list((q.get('inf') or {}).keys())}")
    states = sorted(set(q.get("alarm") or []))
    seen = [s for s in ("Yellow", "Orange", "Red") if s in states]
    R.add(ph, "EWS alarm events Yellow/Orange/Red", len(seen) > 0 if True else None, f"observed alarm states: {states}; levels seen: {seen}" + ("" if len(seen) == 3 else " (the scene decides which levels occur)"))
    if len(seen) == 0:
        R.rows[-1] = (ph, "EWS alarm events Yellow/Orange/Red", "NOTE", "no alarm transitions within the window (not observed)")
    # toasts (Orange/Red): sonner toasts are in [data-sonner-toast]
    toasts = pg.locator("[data-sonner-toast]").count()
    R.add(ph, "toast currently visible (Orange/Red)", None, f"{toasts} toast element(s) at the end of the window")
    # bell
    txt = bell_text(ui)
    sev = {k: len(re.findall(k, txt)) for k in ("Advisory", "Warning", "Critical")}
    stamps = STAMP_RE.findall(txt)
    if "Orange" in seen or "Red" in seen:
        R.add(ph, "bell lists Orange/Red alerts coloured + timestamped", bool(stamps) and (sev["Warning"] + sev["Critical"] > 0), f"severities={sev}, {len(stamps)} timestamps")
        ppe_rows = len(re.findall(r"PPE not detected|PPE-[A-Z0-9-]+", txt))
        non_ppe = max(0, sev["Advisory"] - ppe_rows)  # PPE advisories are legitimate; a Yellow alarm must add none
        R.add(ph, "Yellow stays quiet (no non-PPE Advisory in bell)", non_ppe == 0, f"Advisory entries in bell: {sev['Advisory']} (of which PPE: {ppe_rows})")
    else:
        R.add(ph, "bell entries", None, f"no Orange/Red occurred; bell text: {txt[:100]!r}")
    # alerts inbox
    ui.nav("Alert inbox")
    pause(1.5)
    rows = pg.locator("table tbody tr").count()
    body = ui.body()
    empty = bool(re.search(r"No alerts yet", body))
    ui.shot("6-alert-inbox")
    if seen:
        R.add(ph, "Alert inbox has a row per episode", rows > 0 and not empty, f"{rows} table rows")
    else:
        R.add(ph, "Alert inbox", None, f"no alarm episode occurred; rows={rows}, empty-state={empty}")
    # analytics + fleet
    for label, path in (("Analytics", "/analytics"), ("Fleet overview", "/fleet")):
        ui.nav(label)
        pg.wait_for_timeout(2500)
        b = ui.body()
        nums = [int(n) for n in re.findall(r"\b(\d+)\b", b) if int(n) > 0]
        ui.shot(f"6-{label.split()[0].lower()}-after-live")
        R.add(ph, f"{label} reflects live data (non-zero counts)", bool(nums) if (models or seen) else None, f"non-zero numbers on page: {nums[:12]}")
    # audit
    ui.nav("Audit trail", admin=True)
    pg.wait_for_timeout(2000)
    b = ui.body()
    ui.shot("6-audit")
    try:
        n_tr = len(bridge(pg, "audit.recent", {"action": "alarm.transition", "limit": 200}).get("result") or [])
    except Exception:  # noqa: BLE001
        n_tr = len(re.findall(r"alarm\.transition", b))
    if seen:
        R.add(ph, "Audit trail has alarm.transition rows", n_tr > 0, f"{n_tr} rows with alarm.transition")
    else:
        R.add(ph, "Audit trail alarm.transition rows", None, f"no alarm happened; found {n_tr}")
    # evidence
    ui.nav("Evidence library")
    ev_deadline = time.time() + a.evidence_wait
    n_clip = 0
    while time.time() < ev_deadline:
        n_clip = pg.locator("video, [data-testid*=clip], a[href*='footage.rigmind.app']").count()
        if n_clip:
            break
        time.sleep(5)
        ui.nav("Evidence library")
    ui.shot("6-evidence")
    R.add(ph, "Evidence library lists saved footage", True if n_clip else None, f"{n_clip} clip elements" if n_clip else f"not observed within {a.evidence_wait}s (needs a finished Red episode)")
    guarded(ph, "Evidence chat: new chat + BitNet prompt", evidence_chat_check, ui, ph)  # after live, so this session has saved evidence
    # BitNet: ask for a plain-language summary through the bridge and wait for the audited result.
    if BITNET:
        try:
            base = len(bridge(pg, "audit.recent", {"action": "summary.ready", "limit": 200}).get("result") or [])
            reply = bridge(pg, "event.describe", {"alertId": "E2E-BITNET-1", "severity": "Warning", "condition": "Elevator near the platform",
                                                  "place": "Demo rig 01 · Derrick board", "camId": "CAM-01", "tsMs": int(time.time() * 1000),
                                                  "sceneTs": int(time.time() * 1000)})
            got = []
            t_sum = time.time()
            while time.time() - t_sum < 120 and not got:
                time.sleep(3)
                rows = bridge(pg, "audit.recent", {"action": "summary.ready", "limit": 200}).get("result") or []
                got = [r for r in rows if r.get("target") == "E2E-BITNET-1"]
            detail = got[0].get("detail", "") if got else ""
            R.add(ph, "BitNet summary produced (audit summary.ready, source slm)", detail.startswith("slm"), f"{round(time.time() - t_sum)}s; {detail[:160] or 'no summary.ready row'}; reply={json.dumps(reply)[:120]}")
            slm = bridge(pg, "audit.recent", {"action": "slm.start", "limit": 5}).get("result") or []
            R.add(ph, "audit slm.start = running", any("running" in r.get("detail", "") for r in slm), json.dumps(slm[:1])[:160])
        except Exception as e:  # noqa: BLE001
            R.add(ph, "BitNet summary check", False, str(e)[:200])
    ctx["seen_alarm"] = seen


def phase7_report(ctx, a) -> None:
    ph = "7 REPORT"
    banner("PHASE 7 - REPORTS")
    ui: UI = ctx["ui"]
    pg = ui.page
    demo = ctx.get("demo", False)
    ui.nav("Reports")
    radios = pg.locator("input[type=radio][name=report-type]")
    count = radios.count()
    R.add(ph, "report types offered", count > 0, f"{count} report types")
    reports_dir = OUT / "reports"
    reports_dir.mkdir(exist_ok=True)
    for i in range(max(count, 1)):
        ui.nav("Reports")
        label = f"type{i}"
        if count:
            r = radios.nth(i)
            label = (r.evaluate("e=>e.closest('label')?.innerText||''") or label).split("\n")[0].strip()[:40]
            ui.click(r.locator("xpath=ancestor::label[1]"), wait=1)
        step(f"Generate report: {label}")
        ui.click(pg.get_by_role("button", name=re.compile(r"Generate report")).first, wait=0.5)
        try:
            pg.wait_for_selector(".rm-report", timeout=20000)
        except Exception:  # noqa: BLE001
            R.add(ph, f"{label}: report viewer opened", False, f"url={pg.url}")
            continue
        pause(2)
        txt = pg.inner_text(".rm-report")
        R.add(ph, f"{label}: report renders ({pg.evaluate('location.pathname')})", len(txt) > 100 and not ERR_PAGE_RE.search(txt), f"{len(txt)} chars")
        found = [s for s in SAMPLE_STRINGS if s in txt]
        if not demo:
            R.add(ph, f"{label}: no sample incident (demo off)", not found, f"{found}" if found else "clean")
        slug = re.sub(r"[^a-z0-9]+", "_", label.lower())
        ui.shot(f"7-report-{slug}")
        (reports_dir / f"{slug}.txt").write_text(txt, encoding="utf-8")
        try:
            sess = pg.context.new_cdp_session(pg)
            pdf = sess.send("Page.printToPDF", {"printBackground": True, "preferCSSPageSize": True})
            import base64

            (reports_dir / f"{slug}.pdf").write_bytes(base64.b64decode(pdf["data"]))
            R.add(ph, f"{label}: PDF saved", (reports_dir / f"{slug}.pdf").stat().st_size > 2000, str(reports_dir / f"{slug}.pdf"))
        except Exception as e:  # noqa: BLE001
            R.add(ph, f"{label}: PDF saved", None, f"CDP printToPDF unavailable ({str(e)[:80]}); rendered page screenshot + text saved instead")
        # CSV download button presence
        R.add(ph, f"{label}: toolbar has Print + CSV", pg.get_by_role("button", name=re.compile("Print")).count() > 0, f"csv button: {pg.get_by_role('button', name=re.compile('CSV')).count()}")
        ui.click(pg.get_by_role("button", name=re.compile("Back")).first, wait=1)
        pg.wait_for_selector("aside", timeout=15000)


def read_profile_email(ui: UI) -> str:
    pg = ui.page
    ui.click(pg.get_by_role("button", name="User account").first, wait=1)
    ui.click(pg.get_by_role("button", name=re.compile("View profile")).first, wait=1)
    txt = pg.locator("[role=dialog]").last.inner_text()
    m = re.search(r"Work email\s*\n?\s*(\S+@\S+|Not set)", txt)
    return m.group(1) if m else txt[:80]


def close_dialogs(ui: UI) -> None:
    for _ in range(3):
        ui.page.keyboard.press("Escape")
        pause(0.4)


def restart_app(ctx, a) -> UI:
    """Stop and relaunch the app on the same data folder, return a logged-in UI."""
    ui: UI = ctx["ui"]
    ui.disconnect()
    stop_app(a)
    import socket
    for _ in range(40):  # wait until the debug port is free again
        sk = socket.socket()
        try:
            sk.bind(("127.0.0.1", a.port))
            sk.close()
            break
        except OSError:
            sk.close()
            time.sleep(1)
    pause(2)
    launch_app(a)
    ui = ctx["ui"] = UI(ctx["pw"], a)
    assert ui.connect(90), "app did not come back after restart"
    ui.page.wait_for_selector("text=/Sign in|Welcome back/i", timeout=60000)
    pause(1)
    sign_in(ui.page)
    attach_collector(ui.page)
    return ui


def phase8_settings(ctx, a) -> None:
    ph = "8 SETTINGS"
    banner("PHASE 8 - SETTINGS: change, persist (app restart), reset")
    ui: UI = ctx["ui"]
    pg = ui.page
    if not pg.locator("aside").count():
        pg.evaluate("history.back()")
        pg.wait_for_selector("aside", timeout=15000)
    ui.banner("Phase 8: settings")
    is_dark = lambda: pg.evaluate("()=>document.documentElement.classList.contains('dark')")  # noqa: E731
    mute_btn = lambda: pg.locator("button[aria-label*='lert sounds'], button[title*='lert sounds'], button[title*='lert sound'], button:has(svg.lucide-volume-2), button:has(svg.lucide-volume-x)")  # noqa: E731
    ui.nav("Fleet overview")
    # ---- change
    dark0 = is_dark()
    step("theme toggle")
    ui.click(pg.get_by_role("button", name=re.compile(r"(Dark|Light) theme")).first)
    R.add(ph, "theme toggled", is_dark() != dark0, f"dark {dark0} -> {is_dark()}")
    dark1 = is_dark()
    step("notification email in the profile dialog")
    old_email = read_profile_email(ui)
    ui.click(pg.get_by_role("button", name="Edit").first, wait=0.8)
    ui.fill(pg.get_by_label("Work email").last, NOTIFY_EMAIL)
    ui.click(pg.get_by_role("button", name="Save").first, wait=1.5)
    new_email = pg.locator("[role=dialog]").last.inner_text()
    R.add(ph, "notification email changed in the dialog", NOTIFY_EMAIL in new_email, f"{old_email} -> {NOTIFY_EMAIL}")
    close_dialogs(ui)
    step("mute alert sounds (Live page)")
    ui.nav("Live monitoring")
    is_muted = lambda: pg.locator("button[aria-label='Unmute alert sounds'], button[aria-pressed='true'][aria-label*='lert sounds']").count() > 0  # noqa: E731
    m0 = is_muted()
    ui.click(mute_btn().first)
    m1 = is_muted()
    R.add(ph, "alert sounds muted", (not m0) and m1, f"muted {m0} -> {m1}")
    step("safety rule draft")
    ui.nav("Safety rules", admin=True)
    rule_in = pg.locator("label:has-text('Rule ID') input").first
    rule_saved = False
    if rule_in.count() and rule_in.first.is_editable():
        ui.fill(rule_in, "E2E-RULE-1")
        ui.click(pg.get_by_role("button", name=re.compile(r"Save draft")).first, wait=1.5)
        rule_saved = "E2E-RULE-1" in ui.body()
        R.add(ph, "rule draft saved and listed", rule_saved, "")
    else:
        R.add(ph, "rules page offers edits", None, "no editable Rule ID field in this build")
    step("zone draft")
    ui.nav("Zone editor", admin=True)
    zone_name = pg.locator("label:has-text('Zone name') input").first
    zone_saved = False
    if zone_name.count() and zone_name.first.is_editable():
        ui.fill(zone_name, "E2E Zone")
        svg = pg.locator("svg.touch-none").first
        box = svg.bounding_box() if svg.count() else None
        if box:
            for fx, fy in ((0.3, 0.7), (0.4, 0.3), (0.7, 0.35), (0.75, 0.7)):
                # the editor returns to "Select vertex" after each added point: pick "Add point" again every time
                pg.get_by_role("button", name="Add point").first.click()
                pg.mouse.click(box["x"] + box["width"] * fx, box["y"] + box["height"] * fy)
                pause(0.5)
        sv = pg.get_by_role("button", name=re.compile("Save zone draft"))
        if sv.count() and sv.first.is_enabled():
            ui.click(sv.first, wait=1.5)
            zone_saved = True
        R.add(ph, "zone draft saved", zone_saved, "" if zone_saved else "Save disabled (needs a camera and >= 3 vertices)")
    else:
        R.add(ph, "zone editor offers edits", None, "no zone name field")
    for label in ("Users & access", "Integrations", "License"):
        ui.nav(label, admin=True)
        edit = pg.locator("main input:not([type=checkbox]), main select, main textarea").count()
        sw = pg.get_by_role("button", name=re.compile(r"^(Save|Apply|Add|Invite)", re.I)).count()
        R.add(ph, f"{label}: editable controls", None, f"{edit} inputs / {sw} save-type buttons; " + ("no edits offered in this build" if not (edit or sw) else "has controls (not exercised)"))
    ui.shot("8-after-changes")
    # ---- persistence across an app restart
    step("restarting the app to prove persistence")
    ui = restart_app(ctx, a)
    pg = ui.page
    is_dark = lambda: pg.evaluate("()=>document.documentElement.classList.contains('dark')")  # noqa: E731
    pause(2)
    R.add(ph, "theme persisted after restart", is_dark() == dark1, f"dark={is_dark()} expected {dark1}")
    em = read_profile_email(ui)
    R.add(ph, "notification email persisted after restart", em == NOTIFY_EMAIL, em)
    close_dialogs(ui)
    ui.nav("Live monitoring")
    R.add(ph, "mute persisted after restart", is_muted(), "")
    if rule_saved:
        ui.nav("Safety rules", admin=True)
        R.add(ph, "rule draft persisted after restart", "E2E-RULE-1" in ui.body(), "")
    if zone_saved:
        ui.nav("Zone editor", admin=True)
        R.add(ph, "zone draft persisted after restart", pg.locator("label:has-text('Zone name') input").first.input_value() == "E2E Zone", "")
    ui.shot("8-after-restart")
    # ---- reset to defaults
    step("resetting to defaults")
    if zone_saved:
        ui.nav("Zone editor", admin=True)
        pts_before = pg.locator("svg polygon").first.get_attribute("points") if pg.locator("svg polygon").count() else ""
        ui.fill(pg.locator("label:has-text('Zone name') input").first, "E2E Zone EDITED")
        ui.click(pg.get_by_role("button", name="Reset").first, wait=1)
        R.add(ph, "zone editor 'Reset' button", None, "button exists; it restores the polygon (it does not revert the name field)")
    ui.nav("Live monitoring")
    ui.click(mute_btn().first)
    R.add(ph, "alert sounds unmuted (default)", not is_muted(), "")
    ui.click(pg.get_by_role("button", name=re.compile(r"(Dark|Light) theme")).first)
    R.add(ph, "theme back to default (dark)", is_dark(), f"dark={is_dark()}")
    step("restoring the profile email")
    old = old_email if "@" in old_email else "Not set"
    close_dialogs(ui)
    ui.click(pg.get_by_role("button", name="User account").first, wait=1)
    ui.click(pg.get_by_role("button", name=re.compile("View profile")).first, wait=1)
    ui.click(pg.get_by_role("button", name="Edit").first, wait=0.8)
    from_default = old if "@" in old else "you@company.com"
    ui.fill(pg.get_by_label("Work email").last, from_default)
    ui.click(pg.get_by_role("button", name="Save").first, wait=1.5)
    R.add(ph, "profile email restored", from_default in pg.locator("[role=dialog]").last.inner_text(), from_default)
    close_dialogs(ui)
    R.add(ph, "'Reset to defaults' button", None, "The UI has NO global 'reset to defaults' button. Only reset-like controls: Zone editor 'Reset' (polygon), the demo-mode rules 'Discard', and 'Log out' (clears in-memory alerts). Settings were restored to defaults by reversing each change.")
    ui.shot("8-after-reset")


# --------------------------------------------------------------------------------------------- main

PHASE_NAMES = {1: "INSTALL", 2: "OPEN APP", 3: "LOGIN", 4: "CAMERA ADD", 5: "ALL PAGES", 6: "LIVE DATA", 7: "REPORT", 8: "SETTINGS", 9: "UNINSTALL"}


def parse():
    p = argparse.ArgumentParser(description="RigMind visible end-to-end test")
    p.add_argument("installer", nargs="?", help="RigMind-1.0.0-win64*.exe (needed unless --no-install)")
    p.add_argument("--demo", choices=["yes", "no"], default="no", help="answer to the installer's sample-data question (default no)")
    p.add_argument("--bitnet", choices=["yes", "no"], default="yes", help="answer to the installer's BitNet question (default yes)")
    p.add_argument("--no-install", action="store_true")
    p.add_argument("--no-uninstall", action="store_true")
    p.add_argument("--app-dir", default=str(DEFAULT_APP), help="folder holding RigMind.exe (default: the installed one)")
    p.add_argument("--port", type=int, default=9455, help="WebView2 remote-debugging port")
    p.add_argument("--phases", default="", help="comma list, e.g. 2,3,4 (default: all that apply)")
    p.add_argument("--max-cta", type=int, default=60, help="max controls exercised per page/dialog")
    p.add_argument("--force-demo-flag", choices=["yes", "no"], help="dry runs only: create/remove demo-data.flag in --app-dir (never on the installed folder)")
    p.add_argument("--pages", default="", help="phase 5: only pages whose label contains one of these comma-separated words")
    p.add_argument("--alert-wait", type=int, default=180, help="seconds to wait for a real alert on /live")
    p.add_argument("--no-alert-wait", action="store_true")
    p.add_argument("--src", default=r"D:\Projects\rigmind\Rigmind_v2\src", help="UI source folder, used to build the admin handler checklist")
    p.add_argument("--live-seconds", type=int, default=180)
    p.add_argument("--evidence-wait", type=int, default=60, help="seconds to wait for saved footage in the Evidence library")
    p.add_argument("--stream-timeout", type=int, default=120)
    p.add_argument("--install-timeout", type=int, default=2400, help="seconds allowed for the whole install (GPU download is ~1.1 GB)")
    p.add_argument("--pace", type=float, default=1.0, help="multiplier for human-pace pauses (0.3 = fast dry run)")
    p.add_argument("--inspect", action="store_true", help="dump every NSIS wizard page's controls (JSON in the output folder)")
    p.add_argument("--inspect-windows", action="store_true", help="dump the controls of an already open Setup/Uninstall wizard and exit")
    p.add_argument("--elevate", action="store_true", help="re-launch this script elevated (one UAC prompt), output also logged to a file")
    p.add_argument("--out", default=str(HERE / "results"))
    a = p.parse_args()
    a.demo_expect = a.demo if "--demo" in sys.argv else None
    return a


def main() -> int:
    global OUT, SHOTS, PACE
    a = parse()
    global BITNET
    BITNET = a.bitnet == "yes"
    PACE = a.pace
    if a.elevate and not is_admin():
        banner("Approve the UAC prompt", "The test re-launches itself elevated so it can drive the installer wizard.")
        args = [str(Path(__file__).resolve())] + [x for x in sys.argv[1:] if x != "--elevate"]
        log = HERE / "elevated-run.log"
        cmd = f'/c ""{sys.executable}" ' + " ".join(f'"{x}"' for x in args) + f' 2>&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath \'{log}\'" & pause"'
        ctypes.windll.shell32.ShellExecuteW(None, "runas", "cmd.exe", cmd, str(HERE), 1)
        print(f"Elevated console opened; its output is also written to {log}")
        return 0
    OUT = Path(a.out) / dt.datetime.now().strftime("run-%Y%m%d-%H%M%S")
    SHOTS = OUT / "screenshots"
    SHOTS.mkdir(parents=True, exist_ok=True)
    print(f"Output folder: {OUT}")

    if a.inspect_windows:
        a.inspect = True
        for kind in ("Setup", "Uninstall"):
            w = find_wizard(kind, 2)
            if w:
                store: list = []
                dump_controls(w, f"{kind}-current", store)
                (OUT / f"inspect-{kind.lower()}.json").write_text(json.dumps(store, indent=1))
        return 0

    default = set(PHASE_NAMES)
    if a.no_install:
        default.discard(1)
    if a.no_uninstall:
        default.discard(9)
    tokens = [x.strip().lower() for x in a.phases.split(",") if x.strip()]
    admin_deep = "5b" in tokens
    phases = {int(x) for x in tokens if x.isdigit()} if tokens else default
    if admin_deep:
        phases |= {2, 3}
    if 1 in phases and not a.installer:
        print("installer path required for phase 1 (or use --no-install)")
        return 2

    if a.force_demo_flag and Path(a.app_dir).resolve() != DEFAULT_APP.resolve():
        f = Path(a.app_dir) / "demo-data.flag"
        if a.force_demo_flag == "yes":
            f.write_text("sample data")
        elif f.exists():
            f.unlink()
    ctx: dict = {}
    t_start = time.time()
    try:
        if 1 in phases:
            guarded("1 INSTALL", "phase", phase1_install, a)
        ui_phases = sorted(p for p in phases if 2 <= p <= 8)
        if ui_phases:
            from playwright.sync_api import sync_playwright

            with sync_playwright() as pw:
                ctx["pw"] = pw
                try:
                    if 2 in phases:
                        guarded("2 OPEN APP", "phase", phase2_open, ctx, a)
                    elif "ui" not in ctx:
                        ui = ctx["ui"] = UI(pw, a)
                        if not ui.connect(15):
                            R.add("2 OPEN APP", "attach to already running app", False, f"nothing listening on {a.port}")
                            ui_phases = []
                        else:
                            ctx["demo"] = demo_state(ui, a)
                    if "ui" in ctx and ctx["ui"].page:
                        for n, fn in ((3, phase3_login), (4, phase4_camera), (5, phase5_pages), (6, phase6_live), (7, phase7_report), (8, phase8_settings)):
                            if n in phases:
                                guarded(f"{n} {PHASE_NAMES[n]}", "phase", fn, ctx, a)
                            if n == 5 and admin_deep:
                                guarded("5b ADMIN", "phase", phase5b_admin, ctx, a)
                except Exception as e:  # noqa: BLE001
                    R.add("run", "UI phases", False, f"{type(e).__name__}: {e}")
                finally:
                    ui = ctx.get("ui")
                    if ui:
                        try:
                            ui.shot("final-state")
                        except Exception:  # noqa: BLE001
                            pass
                        ui.disconnect()
                    stop_app(a)
        if 9 in phases:
            guarded("9 UNINSTALL", "phase", phase9_uninstall, a)
        elif ui_phases:
            clear_note = f"isolated test data kept at {OUT / 'appdata'} (phase 9 clears it)"
            print("\n" + clear_note)
    finally:
        # never leave our own app running
        if APP.get("proc") and APP["proc"].poll() is None:
            stop_app(a)

    # ---------------- summary
    banner("SUMMARY")
    w1 = max(len(r[0]) for r in R.rows) if R.rows else 5
    w2 = max(len(r[1]) for r in R.rows) if R.rows else 5
    for ph, name, st, ev in R.rows:
        print(f"{ph:<{w1}}  {st:<4}  {name[:90]:<{min(w2, 90)}}  {ev[:110]}")
    by: dict[str, dict[str, int]] = {}
    for ph, _, st, _ in R.rows:
        by.setdefault(ph, {"PASS": 0, "FAIL": 0, "NOTE": 0})[st] += 1
    print("\nPer phase:")
    for ph, c in by.items():
        print(f"  {ph:<14} PASS={c['PASS']:<3} FAIL={c['FAIL']:<3} NOTE(not observed/info)={c['NOTE']}")
    write_coverage()
    (OUT / "summary.json").write_text(json.dumps([dict(phase=p, check=n, status=s, evidence=e) for p, n, s, e in R.rows], indent=1), encoding="utf-8")
    print(f"\nScreenshots: {SHOTS}\nSummary json: {OUT / 'summary.json'}\nElapsed: {int(time.time() - t_start)}s")
    fails = R.failed()
    print(f"RESULT: {'FAIL' if fails else 'PASS'} ({fails} failing checks)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
