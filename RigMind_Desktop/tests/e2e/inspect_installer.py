"""Elevated helper: starts the installer, then dumps every top-level window of the installer process with its child controls
(win32 and UIA views) and tries to press the default button, so the driver's selectors can be fixed from facts.
usage (elevated): python inspect_installer.py <installer.exe> <out.json> [--uninstall]
It never answers the sample-data question and stops after the first few pages; close the installer by hand afterwards."""
import json, subprocess, sys, time
from pathlib import Path
from pywinauto import Desktop

exe, out = sys.argv[1], Path(sys.argv[2])
proc = subprocess.Popen([exe])
report = {"pid": proc.pid, "snapshots": []}


def dump(backend: str):
    rows = []
    try:
        for w in Desktop(backend=backend).windows():
            try:
                if w.process_id() != proc.pid and "RigMind" not in (w.window_text() or ""):
                    continue
                item = {"title": w.window_text(), "class": w.class_name(), "handle": w.handle, "pid": w.process_id(), "visible": w.is_visible(), "children": []}
                for c in w.descendants():
                    try:
                        item["children"].append({"class": c.class_name(), "text": (c.window_text() or "")[:80], "id": c.control_id() if backend == "win32" else None,
                                                 "enabled": c.is_enabled(), "visible": c.is_visible()})
                    except Exception as e:  # noqa: BLE001
                        item["children"].append({"error": str(e)[:80]})
                rows.append(item)
            except Exception as e:  # noqa: BLE001
                rows.append({"error": str(e)[:120]})
    except Exception as e:  # noqa: BLE001
        rows.append({"backend_error": str(e)[:200]})
    return rows


time.sleep(6)
for step in range(4):
    last = step == 3  # the 4th snapshot only looks (Start menu page): it never presses Install
    snap = {"step": step, "t": time.strftime("%H:%M:%S"), "win32": dump("win32"), "uia": dump("uia")}
    report["snapshots"].append(snap)
    out.write_text(json.dumps(report, indent=1))
    print(f"snapshot {step}: win32 windows={len(snap['win32'])} uia windows={len(snap['uia'])}", flush=True)
    # try to press the default button (Next / I Agree / Install) with the win32 view, then wait for the next page
    pressed = None
    if last:
        break
    try:
        for w in Desktop(backend="win32").windows(class_name="#32770"):
            if "RigMind" in (w.window_text() or "") and w.process_id() == proc.pid:
                for c in w.descendants():
                    if c.class_name() == "Button" and c.control_id() == 1 and c.is_enabled():
                        pressed = (c.window_text(), c.control_id())
                        c.click()
                        break
                break
    except Exception as e:  # noqa: BLE001
        pressed = f"error {e}"[:120]
    snap["pressed"] = str(pressed)
    print("pressed:", pressed, flush=True)
    time.sleep(4)
    if pressed is None:
        break
out.write_text(json.dumps(report, indent=1))
print("written", out, flush=True)
