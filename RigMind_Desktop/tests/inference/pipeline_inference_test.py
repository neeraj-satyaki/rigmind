"""Live inference test: starts rigmind-pipeline.exe, opens the camera, collects "detections" events for a while.

    python pipeline_inference_test.py [--uri rtsp://...] [--seconds 40] [--exe build\\Release\\rigmind-pipeline.exe]
                                      [--plugin build\\Release\\librigmindews.dll] [--models models] [--data <dir>]

Prints per model: events, average inference ms, classes seen, three sample items. Fails (exit 1) when a model
produced no events, or when the detections-line rate exceeds the cap the WebView needs (30 lines/s).
GStreamer must be on PATH (C:\\gstreamer\\1.0\\msvc_x86_64\\bin). The camera URI is not printed.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="rtsp://192.168.1.8:8556/feed0")
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--exe", default=str(ROOT / "build" / "Release" / "rigmind-pipeline.exe"))
    ap.add_argument("--plugin", default=str(ROOT / "build" / "Release" / "librigmindews.dll"))
    ap.add_argument("--models", default=str(ROOT / "models"))
    ap.add_argument("--data", default=None)
    ap.add_argument("--dump", default=None, help="write all event lines to this file")
    args = ap.parse_args()

    gst_bin = r"C:\gstreamer\1.0\msvc_x86_64\bin"
    env = dict(os.environ, PATH=gst_bin + os.pathsep + os.environ.get("PATH", ""))
    data = args.data or tempfile.mkdtemp(prefix="rigmind-inf-")
    proc = subprocess.Popen([args.exe, data, args.plugin, args.models], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, env=env, cwd=data, text=True, bufsize=1)
    lines = []
    lock = threading.Lock()

    def reader():
        for raw in proc.stdout:
            try:
                ev = json.loads(raw)
            except ValueError:
                continue
            with lock:
                lines.append((time.time(), ev))

    threading.Thread(target=reader, daemon=True).start()
    time.sleep(2)
    proc.stdin.write(json.dumps({"cmd": "open", "cam": "CAM-01", "uri": args.uri}) + "\n")
    proc.stdin.flush()
    time.sleep(args.seconds)
    try:
        proc.stdin.close()
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

    with lock:
        events = list(lines)
    if args.dump:
        Path(args.dump).write_text("\n".join(json.dumps(e) for _, e in events), encoding="utf-8")

    status = [e["state"] for _, e in events if e.get("ev") == "status"]
    print("camera states:", status)
    for _, e in events:
        if e.get("ev") == "error":
            print("error event:", e.get("message"))

    detections = defaultdict(list)
    for t, e in events:
        if e.get("ev") == "detections":
            detections[e["model"]].append((t, e))
    failed = False
    for model in ("ppe", "idss"):
        evs = detections.get(model, [])
        if not evs:
            print(f"[{model}] NO EVENTS")
            failed = True
            continue
        span = max(evs[-1][0] - evs[0][0], 1e-3)
        classes = defaultdict(int)
        items = []
        for _, e in evs:
            for it in e["items"]:
                classes[it["label"]] += 1
                items.append(it)
        avg_ms = sum(e["ms"] for _, e in evs) / len(evs)
        print(f"[{model}] events={len(evs)} ({len(evs) / span:.2f}/s) avg_ms={avg_ms:.1f} classes={dict(classes)}")
        for it in items[:3]:
            print("   sample:", json.dumps(it))
        first = evs[0][1]
        if first.get("w") != 1280 or first.get("h") != 720:
            print(f"[{model}] unexpected frame size {first.get('w')}x{first.get('h')}")
            failed = True
    # Line-rate cap: no one-second window may hold more than 30 detections lines.
    stamps = sorted(t for evs in detections.values() for t, _ in evs)
    worst = max((sum(1 for u in stamps if t <= u < t + 1) for t in stamps), default=0)
    print("max detections lines in any 1 s window:", worst)
    if worst > 30:
        failed = True
    inf = [e for _, e in events if e.get("ev") == "inference"]
    if inf:
        print("last inference events:", json.dumps(inf[-2:]))
    print("FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
