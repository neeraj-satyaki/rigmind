#!/usr/bin/env python
"""RigMind "two identical streams" load test: one camera (baseline) against two cameras fed from the SAME RTSP feed.

    python tests\\validation\\dual_stream_load.py                    # warm-up 60 s + 240 s measured, baseline then dual
    python tests\\validation\\dual_stream_load.py --duration 30 --warmup 15      # short dry run
    python tests\\validation\\dual_stream_load.py --cameras 2        # only the dual phase (no baseline: the 60 % rule is then not judged)

What it does (a visible RigMind window driven through Playwright connect_over_cdp, real RTSP source, nothing in the product changed):
  - private copy of the installed layout (no BitNet): gst\\ and gpu\\ are junctions to the seed app, the fresh build exes and the staged
    web bundle are copied in; a fresh LOCALAPPDATA; the app requires sign-in, so the page creates the account first (auth.setup via the UI)
  - phase "1 camera":  CAM-01          phase "2 cameras":  CAM-01 + CAM-02, both at --uri
    each phase: records.set stream.sources, stream.open per camera, the Live page open, --warmup seconds, then --duration seconds measured
  - every --interval seconds: system CPU %, CPU % of RigMind.exe / rigmind-pipeline.exe / msedgewebview2 (children), free RAM,
    GPU utilisation / memory / temperature / power (nvidia-smi), the <video> of the Live page (frames, dropped)
  - from the host events the bridge forwards ('detections', 'inference', 'latency', 'facts', 'alarm', 'status', 'stats', 'error'):
    per camera and model detections/s, inference ms and fps, detection latency, the longest gap between detections (freeze),
    camera status changes, alarm states per camera
Output: tests\\validation\\DUAL_STREAM_REPORT.md and dual_stream_samples.json. Only processes this script starts are stopped; the
temporary data folders are deleted at the end.
"""
from __future__ import annotations

import argparse
import collections
import ctypes
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import traceback
from pathlib import Path

import psutil

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]  # RigMind_Desktop
BUILD = REPO / "build-app" / "Release"
RUNS = REPO.parent / "tests_run"
E2E_USER = "e2e.operator"
E2E_PASS = "Rig-Mind-E2E-2026!"
RTSP_URI = "rtsp://192.168.1.8:8556/feed0"
CAMS = ["CAM-01", "CAM-02"]
MODELS = ["ppe", "idss"]
NVSMI = next((p for p in (r"C:\Windows\System32\nvidia-smi.exe", r"C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe") if Path(p).exists()), None)

A: argparse.Namespace
APP: dict = {"proc": None}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def pctl(vals, p):
    v = sorted(x for x in vals if x is not None and not (isinstance(x, float) and math.isnan(x)))
    if not v:
        return None
    k = (len(v) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] if lo == hi else v[lo] + (v[hi] - v[lo]) * (k - lo)


def dist(vals, nd=1):
    v = [x for x in vals if x is not None and not (isinstance(x, float) and math.isnan(x))]
    if not v:
        return {"n": 0, "min": None, "mean": None, "median": None, "p95": None, "max": None}
    r = lambda x: round(x, nd)  # noqa: E731
    return {"n": len(v), "min": r(min(v)), "mean": r(statistics.fmean(v)), "median": r(statistics.median(v)), "p95": r(pctl(v, 95)), "max": r(max(v))}


def fmt(x, nd=1, unit=""):
    return "n/a" if x is None else f"{x:.{nd}f}{unit}"


# ---------------------------------------------------------------------------------------------------- app staging / launch
def stage_app() -> Path:
    app, seed = Path(A.app_dir), Path(A.seed_dir)
    if not seed.exists():
        raise SystemExit(f"seed app {seed} is missing")
    app.mkdir(parents=True, exist_ok=True)
    for item in seed.iterdir():
        if item.name in ("gst", "gpu"):
            if not (app / item.name).exists():
                subprocess.run(["cmd", "/c", "mklink", "/J", str(app / item.name), str(item)], check=True, capture_output=True)
        elif item.name in ("web", "slm"):
            continue  # web comes from the fresh bundle; no BitNet
        elif not (app / item.name).exists():
            shutil.copytree(item, app / item.name) if item.is_dir() else shutil.copy2(item, app / item.name)
    for f in ("RigMind.exe", "rigmind-pipeline.exe", "librigmindews.dll"):
        shutil.copy2(BUILD / f, app / f)
    if (app / "web").exists():
        shutil.rmtree(app / "web")
    shutil.copytree(REPO / "web", app / "web")
    hits = sum(1 for f in (app / "web").rglob("*") if f.suffix in (".js", ".html", ".mjs") and "__RIGMIND_CONFIG__" in f.read_text(encoding="utf-8", errors="ignore"))
    if hits == 0:
        raise SystemExit("the staged web bundle has no '__RIGMIND_CONFIG__': stale bundle, run vite build + scripts\\stage_web.ps1 first")
    log(f"staged {app} (exes from {BUILD}, web bundle with config in {hits} files, slm present: {(app / 'slm').exists()})")
    return app


def launch_app(app: Path, local: Path) -> None:
    env = dict(os.environ)  # inherited: a stripped environment stops WebView2 from opening the debug port
    env["LOCALAPPDATA"] = str(local)
    env["WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"] = f"--remote-debugging-port={A.port} --remote-allow-origins=*"
    APP["proc"] = subprocess.Popen([str(app / "RigMind.exe")], cwd=str(app), env=env)
    log(f"launched RigMind.exe pid {APP['proc'].pid}, LOCALAPPDATA={local}, port {A.port}")


def _close_windows(pid: int) -> None:
    user32 = ctypes.windll.user32
    cb_t = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(h, _):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            user32.PostMessageW(h, 0x0010, 0, 0)  # WM_CLOSE
        return True

    user32.EnumWindows(cb_t(cb), 0)


def stop_app() -> None:
    proc = APP["proc"]
    if proc is None:
        return
    try:
        kids = psutil.Process(proc.pid).children(recursive=True)
    except psutil.Error:
        kids = []
    try:
        _close_windows(proc.pid)
        proc.wait(timeout=20)
    except Exception:  # noqa: BLE001
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            pass
    for k in kids:  # only our own children
        try:
            if k.is_running():
                k.terminate()
        except psutil.Error:
            pass
    gone, alive = psutil.wait_procs(kids, timeout=8)
    for k in alive:
        try:
            k.kill()
        except psutil.Error:
            pass
    APP["proc"] = None
    time.sleep(2)


def rmtree_safe(path: Path) -> None:
    if not path.exists():
        return
    for name in ("gst", "gpu"):  # junctions: remove the link, never the target
        j = path / name
        if j.exists() and (j.is_junction() if hasattr(j, "is_junction") else False):
            os.rmdir(j)
    shutil.rmtree(path, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------- page instrumentation
COLLECTOR_JS = r"""
() => {
  if (window.__dl) return true;
  const pcs = window.__dlPcs = [];
  const Orig = window.RTCPeerConnection;
  if (Orig) { const W = function (...a) { const pc = new Orig(...a); pcs.push(pc); return pc; }; W.prototype = Orig.prototype; Object.setPrototypeOf(W, Orig); window.RTCPeerConnection = W; }
  const dl = window.__dl = { det: {}, inf: [], lat: [], stat: [], stats: [], alarm: [], err: [], facts: [], ppe: [], cnt: {}, rvfc: [], vid: null, cam: {}, hist: [] };
  window.chrome.webview.addEventListener('message', (e) => {
    const d = e.data; if (!d || !d.ev) return;
    const now = Date.now();
    dl.cnt[d.ev] = (dl.cnt[d.ev] || 0) + 1;
    if (d.ev === 'status' || d.ev === 'error' || d.ev === 'alarm') dl.hist.push(Object.assign({ t: now }, d));
    if (d.ev === 'detections') {
      const k = d.cam + '|' + d.model; (dl.det[k] = dl.det[k] || []).push([now, d.latency_ms, d.ms, (d.items || []).length]);
    } else if (d.ev === 'inference') {
      dl.inf.push({ t: now, cam: d.cam, model: d.model, fps: d.fps, ms: d.avg_ms, provider: d.provider, scale_mode: d.scale_mode });
    } else if (d.ev === 'latency') {
      dl.lat.push({ t: now, cam: d.cam, src_gap: d.src_gap, out_gap: d.out_gap, overlay_wait: d.overlay_wait, convert_encode: d.convert_encode, pace: d.pace, sync: d.sync, inn: d.in, out: d.out });
    } else if (d.ev === 'status') {
      dl.cam[d.cam] = d.state; dl.stat.push({ t: now, cam: d.cam, state: d.state, reason: d.reason, reconnects: d.reconnects });
    } else if (d.ev === 'stats') {
      dl.stats.push({ t: now, cam: d.cam, fps: d.fps, kbps: d.kbps, resyncs: d.resyncs, reconnects: d.reconnects });
    } else if (d.ev === 'alarm') {
      dl.alarm.push({ t: now, cam: d.cam, state: d.state, previous: d.previous, condition: d.condition });
    } else if (d.ev === 'facts') {
      dl.facts.push({ t: now, cam: d.cam, state: d.state, zoomed: d.zoomed, pct: d.pct });
    } else if (d.ev === 'ppe') {
      dl.ppe.push({ t: now, cam: d.cam, state: d.state });
    } else if (d.ev === 'error') {
      dl.err.push({ t: now, cam: d.cam, message: d.message });
    }
  });
  setInterval(() => {
    const v = document.querySelector('video');
    if (v && v !== dl.vid && v.requestVideoFrameCallback) {
      dl.vid = v;
      const cb = () => { dl.rvfc.push(performance.timeOrigin + performance.now()); if (dl.vid === v) v.requestVideoFrameCallback(cb); };
      v.requestVideoFrameCallback(cb);
    }
  }, 400);
  window.__dlVideo = () => { const v = document.querySelector('video'); if (!v) return null;
    const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
    return { total: q.totalVideoFrames, dropped: q.droppedVideoFrames, w: v.videoWidth, h: v.videoHeight, paused: v.paused, ct: v.currentTime }; };
  window.__dlMedia = async () => {
    const out = { videos: [...document.querySelectorAll('video')].map(v => ({ w: v.videoWidth, h: v.videoHeight })), rtc: [] };
    for (const pc of pcs) {
      if (pc.connectionState === 'closed') continue;
      let st; try { st = await pc.getStats(); } catch (e) { continue; }
      st.forEach((r) => { if (r.type === 'inbound-rtp' && (r.kind === 'video' || r.mediaType === 'video'))
        out.rtc.push({ bytes: r.bytesReceived, frames: r.framesDecoded, fw: r.frameWidth, fh: r.frameHeight, dropped: r.framesDropped, freezes: r.freezeCount, fps: r.framesPerSecond }); });
    }
    return out; };
  window.__dlDrain = () => { const r = { det: dl.det, inf: dl.inf, lat: dl.lat, stat: dl.stat, stats: dl.stats, alarm: dl.alarm, err: dl.err, facts: dl.facts, ppe: dl.ppe, cnt: dl.cnt, rvfc: dl.rvfc };
    dl.det = {}; dl.inf = []; dl.lat = []; dl.stat = []; dl.stats = []; dl.alarm = []; dl.err = []; dl.facts = []; dl.ppe = []; dl.rvfc = []; return r; };
  window.__dlBridge = (m, p) => new Promise((res, rej) => { const id = 9000000 + Math.floor(Math.random() * 1e6);
    const h = (e) => { if (e.data && e.data.id === id) { window.chrome.webview.removeEventListener('message', h); res(e.data); } };
    window.chrome.webview.addEventListener('message', h); window.chrome.webview.postMessage(JSON.stringify({ id, method: m, params: p || {} }));
    setTimeout(() => rej('bridge timeout ' + m), 15000); });
  return true;
}
"""


def bridge(pg, method: str, params: dict | None = None):
    r = pg.evaluate("([m,p]) => window.__dlBridge(m,p)", [method, params or {}])
    if not r.get("ok"):
        raise RuntimeError(f"bridge {method}: {r.get('error')}")
    return r.get("result")


def sign_in(pg) -> str:
    """The first start creates the account (auth.setup through the sign-in form), later starts sign in."""
    pg.wait_for_selector("text=/Create the first account|Welcome back/i", timeout=90000)
    mode = "created" if "Create the first account" in pg.locator("body").inner_text() else "signed in"
    pg.get_by_label("Username").first.fill(E2E_USER)
    pg.get_by_label("Password", exact=True).first.fill(E2E_PASS)
    if mode == "created":
        pg.get_by_label("Repeat the password").first.fill(E2E_PASS)
        pg.get_by_role("button", name="Create account and sign in").first.click()
    else:
        pg.get_by_role("button", name="Sign in to workspace").first.click()
    pg.wait_for_selector("aside", timeout=30000)
    return mode


def nav_live(pg) -> None:
    link = pg.locator("aside a", has_text=re.compile(r"^\s*Live monitoring(\s*\d+)?\s*$")).first
    link.click()
    time.sleep(2)


def remount_view(pg, view: str) -> None:
    """Leave and re-enter a page so it reads the new stream.sources and mounts its videos: 'live' = full profile, 'fleet' = small tiles."""
    other = "/live" if view == "fleet" else "/fleet"
    pg.evaluate(f"history.pushState(null, '', '{other}'); window.dispatchEvent(new PopStateEvent('popstate'))")
    time.sleep(1.5)
    label = "Fleet overview" if view == "fleet" else "Live monitoring"
    pg.locator("aside a", has_text=re.compile(rf"^\s*{label}(\s*\d+)?\s*$")).first.click()
    time.sleep(2)


# ---------------------------------------------------------------------------------------------------- resource sampling
class Monitor:
    def __init__(self):
        self.cache: dict[int, psutil.Process] = {}
        self.seen: set[int] = set()
        self.ncpu = psutil.cpu_count() or 1
        psutil.cpu_percent(None)

    def procs(self):
        out = {}
        if APP["proc"] is not None:
            try:
                root = psutil.Process(APP["proc"].pid)
                for p in [root] + root.children(recursive=True):
                    out[p.pid] = p
            except psutil.Error:
                pass
        return out

    def sample(self) -> dict:
        groups = collections.defaultdict(lambda: {"cpu": 0.0, "rss_mb": 0.0, "n": 0})
        for pid, p in self.procs().items():
            try:
                c = self.cache.setdefault(pid, p)
                name = (c.name() or "").lower()
                cpu = c.cpu_percent(None)
                first = pid not in self.seen
                self.seen.add(pid)
                g = groups[name]
                g["rss_mb"] += c.memory_info().rss / 1048576
                g["n"] += 1
                if not first:
                    g["cpu"] += cpu  # % of ONE core, summed over the processes of that name
            except psutil.Error:
                pass
        vm = psutil.virtual_memory()
        s = {"t": time.time(), "sys_cpu": psutil.cpu_percent(None), "free_mb": vm.available / 1048576, "groups": {k: dict(v) for k, v in groups.items()}}
        s["gpu"] = gpu_sample()
        return s


def gpu_sample():
    if not NVSMI:
        return None
    try:
        out = subprocess.run([NVSMI, "--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=10).stdout.strip().splitlines()[0]
        v = [float(x) if re.match(r"^-?[\d.]+$", x.strip()) else None for x in out.split(",")]
        return {"util": v[0], "mem_used_mb": v[1], "mem_total_mb": v[2], "temp_c": v[3], "power_w": v[4]}
    except Exception:  # noqa: BLE001
        return None


# ---------------------------------------------------------------------------------------------------- a phase
def wait_online(pg, cams: list[str], timeout: float) -> dict:
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = pg.evaluate("window.__dl.cam")
        if all(st.get(c) == "online" for c in cams):
            return {"online": True, "after_s": round(time.time() - t0, 1), "states": st}
        time.sleep(1)
    return {"online": False, "after_s": round(time.time() - t0, 1), "states": pg.evaluate("window.__dl.cam")}


def run_phase(pg, mon: Monitor, name: str) -> dict:
    view = "fleet" if name == "tile" else "live"
    ncam = 1 if name == "1" else 2
    cams = CAMS[:ncam]
    log(f"=== phase {name}: {ncam} camera(s) {cams}, {view} page ===")
    sources = {c: A.uri for c in cams}
    bridge(pg, "records.set", {"key": "stream.sources", "value": sources})
    for c in cams:
        bridge(pg, "stream.open", {"cam": c, "uri": A.uri})
    remount_view(pg, view)
    ph: dict = {"cams": cams, "view": view, "t_open": time.time()}
    ph["online"] = wait_online(pg, cams, 120)
    log(f"   online: {ph['online']}")
    log(f"   warm-up {A.warmup}s")
    samples: list[dict] = []
    end = time.time() + A.warmup
    while time.time() < end:
        mon.sample()  # primes the per-process CPU counters
        time.sleep(A.interval)
    pg.evaluate("window.__dlDrain()")  # drop warm-up events; alarm/status history before this point is kept in ph["pre"]
    v0 = pg.evaluate("window.__dlVideo()")
    m0 = pg.evaluate("window.__dlMedia()")
    t_start = time.time()
    end = t_start + A.duration
    log(f"   measuring {A.duration}s (sample every {A.interval}s)")
    while time.time() < end:
        s = mon.sample()
        s["video"] = pg.evaluate("window.__dlVideo()")
        s["media"] = pg.evaluate("window.__dlMedia()")
        samples.append(s)
        time.sleep(max(0.0, A.interval - (time.time() - s["t"])))
    t_end = time.time()
    v1 = pg.evaluate("window.__dlVideo()")
    raw = pg.evaluate("window.__dlDrain()")
    ph.update({"t_start": t_start, "t_end": t_end, "samples": samples, "raw": raw, "video0": v0, "video1": v1, "media0": m0, "states_end": pg.evaluate("window.__dl.cam"), "hist": pg.evaluate("window.__dl.hist")})
    log(f"   events in window: {raw['cnt']}")
    return ph


# ---------------------------------------------------------------------------------------------------- analysis
def analyse(ph: dict) -> dict:
    cams, raw = ph["cams"], ph["raw"]
    dur = ph["t_end"] - ph["t_start"]
    t0, t1 = ph["t_start"] * 1000, ph["t_end"] * 1000
    a: dict = {"cams": cams, "duration_s": round(dur, 1), "per_cam": {}}
    for c in cams:
        pc: dict = {"models": {}}
        for m in MODELS:
            rows = [r for r in raw["det"].get(f"{c}|{m}", []) if t0 <= r[0] <= t1]
            ts = [r[0] for r in rows]
            gaps = [(b - a_) / 1000 for a_, b in zip(ts, ts[1:])]
            # the gap from the window start to the first and from the last to the window end count as freezes too
            edge = []
            if ts:
                edge = [(ts[0] - t0) / 1000, (t1 - ts[-1]) / 1000]
            else:
                edge = [dur]
            infs = [i for i in raw["inf"] if i["cam"] == c and i["model"] == m and t0 <= i["t"] <= t1]
            pc["models"][m] = {
                "detections": len(rows), "det_per_s": round(len(rows) / dur, 2) if dur else None,
                "max_gap_s": round(max(gaps + edge), 2), "gap_p95_s": round(pctl(gaps, 95) or 0, 2) if gaps else None,
                "latency_ms": dist([r[1] for r in rows]), "model_run_ms": dist([r[2] for r in rows]),
                "inference_fps": dist([i["fps"] for i in infs]), "inference_ms": dist([i["ms"] for i in infs]),
                "provider": next((i["provider"] for i in infs if i.get("provider")), None),
                "avg_items": round(statistics.fmean([r[3] for r in rows]), 2) if rows else None,
            }
        stats = [s for s in raw["stats"] if s["cam"] == c and t0 <= s["t"] <= t1]
        pc["feed_fps"] = dist([s["fps"] for s in stats])
        pc["feed_kbps"] = dist([s["kbps"] for s in stats])
        pc["resyncs_end"] = stats[-1]["resyncs"] if stats else None
        st = [s for s in raw["stat"] if s["cam"] == c]
        pc["status_changes_in_window"] = [{"t": round(s["t"] / 1000 - ph["t_start"], 1), "state": s["state"], "reason": s["reason"]} for s in st if t0 <= s["t"] <= t1]
        pc["state_end"] = ph["states_end"].get(c)
        pc["alarm_events"] = [{"t": round(s["t"] / 1000 - ph["t_start"], 1), "state": s["state"], "previous": s["previous"], "condition": s["condition"]} for s in raw["alarm"] if s["cam"] == c and t0 <= s["t"] <= t1]
        pc["alarm_states_reached"] = sorted({s["state"] for s in raw["alarm"] if s["cam"] == c and t0 <= s["t"] <= t1})
        facts = [f for f in raw["facts"] if f["cam"] == c and t0 <= f["t"] <= t1]
        pc["facts_state_end"] = facts[-1]["state"] if facts else None
        pc["facts_pct_mean"] = {k: round(statistics.fmean([f["pct"][k] for f in facts if f.get("pct")]), 1) for k in ("machine_present", "link_tilt_angled", "elevator_near_platform")} if facts else {}
        pc["ppe_events"] = [f["state"] for f in raw["ppe"] if f["cam"] == c and t0 <= f["t"] <= t1]
        lat = [l for l in raw["lat"] if l["cam"] == c and t0 <= l["t"] <= t1]
        pc["pipeline_latency"] = {k: dist([(l[k] or {}).get("p95") for l in lat if isinstance(l.get(k), dict)]) for k in ("overlay_wait", "convert_encode", "out_gap", "src_gap")}
        a["per_cam"][c] = pc
    a["errors"] = [e for e in raw["err"] if t0 <= e["t"] <= t1]
    # resources
    smp = ph["samples"]
    res: dict = {"sys_cpu": dist([s["sys_cpu"] for s in smp]), "free_ram_mb": dist([s["free_mb"] for s in smp], 0), "procs": {}}
    names = sorted({n for s in smp for n in s["groups"]})
    for n in names:
        res["procs"][n] = {"cpu_of_one_core": dist([s["groups"].get(n, {}).get("cpu") for s in smp if n in s["groups"]]),
                           "rss_mb": dist([s["groups"].get(n, {}).get("rss_mb") for s in smp if n in s["groups"]], 0)}
    g = [s["gpu"] for s in smp if s.get("gpu")]
    res["gpu"] = {"util": dist([x["util"] for x in g]), "mem_used_mb": dist([x["mem_used_mb"] for x in g], 0), "mem_total_mb": g[0]["mem_total_mb"] if g else None,
                  "temp_c": dist([x["temp_c"] for x in g]), "power_w": dist([x["power_w"] for x in g])} if g else {}
    a["resources"] = res
    # video of the Live page (first <video>)
    fr = [t for t in raw["rvfc"] if t0 <= t <= t1]
    gaps = [(b - c_) for c_, b in zip(fr, fr[1:])]
    v0, v1 = ph.get("video0") or {}, ph.get("video1") or {}
    a["video"] = {"frames": len(fr), "fps": round(len(fr) / dur, 2) if dur else None, "max_gap_ms": round(max(gaps)) if gaps else None,
                  "gaps_over_500ms": sum(1 for x in gaps if x > 500), "gaps_over_1s": sum(1 for x in gaps if x > 1000),
                  "dropped": (v1.get("dropped") or 0) - (v0.get("dropped") or 0) if v0 and v1 else None, "size": f"{v1.get('w')}x{v1.get('h')}" if v1 else None}
    m0, m1 = ph.get("media0") or {"rtc": []}, (smp[-1].get("media") if smp else None) or {"rtc": [], "videos": []}
    streams = []
    for i, e in enumerate(m1["rtc"]):
        b0 = m0["rtc"][i]["bytes"] if i < len(m0["rtc"]) else 0
        streams.append({"stream": i, "out_resolution": f"{e.get('fw')}x{e.get('fh')}", "kbps": round((e["bytes"] - b0) * 8 / 1000 / dur, 0) if dur else None,
                        "fps": round(((e.get("frames") or 0) - (m0["rtc"][i].get("frames") or 0)) / dur, 1) if dur and i < len(m0["rtc"]) else None,
                        "freezes": e.get("freezes"), "dropped": e.get("dropped")})
    a["media"] = {"view": ph.get("view"), "video_elements": [f"{v['w']}x{v['h']}" for v in m1.get("videos", [])], "streams": streams}
    return a


def verdicts(base: dict | None, dual: dict) -> list[dict]:
    V: list[dict] = []

    def add(name, ok, evidence):
        V.append({"name": name, "status": "PASS" if ok is True else "FAIL" if ok is False else "NOTE", "evidence": evidence})

    dc = dual["per_cam"]
    online = all(dc[c]["state_end"] == "online" and not [s for s in dc[c]["status_changes_in_window"] if s["state"] != "online"] for c in dual["cams"])
    add("both cameras stay online during the measurement", online,
        "; ".join(f"{c}: end={dc[c]['state_end']}, status changes in window={len(dc[c]['status_changes_in_window'])}" for c in dual["cams"]))
    if base:
        bc = base["per_cam"]["CAM-01"]
        for c in dual["cams"]:
            for m in MODELS:
                b_rate = bc["models"][m]["det_per_s"] or 0
                d_rate = dc[c]["models"][m]["det_per_s"] or 0
                ratio = d_rate / b_rate if b_rate else None
                add(f"{c} {m} detections/s >= 60 % of single camera", ratio is not None and ratio >= 0.6,
                    f"{d_rate:.2f}/s vs baseline {b_rate:.2f}/s = {fmt(None if ratio is None else ratio * 100, 0, ' %')}")
    else:
        for c in dual["cams"]:
            for m in MODELS:
                add(f"{c} {m} has detections", (dc[c]["models"][m]["detections"] or 0) > 0, f"{dc[c]['models'][m]['det_per_s']}/s (no baseline run)")
    for c in dual["cams"]:
        worst = max(dc[c]["models"][m]["max_gap_s"] for m in MODELS)
        add(f"{c} no gap between detections > 5 s", worst <= 5.0, "longest gap " + ", ".join(f"{m} {dc[c]['models'][m]['max_gap_s']} s" for m in MODELS))
    r = dual["resources"]
    cpu_avg = r["sys_cpu"]["mean"]
    gpu_peak = (r.get("gpu") or {}).get("util", {}).get("max")
    free_min = r["free_ram_mb"]["min"]
    add("system CPU average <= 90 %", None if cpu_avg is None else cpu_avg <= 90, f"avg {fmt(cpu_avg, 1, ' %')}, peak {fmt(r['sys_cpu']['max'], 1, ' %')}")
    add("GPU utilisation peak <= 95 %", None if gpu_peak is None else gpu_peak <= 95, f"avg {fmt((r.get('gpu') or {}).get('util', {}).get('mean'), 1, ' %')}, peak {fmt(gpu_peak, 0, ' %')}")
    add("free RAM >= 300 MB throughout", None if free_min is None else free_min >= 300, f"minimum {fmt(free_min, 0, ' MB')}")
    reached = {c: tuple(dc[c]["alarm_states_reached"]) for c in dual["cams"]}
    seq = {c: [e["state"] for e in dc[c]["alarm_events"]] for c in dual["cams"]}
    same = len(set(reached.values())) == 1
    add("alarm states of the two cameras are consistent (same footage)", same, "; ".join(f"{c}: reached {list(reached[c]) or 'none'}, sequence {seq[c] or 'no change'}" for c in dual["cams"]))
    add("no 'error' events", not dual["errors"], f"{len(dual['errors'])} error event(s)" + (": " + "; ".join(e["message"][:100] for e in dual["errors"][:3]) if dual["errors"] else ""))
    return V


def bottleneck(base: dict | None, dual: dict) -> list[str]:
    out = []
    r = dual["resources"]
    gpu = (r.get("gpu") or {}).get("util", {})
    ncpu = psutil.cpu_count() or 1
    out.append(f"GPU utilisation in the dual run: average {fmt(gpu.get('mean'), 1, ' %')}, peak {fmt(gpu.get('max'), 0, ' %')}; system CPU average {fmt(r['sys_cpu']['mean'], 1, ' %')} (peak {fmt(r['sys_cpu']['max'], 1, ' %')}) on {ncpu} logical CPUs.")
    for m in MODELS:
        busy = 0.0
        parts = []
        for c in dual["cams"]:
            mm = dual["per_cam"][c]["models"][m]
            f, ms = mm["inference_fps"]["mean"], mm["inference_ms"]["mean"]
            if f is not None and ms is not None:
                busy += f * ms / 1000.0
                parts.append(f"{c} {f:.1f} fps x {ms:.0f} ms")
        out.append(f"Model worker '{m}' (one thread, shared by the cameras): busy {busy * 100:.0f} % ({'; '.join(parts)}).")
    feed = [dual["per_cam"][c]["feed_fps"]["mean"] for c in dual["cams"]]
    out.append("Camera feed into the pipeline (stats event): " + ", ".join(f"{c} {fmt(f, 1, ' fps')}" for c, f in zip(dual["cams"], feed)) + " (the source delivers ~3-6 fps in bursts, the pacer releases at most 12.5).")
    rates = {m: [dual["per_cam"][c]["models"][m]["det_per_s"] for c in dual["cams"]] for m in MODELS}
    limit = []
    for m in MODELS:
        for c, rate, f in zip(dual["cams"], rates[m], feed):
            if f and rate is not None and rate >= 0.85 * f:
                limit.append(f"{c}/{m}: detections/s {rate:.1f} is {rate / f * 100:.0f} % of the feed rate")
    if limit:
        out.append("Detections keep up with what the camera delivers -> the feed is the limit, not the machine: " + "; ".join(limit) + ".")
    wv = r["procs"].get("msedgewebview2.exe", {}).get("cpu_of_one_core", {}).get("mean")
    pl = r["procs"].get("rigmind-pipeline.exe", {}).get("cpu_of_one_core", {}).get("mean")
    out.append(f"Process CPU (percent of one core, mean): rigmind-pipeline.exe {fmt(pl, 0)}, msedgewebview2.exe {fmt(wv, 0)}, RigMind.exe {fmt(r['procs'].get('rigmind.exe', {}).get('cpu_of_one_core', {}).get('mean'), 0)}.")
    verdict = "undetermined"
    if gpu.get("mean") is not None and gpu["mean"] >= 85:
        verdict = "GPU-bound (utilisation near saturation)"
    elif r["sys_cpu"]["mean"] is not None and r["sys_cpu"]["mean"] >= 85:
        verdict = "CPU-bound (the machine has no CPU left)"
    elif limit and len(limit) >= len(dual["cams"]) * len(MODELS) * 0.75:
        verdict = "camera feed (models keep pace with the frames the camera delivers)"
    else:
        busy_models = [m for m in MODELS if sum((dual["per_cam"][c]["models"][m]["inference_fps"]["mean"] or 0) * (dual["per_cam"][c]["models"][m]["inference_ms"]["mean"] or 0) / 1000 for c in dual["cams"]) >= 0.85]
        verdict = ("model worker thread saturated (" + ", ".join(busy_models) + "): add GPU-side parallelism or lower the frame rate") if busy_models else "no resource is saturated; see the per-camera table"
    out.append(f"Bottleneck verdict: {verdict}.")
    if base:
        d_gpu = (gpu.get("mean") or 0) - (((base["resources"].get("gpu") or {}).get("util") or {}).get("mean") or 0)
        d_cpu = (r["sys_cpu"]["mean"] or 0) - (base["resources"]["sys_cpu"]["mean"] or 0)
        out.append(f"Cost of the second camera: GPU {d_gpu:+.1f} points, system CPU {d_cpu:+.1f} points.")
    return out


def md_table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] + ["| " + " | ".join(str(c) for c in r) + " |" for r in rows])


def write_report(meta: dict, base: dict | None, dual: dict | None, V: list[dict], notes: list[str], tile: dict | None = None) -> None:
    L = ["# RigMind two-camera load test", "",
         f"Run: {meta['started']}  |  host: {meta['host']}  |  GPU: {meta['gpu_name']}  |  RAM total {meta['ram_total_mb']} MB",
         f"Source for BOTH cameras: `{A.uri}`  |  warm-up {A.warmup} s, measured {A.duration} s per phase, sample every {A.interval} s  |  models run on: {meta.get('providers')}",
         "Build: fresh `build-app\\Release` exes, staged web bundle, installed layout without BitNet, fresh LOCALAPPDATA (signed in as the test account).", ""]
    if dual:
        L += ["## Verdicts", "", md_table(["Check", "Result", "Evidence"], [[v["name"], f"**{v['status']}**", v["evidence"]] for v in V]), ""]
        flags = [v for v in V if v["status"] == "FAIL"]
        L += [f"Overall: **{'FAIL' if flags else 'PASS'}**" + (f" ({len(flags)} failing check(s))" if flags else ""), ""]

    def per_cam_rows(ph):
        rows = []
        for c in ph["cams"]:
            pc = ph["per_cam"][c]
            for m in MODELS:
                mm = pc["models"][m]
                rows.append([c, m, mm["det_per_s"], mm["inference_fps"]["mean"], mm["inference_ms"]["mean"], mm["latency_ms"]["median"], mm["latency_ms"]["p95"], mm["max_gap_s"], mm["avg_items"]])
        return rows

    head = ["Camera", "Model", "detections/s", "inference fps (mean)", "inference ms (mean)", "latency ms median", "latency ms p95", "longest gap s", "avg items"]
    for title, ph in (("1 camera (baseline)", base), ("2 cameras (dual)", dual)):
        if not ph:
            continue
        L += [f"## Per camera and model: {title}", "", md_table(head, per_cam_rows(ph)), ""]
        L += [md_table(["Camera", "feed fps", "feed kbps", "state at end", "status changes in window", "alarm states reached", "alarm sequence", "PPE events", "facts % (machine / angled / near)"],
                       [[c, ph["per_cam"][c]["feed_fps"]["mean"], ph["per_cam"][c]["feed_kbps"]["mean"], ph["per_cam"][c]["state_end"], len(ph["per_cam"][c]["status_changes_in_window"]),
                         ", ".join(ph["per_cam"][c]["alarm_states_reached"]) or "none", " > ".join(e["state"] for e in ph["per_cam"][c]["alarm_events"]) or "no change",
                         ", ".join(ph["per_cam"][c]["ppe_events"]) or "none", " / ".join(str(ph["per_cam"][c]["facts_pct_mean"].get(k, "")) for k in ("machine_present", "link_tilt_angled", "elevator_near_platform"))] for c in ph["cams"]]), ""]
    if base and dual:
        rows = []
        for m in MODELS:
            b = base["per_cam"]["CAM-01"]["models"][m]["det_per_s"]
            for c in dual["cams"]:
                d = dual["per_cam"][c]["models"][m]["det_per_s"]
                rows.append([m, c, b, d, f"{(d / b * 100) if b else 0:.0f} %", f"{d - b:+.2f}"])
        L += ["## Detections per second: baseline vs dual", "", md_table(["Model", "Dual camera", "Baseline CAM-01 /s", "Dual /s", "Share of baseline", "Delta /s"], rows), ""]
    L += ["## Resources", ""]
    rrows = []
    for label, ph in (("1 camera", base), ("2 cameras", dual)):
        if not ph:
            continue
        r = ph["resources"]
        g = r.get("gpu") or {}
        pr = r["procs"]

        def cpu(n):
            return fmt(pr.get(n, {}).get("cpu_of_one_core", {}).get("mean"), 0)

        rrows.append([label, fmt(r["sys_cpu"]["mean"], 1), fmt(r["sys_cpu"]["max"], 0), fmt(g.get("util", {}).get("mean"), 1), fmt(g.get("util", {}).get("max"), 0),
                      fmt(g.get("mem_used_mb", {}).get("max"), 0), fmt(g.get("temp_c", {}).get("max"), 0), fmt(g.get("power_w", {}).get("mean"), 0), fmt(r["free_ram_mb"]["min"], 0), fmt(r["free_ram_mb"]["mean"], 0),
                      cpu("rigmind.exe"), cpu("rigmind-pipeline.exe"), cpu("msedgewebview2.exe")])
    if len(rrows) == 2:
        rrows.append(["delta"] + [f"{float(rrows[1][i]) - float(rrows[0][i]):+.1f}" if re.match(r"^-?[\d.]+$", rrows[0][i]) and re.match(r"^-?[\d.]+$", rrows[1][i]) else "n/a" for i in range(1, 13)])
    L += [md_table(["Phase", "sys CPU avg %", "sys CPU peak %", "GPU util avg %", "GPU util peak %", "GPU mem peak MB", "GPU temp peak C", "GPU power avg W", "free RAM min MB", "free RAM mean MB",
                    "RigMind.exe CPU (% of 1 core)", "pipeline CPU (% of 1 core)", "webview2 CPU (% of 1 core)"], rrows), ""]
    L += ["## Live page video (first `<video>`)", "", md_table(["Phase", "frames", "fps", "longest gap ms", "gaps > 500 ms", "gaps > 1 s", "dropped", "size"],
                                                           [[l, ph["video"]["frames"], ph["video"]["fps"], ph["video"]["max_gap_ms"], ph["video"]["gaps_over_500ms"], ph["video"]["gaps_over_1s"], ph["video"]["dropped"], ph["video"]["size"]]
                                                            for l, ph in (("1 camera", base), ("2 cameras", dual)) if ph]), ""]
    if tile and dual:
        def rr(label, ph):
            r = ph["resources"]
            g = r.get("gpu") or {}
            pr = r["procs"]
            rate = [ph["per_cam"][c]["models"][m]["det_per_s"] for c in ph["cams"] for m in MODELS]
            return [label, ", ".join(x["out_resolution"] for x in ph["media"]["streams"]) or "n/a", ", ".join(str(x["kbps"]) for x in ph["media"]["streams"]) or "n/a",
                    ", ".join(str(x["fps"]) for x in ph["media"]["streams"]) or "n/a", fmt(r["sys_cpu"]["mean"], 1), fmt(g.get("util", {}).get("mean"), 1),
                    fmt(pr.get("rigmind-pipeline.exe", {}).get("cpu_of_one_core", {}).get("mean"), 0), fmt(pr.get("msedgewebview2.exe", {}).get("cpu_of_one_core", {}).get("mean"), 0),
                    fmt(g.get("power_w", {}).get("mean"), 0), ", ".join(f"{x:.1f}" for x in rate)]
        L += ["## Tile scenario: both cameras as small viewers (Fleet page) vs full viewers (Live page)", "",
              md_table(["Scenario", "received resolution per stream", "kbps per stream (getStats bytesReceived)", "decoded fps per stream", "sys CPU avg %", "GPU util avg %", "pipeline CPU (% of 1 core)", "webview2 CPU (% of 1 core)", "GPU power avg W", "detections/s (cam1 ppe, idss, cam2 ppe, idss)"],
                       [rr("2 x full (Live)", dual), rr("2 x small (Fleet tiles)", tile)]),
              "", f"Video elements on the page: full `{dual['media']['video_elements']}`, small `{tile['media']['video_elements']}`.", ""]
    if dual:
        L += ["## Bottleneck analysis", ""] + [f"- {x}" for x in bottleneck(base, dual)] + [""]
    if notes:
        L += ["## Notes", ""] + [f"- {n}" for n in notes] + [""]
    Path(A.out_md).write_text("\n".join(L), encoding="utf-8")
    log(f"report written: {A.out_md}")


# ---------------------------------------------------------------------------------------------------- main
def main() -> int:
    global A
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--duration", type=int, default=240, help="measured seconds per phase")
    ap.add_argument("--warmup", type=int, default=60, help="warm-up seconds before each measured phase")
    ap.add_argument("--interval", type=float, default=2.0, help="resource sampling period (s)")
    ap.add_argument("--cameras", default="1,2,tile", help="phases to run: 1 (baseline), 2 (dual, Live page = full profile), tile (dual on the Fleet page = small profile); default 1,2,tile")
    ap.add_argument("--uri", default=RTSP_URI)
    ap.add_argument("--port", type=int, default=9341)
    ap.add_argument("--seed-dir", default=str(RUNS / "app_evidence"), help="installed layout to copy (gst, gpu, models, ...), without BitNet")
    ap.add_argument("--app-dir", default=str(RUNS / "app_dual"), help="private app folder created for this run")
    ap.add_argument("--local-dir", default=str(RUNS / "dual_local"), help="fresh LOCALAPPDATA for the app")
    ap.add_argument("--out-md", default=str(HERE / "DUAL_STREAM_REPORT.md"))
    ap.add_argument("--out-json", default=str(HERE / "dual_stream_samples.json"))
    ap.add_argument("--keep", action="store_true", help="keep the app copy and the data folder")
    A = ap.parse_args()
    phases = [x.strip() for x in A.cameras.split(",") if x.strip()]  # "1" baseline, "2" dual (Live page, full), "tile" (2 cameras on the Fleet page, small)
    from playwright.sync_api import sync_playwright

    app_dir, local = Path(A.app_dir), Path(A.local_dir)
    rmtree_safe(local)
    local.mkdir(parents=True, exist_ok=True)
    meta = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "host": os.environ.get("COMPUTERNAME", "?"), "ram_total_mb": round(psutil.virtual_memory().total / 1048576), "gpu_name": "n/a"}
    if NVSMI:
        try:
            meta["gpu_name"] = subprocess.run([NVSMI, "--query-gpu=name,driver_version", "--format=csv,noheader"], capture_output=True, text=True, timeout=10).stdout.strip()
        except Exception:  # noqa: BLE001
            pass
    result: dict = {"meta": meta, "args": vars(A), "phases": {}}
    notes: list[str] = []
    analysed: dict[str, dict] = {}
    try:
        stage_app()
        launch_app(app_dir, local)
        with sync_playwright() as pw:
            browser = None
            for _ in range(90):
                try:
                    browser = pw.chromium.connect_over_cdp(f"http://127.0.0.1:{A.port}")
                    pages = [p for c in browser.contexts for p in c.pages if "rigmind.app" in p.url]
                    if pages:
                        break
                    browser.close()
                    browser = None
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(1)
            if not browser:
                raise RuntimeError("cannot attach to the app window over CDP")
            pg = pages[0]
            log(f"attached to {pg.url}")
            mode = sign_in(pg)
            log(f"sign-in: {mode}")
            pg.evaluate(COLLECTOR_JS)
            mon = Monitor()
            for n in phases:
                ph = run_phase(pg, mon, n)
                analysed[n] = analyse(ph)
                result["phases"][n] = {"analysis": analysed[n], "samples": ph["samples"], "online": ph["online"],
                                            "history": ph["hist"], "raw_events": {k: v for k, v in ph["raw"].items() if k in ("inf", "stat", "stats", "alarm", "err", "facts", "ppe", "lat", "cnt")},
                                            "detection_times_ms": {k: [r[0] for r in v] for k, v in ph["raw"]["det"].items()}}
                if not ph["online"]["online"]:
                    notes.append(f"phase {n}: not every camera came online within 120 s: {ph['online']['states']}")
            browser.close()
    except Exception as e:  # noqa: BLE001
        log(f"FAILED: {type(e).__name__}: {e}")
        result["error"] = traceback.format_exc()[-2000:]
        notes.append(f"run aborted: {type(e).__name__}: {e}")
    finally:
        stop_app()
        if not A.keep:
            rmtree_safe(local)
            rmtree_safe(app_dir)
            log("temporary data and app copy deleted")
    base, dual, tile = analysed.get("1"), analysed.get("2"), analysed.get("tile")
    V = verdicts(base, dual) if dual else []
    provs = {m: (dual or base)["per_cam"][CAMS[0]]["models"][m]["provider"] for m in MODELS} if (dual or base) else {}
    meta["providers"] = ", ".join(f"{m}={p}" for m, p in provs.items()) or "n/a"
    result["verdicts"] = V
    Path(A.out_json).write_text(json.dumps(result, indent=1, default=str), encoding="utf-8")
    write_report(meta, base, dual, V, notes, tile)
    for v in V:
        print(f"  [{v['status']}] {v['name']} -- {v['evidence']}")
    return 1 if any(v["status"] == "FAIL" for v in V) or "error" in result else 0


if __name__ == "__main__":
    sys.exit(main())
