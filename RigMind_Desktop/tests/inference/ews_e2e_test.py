"""End-to-end EWS test with synthetic facts: the camera supplies the frames, the "facts" command supplies the geometry.

    python ews_e2e_test.py [--uri rtsp://...] [--seconds 90]

Sends a derrickman-outside-the-boundary situation (machine present, link tilt angled, elevator near the platform and
present, derrickman out of the safe zone) with "auto":false so the live detections do not overwrite it, then checks that
the EWS goes None -> Yellow -> (Orange) -> Red and that a footage clip is saved. Prints each alarm event with its frame.
Needs GStreamer on PATH and the camera; the EWS counts camera frames (Yellow > 4, Orange > 20, Red > 200 frames).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

FACTS = {"machine_present": True, "link_tilt_present": True, "link_tilt_angled": True, "elevator_present": True,
         "elevator_near_platform": True, "derrickman_out_of_safe_zone": True}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="rtsp://192.168.1.8:8554/feed0")
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--exe", default=str(ROOT / "build" / "Release" / "rigmind-pipeline.exe"))
    ap.add_argument("--plugin", default=str(ROOT / "build" / "Release" / "librigmindews.dll"))
    ap.add_argument("--models", default=str(ROOT / "models"))
    args = ap.parse_args()

    env = dict(os.environ, PATH=r"C:\gstreamer\1.0\msvc_x86_64\bin" + os.pathsep + os.environ.get("PATH", ""))
    data = tempfile.mkdtemp(prefix="rigmind-ews-")
    proc = subprocess.Popen([args.exe, data, args.plugin, args.models], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, env=env, cwd=data, text=True, bufsize=1)
    events, lock, t0 = [], threading.Lock(), time.time()

    def reader():
        for raw in proc.stdout:
            try:
                ev = json.loads(raw)
            except ValueError:
                continue
            with lock:
                events.append((time.time() - t0, ev))

    threading.Thread(target=reader, daemon=True).start()

    def send(obj):
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()

    time.sleep(2)
    send({"cmd": "open", "cam": "CAM-01", "uri": args.uri})
    # Wait for the camera, then switch the automatic facts off and set the synthetic situation.
    while time.time() - t0 < 30:
        with lock:
            if any(e.get("ev") == "status" and e.get("state") == "online" for _, e in events):
                break
        time.sleep(0.5)
    send({"cmd": "facts", "cam": "CAM-01", "auto": False, **FACTS})
    set_at = time.time() - t0
    time.sleep(args.seconds)
    send({"cmd": "facts", "cam": "CAM-01", "auto": False})  # all clear
    time.sleep(8)
    proc.stdin.close()
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

    with lock:
        seen = list(events)
    print(f"facts set at t={set_at:.1f}s")
    states = []
    for t, e in seen:
        if e.get("ev") == "alarm":
            states.append(e["state"])
            print(f"  t={t:6.1f}s alarm {e['previous']} -> {e['state']} severity={e['severity']} condition={e['condition']} place={e['place']} frame={e['frame']}")
        elif e.get("ev") in ("clip", "evidence"):
            print(f"  t={t:6.1f}s {json.dumps(e)}")
    clips = [e for _, e in seen if e.get("ev") == "clip"]
    ok = "Yellow" in states and "Red" in states and states.index("Yellow") < states.index("Red") and bool(clips)
    footage = list((Path(data) / "footage").rglob("*.mp4")) if (Path(data) / "footage").exists() else []
    print("clip events:", len(clips), " footage files:", len(footage))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
