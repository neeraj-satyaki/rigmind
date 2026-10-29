#!/usr/bin/env python
"""RigMind performance / functional validation harness (visible app, real RTSP stream, no code or threshold changes).

    python tests\\validation\\perf_validate.py                       # full run (waits for an idle machine, >= 300 s stream measurement)
    python tests\\validation\\perf_validate.py --duration 60 --skip-wait --no-restart   # short dry run

What it does (all in a VISIBLE RigMind window driven through Playwright connect_over_cdp):
  0  copy the build into the scratch installed-layout app dir (keeps its gst\\ gpu\\ models\\), fresh LOCALAPPDATA, debug port
  1  INFERENCE      per model fps / avg_ms / provider / conf / iou from 'inference' events, detections/s, class counts, polygons
  2  FPS + DROPS    pipeline 'stats' + <video>.getVideoPlaybackQuality + RTCPeerConnection.getStats inbound-rtp
  3  LATENCY        host processing latency (detections.latency_ms), rtp/pts <-> requestVideoFrameCallback matching, page decode/jitter
  4  BANDWIDTH      WebRTC kbps, RTSP kbps, bridge message rate/bytes, CPU/RAM of RigMind / pipeline / msedgewebview2, GPU (nvidia-smi)
  5  ROIs           platform ROI geometry + in_roi check from events; user zones: add / edit / save / reload / records.get / delete
  6  EXPERT OUTPUTS PPE + IDSS classes, facts, alarms, audit rows, alerts.feed, footage rows + files + playback, cross-checks
  7  SUMMARIES      event.describe (BitNet / template), grounding, latency, UI rendering
  8  PAGES/REPORTS  every route, every printable report, sample-string scan, settings change / restart / reset
Output: tests\\validation\\VALIDATION_REPORT.md and tests\\validation\\perf_validate_raw.json (+ screenshots\\).
Only processes started by this script (and their children) are ever stopped.
"""
from __future__ import annotations

import argparse
import base64
import collections
import ctypes
import datetime as dt
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
import urllib.request
from pathlib import Path

import psutil

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]  # RigMind_Desktop
BUILD = REPO / "build-app" / "Release"
DEFAULT_SEED = Path(r"C:\Users\iotwi\.claude\jobs\7d6a52c9\tmp\app")  # shared installed-layout scratch app (another agent also runs from it)
DEFAULT_APP = Path(r"C:\Users\iotwi\.claude\jobs\7d6a52c9\tmp\app_val")  # this harness runs from its own private copy (junctions to gst, gpu)
DEFAULT_RUNROOT = Path(r"C:\Users\iotwi\.claude\jobs\7d6a52c9\tmp")
RTSP_URI = "rtsp://192.168.1.8:8556/feed0"
SAMPLE_STRINGS = ["EV-1042", "Faisal A.", "Maha S.", "Demo rig 01", "faisal.demo@example.com", "Faisal", "Maha S"]
ERR_PAGE_RE = re.compile(r"Page not found|\b404\b|This page didn't load|Something went wrong|Not Found", re.I)
BENIGN_CONSOLE = re.compile(r"fonts\.(googleapis|gstatic)|favicon|ERR_INTERNET_DISCONNECTED|ERR_NAME_NOT_RESOLVED|net::ERR_FAILED.*font|Autofocus", re.I)
TEST_EMAIL = "validate.operator@rigmind-validation.test"
NOTIFY_EMAIL = "validate.notify@rigmind-validation.test"
OUT_FPS = 12.5
FRAME_MS = 1000.0 / OUT_FPS
FACT_NAMES = ["machine_present", "link_tilt_present", "link_tilt_angled", "elevator_present", "elevator_near_platform",
              "platform_elevator_collision", "derrickman_out_of_safe_zone", "machine_past_right_edge", "link_tilt_near_derrickman"]

A = None  # parsed args
R: dict = {"meta": {}, "errors": [], "checks": [], "console": [], "notes": []}
SHOTS: Path
PACE = 1.0


# ------------------------------------------------------------------------------------------------ small helpers
def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def pause(s: float = 1.0) -> None:
    time.sleep(s * PACE)


def pctl(vals, p):
    v = sorted(x for x in vals if x is not None and not (isinstance(x, float) and math.isnan(x)))
    if not v:
        return None
    k = (len(v) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] if lo == hi else v[lo] + (v[hi] - v[lo]) * (k - lo)


def dist(vals, nd=2):
    v = [x for x in vals if x is not None and not (isinstance(x, float) and math.isnan(x))]
    if not v:
        return {"n": 0, "min": None, "median": None, "p95": None, "max": None, "mean": None}
    r = lambda x: round(x, nd)  # noqa: E731
    return {"n": len(v), "min": r(min(v)), "median": r(statistics.median(v)), "p95": r(pctl(v, 95)), "max": r(max(v)), "mean": r(statistics.fmean(v))}


def check(section: str, name: str, ok, source: str, evidence: str = "") -> None:
    status = "PASS" if ok is True else "FAIL" if ok is False else "NOTE"
    R["checks"].append({"section": section, "name": name, "status": status, "source": source, "evidence": evidence})
    log(f"   [{status}] {name}" + (f" -- {evidence}" if evidence else ""))


def guarded(name: str, fn, *a, **kw):
    try:
        return fn(*a, **kw)
    except Exception as e:  # noqa: BLE001
        tb = traceback.format_exc()
        R["errors"].append({"phase": name, "error": f"{type(e).__name__}: {e}", "trace": tb[-1500:]})
        log(f"   !! phase {name} failed: {type(e).__name__}: {e}")
        return None


def shot(page, name: str) -> str:
    p = SHOTS / f"{name}.png"
    try:
        page.screenshot(path=str(p))
    except Exception as e:  # noqa: BLE001
        log(f"   (screenshot failed: {e})")
    return str(p)


# ------------------------------------------------------------------------------------------------ in-page instrumentation
INIT_JS = r"""
(() => {
  if (window.__pv) return;
  const pv = window.__pv = { pcs: [], det: [], other: [], rvfc: [], cnt: {}, bytes: {}, msgN: 0, msgBytes: 0, replies: {}, vid: null, origin: performance.timeOrigin };
  const Orig = window.RTCPeerConnection;
  if (Orig) {
    const W = function (...a) { const pc = new Orig(...a); pv.pcs.push(pc); return pc; };
    W.prototype = Orig.prototype; Object.setPrototypeOf(W, Orig); window.RTCPeerConnection = W;
  }
  const hook = () => {
    try {
      window.chrome.webview.addEventListener('message', (e) => {
        const d = e.data; if (!d) return;
        if (typeof d.id === 'number' && !d.ev) { if (d.id >= 1e6) pv.replies[d.id] = d; return; }
        if (!d.ev) return;
        const now = performance.timeOrigin + performance.now();
        const sz = JSON.stringify(d).length;
        pv.cnt[d.ev] = (pv.cnt[d.ev] || 0) + 1; pv.bytes[d.ev] = (pv.bytes[d.ev] || 0) + sz; pv.msgN++; pv.msgBytes += sz;
        if (d.ev === 'detections') {
          const items = d.items || [];
          pv.det.push({ m: d.model, recv: now, ts: d.ts, pts: d.pts_ms, rtp: d.rtp, lat: d.latency_ms, ms: d.ms, w: d.w, h: d.h, sz,
            n: items.length, roi: d.roi === undefined ? undefined : d.roi, platform: d.platform ? d.platform.box : (d.platform === null ? null : undefined),
            items: items.map(i => ({ l: i.label, b: i.box, c: i.conf, r: i.in_roi, p: i.polygon ? i.polygon.length : 0 })) });
        } else if (d.ev !== 'answer') {
          pv.other.push(Object.assign({ recv: now, sz }, d));
        }
      });
    } catch (err) { setTimeout(hook, 50); }
  };
  hook();
  let nextId = 1e6;
  window.__pvCall = (method, params, timeout) => new Promise((resolve) => {
    const id = nextId++; const t0 = performance.now();
    const iv = setInterval(() => {
      const r = pv.replies[id];
      if (r) { clearInterval(iv); delete pv.replies[id]; resolve({ ok: r.ok, result: r.result, error: r.error, ms: performance.now() - t0 }); }
      else if (performance.now() - t0 > (timeout || 15000)) { clearInterval(iv); resolve({ ok: false, error: 'timeout', ms: performance.now() - t0 }); }
    }, 10);
    try { window.chrome.webview.postMessage(JSON.stringify({ id, method, params: params || {} })); } catch (e) { clearInterval(iv); resolve({ ok: false, error: String(e) }); }
  });
  const attach = () => {
    const v = document.querySelector('video');
    if (v && v !== pv.vid && v.requestVideoFrameCallback) {
      pv.vid = v;
      const cb = (now, md) => {
        const o = performance.timeOrigin;
        pv.rvfc.push([o + now, o + md.presentationTime, o + md.expectedDisplayTime,
          md.receiveTime != null ? o + md.receiveTime : null, md.captureTime != null ? o + md.captureTime : null,
          md.rtpTimestamp != null ? md.rtpTimestamp : null, md.presentedFrames, md.processingDuration != null ? md.processingDuration * 1000 : null, md.width, md.height]);
        if (pv.vid === v) v.requestVideoFrameCallback(cb);
      };
      v.requestVideoFrameCallback(cb);
    }
  };
  setInterval(attach, 400);
  window.__pvSample = async () => {
    const out = { t: Date.now(), msg: { n: pv.msgN, bytes: pv.msgBytes, cnt: Object.assign({}, pv.cnt), by: Object.assign({}, pv.bytes) } };
    const v = document.querySelector('video');
    if (v) {
      const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
      out.vq = { total: q.totalVideoFrames, dropped: q.droppedVideoFrames, corrupted: q.corruptedVideoFrames, w: v.videoWidth, h: v.videoHeight, paused: v.paused, ct: v.currentTime, rs: v.readyState };
    }
    for (const pc of pv.pcs.slice().reverse()) {
      if (pc.connectionState === 'closed') continue;
      let st; try { st = await pc.getStats(); } catch (e) { continue; }
      let inb = null, pair = null;
      st.forEach((r) => {
        if (r.type === 'inbound-rtp' && (r.kind === 'video' || r.mediaType === 'video')) inb = r;
        if (r.type === 'candidate-pair' && (r.nominated || r.state === 'succeeded') && r.currentRoundTripTime !== undefined) pair = r;
      });
      if (inb) {
        const o = {}; for (const k in inb) { const x = inb[k]; if (typeof x === 'number' || typeof x === 'string' || typeof x === 'boolean') o[k] = x; }
        out.inb = o; out.pcState = pc.connectionState;
        if (pair) out.pair = { rtt: pair.currentRoundTripTime, avail: pair.availableIncomingBitrate, state: pair.state };
        break;
      }
    }
    return out;
  };
  window.__pvDrain = () => {
    const r = { det: pv.det, other: pv.other, rvfc: pv.rvfc };
    pv.det = []; pv.other = []; pv.rvfc = [];
    return r;
  };
})();
"""


def call(page, method: str, params=None, timeout=15000):
    r = page.evaluate("([m,p,t])=>window.__pvCall(m,p,t)", [method, params or {}, timeout])
    return r


def call_ok(page, method, params=None, timeout=15000):
    r = call(page, method, params, timeout)
    if not r.get("ok"):
        raise RuntimeError(f"bridge {method} failed: {r.get('error')}")
    return r["result"]


# ------------------------------------------------------------------------------------------------ process control (scratch app only)
APP: dict = {"proc": None, "dir": None, "local": None}


def other_rigmind_procs() -> list[str]:
    """RigMind.exe / rigmind-pipeline.exe processes that are NOT in our scratch app tree."""
    mine = set()
    if APP["proc"] is not None:
        try:
            p = psutil.Process(APP["proc"].pid)
            mine = {p.pid} | {c.pid for c in p.children(recursive=True)}
        except psutil.Error:
            pass
    out = []
    for p in psutil.process_iter(["name", "pid", "exe"]):
        try:
            n = (p.info["name"] or "").lower()
            if n in ("rigmind.exe", "rigmind-pipeline.exe", "llama-server.exe") and p.info["pid"] not in mine:
                out.append(f"{n}#{p.info['pid']}")
        except psutil.Error:
            pass
    return out


def wait_idle() -> dict:
    t0 = time.time()
    info = {"waited_s": 0, "contended": False, "seen": []}
    if A.skip_wait:
        info["note"] = "wait skipped (--skip-wait)"
        info["seen"] = other_rigmind_procs()
        info["contended"] = bool(info["seen"])
        return info
    while time.time() - t0 < A.wait_idle_max:
        busy = other_rigmind_procs()
        if not busy:
            info["waited_s"] = round(time.time() - t0)
            log(f"machine idle after {info['waited_s']} s")
            return info
        info["seen"] = busy
        log(f"waiting for other RigMind processes to finish: {busy} ({int(time.time() - t0)} s)")
        time.sleep(20)
    info["waited_s"] = round(time.time() - t0)
    info["contended"] = True
    log("still busy after the wait limit: measuring under CONTENTION")
    return info


def stage_app() -> None:
    app = Path(A.app_dir)
    app.mkdir(parents=True, exist_ok=True)
    if not (app / "gst").exists():  # private copy of the shared scratch app: gst and gpu are junctions (no 2 GB copy, never written)
        seed = Path(A.seed_dir)
        for item in seed.iterdir():
            if item.name in ("gst", "gpu"):
                subprocess.run(["cmd", "/c", "mklink", "/J", str(app / item.name), str(item)], check=True, capture_output=True)
            elif item.name != "web" and not (app / item.name).exists():
                if item.is_dir():
                    shutil.copytree(item, app / item.name)
                else:
                    shutil.copy2(item, app / item.name)
        log(f"created private app dir {app} from {seed}")
    for f in ("RigMind.exe", "rigmind-pipeline.exe", "librigmindews.dll"):
        shutil.copy2(BUILD / f, app / f)
    if (app / "web").exists():
        shutil.rmtree(app / "web")
    webdir = REPO / "web"  # fresh vite bundle staged by scripts\stage_web.ps1 (build-app\Release\web was stale once)
    shutil.copytree(webdir, app / "web")
    hits = 0
    for f in (app / "web").rglob("*"):
        if f.suffix in (".js", ".html", ".mjs"):
            try:
                if "__RIGMIND_CONFIG__" in f.read_text(encoding="utf-8", errors="ignore"):
                    hits += 1
            except Exception:  # noqa: BLE001
                pass
    R["meta"]["web_bundle_has_RIGMIND_CONFIG"] = hits
    if hits == 0:
        raise RuntimeError(f"web bundle {webdir} does not contain '__RIGMIND_CONFIG__': stale bundle, refusing to measure")
    log(f"web bundle fresh: '__RIGMIND_CONFIG__' found in {hits} files")
    R["meta"]["staged_from"] = f"{BUILD} (exe/dll) + {webdir} (web)"
    R["meta"]["staged_hashes"] = {f: f"{(app / f).stat().st_size}B" for f in ("RigMind.exe", "rigmind-pipeline.exe", "librigmindews.dll")}
    R["meta"]["slm_present"] = (app / "slm" / "llama-server.exe").exists() and (app / "slm" / "ggml-model-i2_s.gguf").exists()
    log(f"staged build into {app}")


def launch_app() -> subprocess.Popen:
    exe = Path(A.app_dir) / "RigMind.exe"
    env = {**os.environ, "LOCALAPPDATA": str(APP["local"]),
           "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS": f"--remote-debugging-port={A.port} --remote-allow-origins=*"}
    log(f"launching {exe} (visible), LOCALAPPDATA={APP['local']}, debug port {A.port}")
    APP["proc"] = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env)
    return APP["proc"]


def _close_windows(pid: int) -> None:
    user32 = ctypes.windll.user32
    WM_CLOSE = 0x0010
    cb_t = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(h, _):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            user32.PostMessageW(h, WM_CLOSE, 0, 0)
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
        proc.wait(timeout=15)
    except Exception:  # noqa: BLE001
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            pass
    for k in kids:  # only our own children
        try:
            if k.is_running() and (k.name() or "").lower() == "rigmind-pipeline.exe":
                k.terminate()
        except psutil.Error:
            pass
    APP["proc"] = None
    time.sleep(2)


# ------------------------------------------------------------------------------------------------ browser session
class Session:
    def __init__(self, pw, label: str):
        self.pw, self.label = pw, label
        self.browser = None
        self.page = None
        self.where = "boot"
        self.errors: list[dict] = []

    def connect(self, timeout=90) -> bool:
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{A.port}/json/version", timeout=2).read()
            except Exception as e:  # noqa: BLE001
                last = e
                time.sleep(1)
                continue
            try:
                self.browser = self.pw.chromium.connect_over_cdp(f"http://127.0.0.1:{A.port}")
                pages = [p for c in self.browser.contexts for p in c.pages if "rigmind.app" in p.url]
                if pages:
                    self.page = pages[-1]
                    self.page.on("console", self._console)
                    self.page.on("pageerror", lambda e: self._err("pageerror", str(e)))
                    self.page.context.add_init_script(INIT_JS)
                    return True
            except Exception as e:  # noqa: BLE001
                last = e
            time.sleep(1)
        log(f"CDP connect failed: {last}")
        return False

    def _err(self, kind, text):
        rec = {"where": f"{self.label}:{self.where}", "kind": kind, "text": text[:300], "benign": bool(BENIGN_CONSOLE.search(text))}
        self.errors.append(rec)
        R["console"].append(rec)

    def _console(self, m):
        if m.type == "error":
            self._err("console.error", m.text or "")

    def disconnect(self):
        try:
            if self.browser:
                self.browser.close()
        except Exception:  # noqa: BLE001
            pass
        self.browser = self.page = None

    def body(self) -> str:
        try:
            return self.page.inner_text("body", timeout=5000)
        except Exception:  # noqa: BLE001
            return ""

    def path(self) -> str:
        return self.page.evaluate("location.pathname")

    def hl(self, loc, ms=700):
        try:
            loc.scroll_into_view_if_needed(timeout=3000)
            loc.evaluate("(el,ms)=>{const o=el.style.outline;el.style.outline='3px solid #ff2d95';setTimeout(()=>{el.style.outline=o},ms)}", int(ms * PACE))
        except Exception:  # noqa: BLE001
            pass

    def click(self, loc, wait=0.8):
        self.hl(loc)
        pause(0.5)
        loc.click(timeout=8000)
        pause(wait)

    def fill(self, loc, text, wait=0.5):
        self.hl(loc)
        loc.click(timeout=8000)
        loc.fill("")
        loc.type(text, delay=int(25 * PACE))
        pause(wait)

    def banner(self, text):
        try:
            self.page.evaluate(
                """t=>{let b=document.getElementById('__pv_banner');if(!b){b=document.createElement('div');b.id='__pv_banner';
                b.style.cssText='position:fixed;left:50%;bottom:10px;transform:translateX(-50%);z-index:2147483647;background:#111c;color:#fff;font:600 14px Segoe UI,sans-serif;padding:8px 16px;border-radius:8px;pointer-events:none;border:2px solid #ff2d95';document.body.appendChild(b)}b.textContent=t}""", text)
        except Exception:  # noqa: BLE001
            pass

    def go(self, route: str):
        self.page.evaluate("r=>{history.pushState({},'',r);dispatchEvent(new PopStateEvent('popstate'))}", route)
        pause(1.0)

    def ensure_shell(self):
        pg = self.page
        for _ in range(3):
            if pg.locator("aside").count():
                return
            pg.evaluate("history.back()")
            pause(1.0)
        if not pg.locator("aside").count():
            self.go("/fleet")

    def nav(self, label: str, admin: bool = False):
        self.where = f"nav:{label}"
        pg = self.page
        self.ensure_shell()
        rx = re.compile(rf"^\s*{re.escape(label)}(\s*\d+)?\s*$")
        if admin and pg.locator("aside a", has_text=rx).count() == 0:
            self.click(pg.locator("aside button", has_text="Administration").first)
        self.click(pg.locator("aside a", has_text=rx).first)
        pause(0.8)

    def login(self):
        pg = self.page
        pg.wait_for_selector("text=/Sign in|Welcome back/i", timeout=60000)
        email = pg.get_by_label("Work email").first if pg.get_by_label("Work email").count() else pg.locator("input").first
        try:
            if not email.input_value():
                self.fill(email, TEST_EMAIL)
        except Exception:  # noqa: BLE001
            pass
        self.click(pg.get_by_role("button", name="Sign in to workspace").first)
        pg.wait_for_selector("aside", timeout=30000)
        pause(1)


def reload_and_login(S: Session):
    """Attach the in-page instrumentation and sign in. A real page reload is NOT used: the host's virtual host serves only '/',
    so location.reload() on a deep route gives ERR_FILE_NOT_FOUND and navigating to '/' over CDP gives ERR_ACCESS_DENIED (see probe_reload).
    The hooks are installed at run time instead; RTCPeerConnection is wrapped before the Live page creates its connection."""
    S.page.wait_for_selector("aside, :text-matches('Sign in|Welcome back', 'i')", timeout=60000)
    S.page.evaluate(INIT_JS)
    pause(1)
    if S.page.locator("aside").count() == 0:
        S.login()


def probe_reload(S: Session):
    """What a user can do: press F5 / Ctrl+R. The host disables browser accelerator keys (main.cpp AreBrowserAcceleratorKeysEnabled=FALSE), so the page must stay.
    A programmatic location.reload() is not user-reachable and is only recorded as information (it gives ERR_FILE_NOT_FOUND: the virtual host serves only '/')."""
    pg = S.page
    pg.evaluate("window.__f5marker = 42")
    pg.keyboard.press("F5")
    pause(2)
    pg.keyboard.press("Control+R")
    pause(2)
    kept = pg.evaluate("window.__f5marker") == 42
    R["reload_probe"] = {"f5_ctrl_r_kept_page": kept, "url": pg.url, "programmatic_reload": "ERR_FILE_NOT_FOUND on deep routes (information only; not user-reachable)"}
    check("8 PAGES", "F5 / Ctrl+R inside the app do not break the app (accelerator keys disabled)", kept, "main.cpp AreBrowserAcceleratorKeysEnabled(FALSE)", f"page state kept={kept} url={pg.url}")


# ------------------------------------------------------------------------------------------------ resource monitor
class ProcMon:
    def __init__(self):
        self.cache: dict[int, psutil.Process] = {}
        self.seen: set[int] = set()
        self.rows: list[dict] = []
        self.ncpu = psutil.cpu_count() or 1

    def _procs(self):
        procs = {}
        if APP["proc"] is not None:
            try:
                root = psutil.Process(APP["proc"].pid)
                for p in [root] + root.children(recursive=True):
                    procs[p.pid] = p
            except psutil.Error:
                pass
        appdir = str(Path(A.app_dir)).lower()
        for p in psutil.process_iter(["name", "exe"]):
            try:
                if (p.info["exe"] or "").lower().startswith(appdir):
                    procs.setdefault(p.pid, p)
            except psutil.Error:
                pass
        return procs

    def sample(self):
        t = time.time()
        groups = collections.defaultdict(lambda: {"cpu": 0.0, "rss": 0, "n": 0})
        for pid, p in self._procs().items():
            try:
                c = self.cache.setdefault(pid, p)
                name = (c.name() or "").lower()
                cpu = c.cpu_percent(None)
                first = pid not in self.seen
                self.seen.add(pid)
                g = groups[name]
                g["rss"] += c.memory_info().rss
                g["n"] += 1
                if not first:
                    g["cpu"] += cpu
            except psutil.Error:
                pass
        self.rows.append({"t": t, "g": {k: dict(v) for k, v in groups.items()}, "sys_cpu": psutil.cpu_percent(None), "sys_free_mb": psutil.virtual_memory().available / 1048576})

    def summary(self) -> dict:
        names = sorted({n for r in self.rows for n in r["g"]})
        out = {}
        for n in names:
            cpu = [r["g"][n]["cpu"] for r in self.rows if n in r["g"]]
            rss = [r["g"][n]["rss"] / 1048576 for r in self.rows if n in r["g"]]
            out[n] = {"cpu_pct_of_one_core": dist(cpu, 1), "cpu_pct_of_machine": dist([c / self.ncpu for c in cpu], 1), "rss_mb": dist(rss, 0)}
        out["_system"] = {"cpu_pct": dist([r["sys_cpu"] for r in self.rows], 1), "free_ram_mb": dist([r["sys_free_mb"] for r in self.rows], 0), "logical_cpus": self.ncpu}
        return out


NVSMI = next((p for p in (r"C:\Windows\System32\nvidia-smi.exe", r"C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe") if Path(p).exists()), None)


def gpu_sample():
    if not NVSMI:
        return None
    try:
        out = subprocess.run([NVSMI, "--query-gpu=utilization.gpu,utilization.memory,memory.used,memory.total,power.draw,temperature.gpu,utilization.encoder,utilization.decoder",
                              "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=10).stdout.strip().splitlines()[0]
        v = [float(x) if re.match(r"^-?[\d.]+$", x.strip()) else None for x in out.split(",")]
        return {"t": time.time(), "util": v[0], "mem_util": v[1], "mem_used_mb": v[2], "mem_total_mb": v[3], "power_w": v[4], "temp_c": v[5], "enc": v[6], "dec": v[7]}
    except Exception:  # noqa: BLE001
        return None


# ------------------------------------------------------------------------------------------------ first launch: app, camera, live
def add_camera(S: Session) -> None:
    pg = S.page
    S.banner("Validation: add the camera (Administration > Cameras)")
    S.nav("Cameras", admin=True)
    for label, value in {"Camera name": "Validation Camera", "Stream URI": A.uri}.items():
        loc = pg.get_by_label(label).first if pg.get_by_label(label).count() else pg.locator(f"label:has-text('{label}') input").first
        S.fill(loc, value)
    ign = pg.get_by_role("button", name=re.compile(r"^\s*Ignore\s*$", re.I))
    if ign.count():
        S.click(ign.first)
    S.click(pg.get_by_role("button", name="Continue").first)
    run = pg.get_by_role("button", name=re.compile(r"(Run stream validation|Revalidate|Validate)", re.I))
    if run.count():
        S.click(run.first, wait=2.5)
    pause(1)
    for _ in range(90):  # validation can be slow when the machine is loaded
        if pg.get_by_role("button", name="Continue").first.is_enabled():
            break
        time.sleep(1)
    S.click(pg.get_by_role("button", name="Continue").first)
    S.click(pg.get_by_role("button", name=re.compile(r"Save camera|Camera saved")).first, wait=2)
    shot(pg, "01-camera-saved")


def drain(page, store: dict) -> None:
    d = page.evaluate("window.__pvDrain()")
    store["det"].extend(d["det"])
    store["other"].extend(d["other"])
    store["rvfc"].extend(d["rvfc"])


def wait_video(page, timeout=120) -> dict | None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = page.evaluate("(()=>{const v=document.querySelector('video');return v?{w:v.videoWidth,h:v.videoHeight,t:v.currentTime,paused:v.paused}:null})()")
        if v and v["w"] > 0 and not v["paused"]:
            return {**v, "after_s": round(time.time() - t0, 1)}
        time.sleep(1)
    return None


DATA: dict = {"det": [], "other": [], "rvfc": [], "samples": [], "t_start": None, "t_end": None}
MON = ProcMon()
GPU: list = []


def measure(S: Session) -> None:
    """The stream measurement window on the Live page: samples getStats/video quality 1 Hz, processes 2 s, GPU 5 s, events always."""
    pg = S.page
    S.where = "measure"
    S.nav("Live monitoring")
    play = wait_video(pg, 150)
    R["live_first_play"] = play
    check("2 FPS", "video element starts playing on /live", bool(play), "PRD: live view", json.dumps(play))
    if not play:
        raise RuntimeError("no video: cannot measure")
    shot(pg, "02-live-playing")
    # wait for both models to report (models load first)
    t0 = time.time()
    while time.time() - t0 < 120:
        drain(pg, DATA)
        seen = {o.get("model") for o in DATA["other"] if o.get("ev") == "inference"}
        if {"ppe", "idss"} <= seen:
            break
        time.sleep(2)
    warm = max(0, A.warmup - (time.time() - t0))
    log(f"models reporting; warm-up {warm:.0f} s more")
    time.sleep(warm)
    S.banner(f"Validation: measuring the live stream for {A.duration} s")
    base = pg.evaluate("window.__pvSample()")
    DATA["t_start"] = base["t"]
    DATA["base"] = base
    t_end = time.time() + A.duration
    nxt = time.time()
    i = 0
    while time.time() < t_end:
        if APP["proc"] is not None and APP["proc"].poll() is not None:
            raise RuntimeError(f"RigMind.exe exited during the measurement with code {APP['proc'].returncode} (killed from outside? other RigMind processes: {other_rigmind_procs()})")
        s = pg.evaluate("window.__pvSample()")
        DATA["samples"].append(s)
        if i % 2 == 0:
            MON.sample()
        if i % 5 == 0:
            oth = other_rigmind_procs()
            if oth:
                DATA.setdefault("contention", []).append({"t": time.time(), "procs": oth})
            g = gpu_sample()
            if g:
                GPU.append(g)
            drain(pg, DATA)
        if i % 60 == 0:
            log(f"   measuring... {int(time.time() - (t_end - A.duration))}/{A.duration} s, video {s.get('vq', {}).get('w')}x{s.get('vq', {}).get('h')}")
        i += 1
        nxt += 1.0
        time.sleep(max(0, nxt - time.time()))
    drain(pg, DATA)
    DATA["t_end"] = int(time.time() * 1000)
    DATA["origin_note"] = "all timestamps are absolute epoch ms (page performance.timeOrigin + performance.now, same machine clock as the pipeline wall_ms)"
    shot(pg, "03-live-after-measure")


# ------------------------------------------------------------------------------------------------ analysis of the measurement
def analyze_measurement() -> None:
    t0 = DATA["t_start"]
    other = DATA["other"]
    dets = [d for d in DATA["det"] if d["recv"] >= t0]
    samples = DATA["samples"]
    cfg = R.get("inference_cfg", {})
    models_cfg = cfg.get("models", {})

    # ---- 1 inference
    inf = collections.defaultdict(list)
    for o in other:
        if o.get("ev") == "inference" and o["recv"] >= t0:
            inf[o["model"]].append(o)
    sec1 = {}
    dur = max(1.0, (DATA["t_end"] - t0) / 1000.0)
    for m in ("ppe", "idss"):
        evs = inf.get(m, [])
        dm = [d for d in dets if d["m"] == m]
        cls = collections.Counter(i["l"] for d in dm for i in d["items"])
        frames_with = collections.Counter(l for d in dm for l in {i["l"] for i in d["items"]})
        poly_items = sum(1 for d in dm for i in d["items"] if i["p"] > 0)
        items_total = sum(d["n"] for d in dm)
        mc = models_cfg.get(m, {})
        sec1[m] = {
            "fps": dist([e["fps"] for e in evs], 1), "avg_ms": dist([e["avg_ms"] for e in evs], 1),
            "providers": sorted({str(e.get("provider")) for e in evs}), "provider_note": sorted({str(e.get("provider_note")) for e in evs if e.get("provider_note")})[:2],
            "load_s": sorted({e.get("load_s") for e in evs})[:3],
            "conf_events": sorted({e.get("conf") for e in evs}), "iou_events": sorted({e.get("iou") for e in evs}),
            "conf_cfg": mc.get("conf"), "iou_cfg": mc.get("iou"), "scale_mode": sorted({str(e.get("scale_mode")) for e in evs}), "scale": sorted({e.get("scale") for e in evs if e.get("scale") is not None}),
            "detection_events": len(dm), "detection_events_per_s": round(len(dm) / dur, 2), "items_per_s": round(items_total / dur, 2),
            "items_per_event": dist([d["n"] for d in dm], 1),
            "class_counts": dict(cls.most_common()), "frames_with_class": dict(frames_with.most_common()),
            "polygon_items": poly_items, "items_total": items_total, "burned_in": sorted({str(e.get("burned_in")) for e in evs}),
        }
    R["s1_inference"] = sec1
    cont = sorted({p for c in DATA.get("contention", []) for p in c["procs"]})
    R["contention_during_measure"] = cont
    if cont:
        R["notes"].append(f"CONTENTION DURING THE MEASUREMENT: other RigMind processes were running ({cont}) in {len(DATA['contention'])} of the 5-s checks; GPU/camera shared, numbers are not clean.")

    # ---- 2 fps / drops
    st = [o for o in other if o.get("ev") == "stats" and o["recv"] >= t0]
    s2 = {"pipeline_stats": {"fps": dist([o["fps"] for o in st], 2), "kbps": dist([o["kbps"] for o in st], 0), "n": len(st)}}
    if st:
        last, first = st[-1], st[0]
        s2["pipeline_stats"].update({
            "reconnects_total": last.get("reconnects"), "resyncs_start": first.get("resyncs"), "resyncs_end": last.get("resyncs"), "resyncs_in_window": (last.get("resyncs") or 0) - (first.get("resyncs") or 0),
            "keyframes_start": first.get("keyframes"), "keyframes_end": last.get("keyframes"),
            "overlay_frames_end": last.get("overlay_frames"), "overlay_drawn_end": last.get("overlay_drawn"), "overlay_avg_ms_end": round(last.get("overlay_avg_ms", 0), 3),
            "overlay_avg_ms": dist([o.get("overlay_avg_ms") for o in st], 3), "gate_open": sorted({str(o.get("gate_open")) for o in st}), "live": sorted({str(o.get("live")) for o in st})})
    stat_events = [o for o in other if o.get("ev") == "status"]
    s2["status_events"] = [{k: o.get(k) for k in ("state", "reason", "width", "height", "fps")} for o in stat_events][-6:]
    b, e = DATA["base"], samples[-1] if samples else DATA["base"]
    if samples and e.get("inb") and b.get("inb"):
        bi, ei = b["inb"], e["inb"]
        d = lambda k: (ei.get(k) or 0) - (bi.get(k) or 0)  # noqa: E731
        sec = (e["t"] - b["t"]) / 1000.0
        recv, dec, drp = d("framesReceived"), d("framesDecoded"), d("framesDropped")
        rtc = {"window_s": round(sec, 1), "framesReceived": recv, "framesDecoded": dec, "framesDropped": drp, "drop_pct": round(100.0 * drp / recv, 3) if recv else None,
               "avg_decoded_fps": round(dec / sec, 2) if sec else None, "framesPerSecond_reported": dist([s["inb"].get("framesPerSecond") for s in samples if s.get("inb")], 2),
               "freezeCount": d("freezeCount"), "totalFreezesDuration_s": round(d("totalFreezesDuration"), 3), "pauseCount": d("pauseCount"),
               "packetsLost": d("packetsLost"), "packetsReceived": d("packetsReceived"), "nackCount": d("nackCount"), "pliCount": d("pliCount"), "firCount": d("firCount"),
               "keyFramesDecoded": d("keyFramesDecoded"), "jitter_ms": dist([(s["inb"].get("jitter") or 0) * 1000 for s in samples if s.get("inb")], 2),
               "frameWidth": ei.get("frameWidth"), "frameHeight": ei.get("frameHeight"), "decoderImplementation": ei.get("decoderImplementation"),
               "loss_pct": round(100.0 * d("packetsLost") / max(1, d("packetsReceived") + d("packetsLost")), 4), "rtt_ms": dist([(s.get("pair") or {}).get("rtt", None) * 1000 if (s.get("pair") or {}).get("rtt") is not None else None for s in samples], 2),
               "pc_states": sorted({s.get("pcState") for s in samples if s.get("pcState")})}
        s2["rtc"] = rtc
        # per-second fps from framesDecoded
        fps_series, kbps_series, dec_ms, jb_ms = [], [], [], []
        for p, q in zip(samples, samples[1:]):
            if not (p.get("inb") and q.get("inb")):
                continue
            dt_s = (q["t"] - p["t"]) / 1000.0
            if dt_s <= 0.2:
                continue
            fps_series.append((q["inb"].get("framesDecoded", 0) - p["inb"].get("framesDecoded", 0)) / dt_s)
            kbps_series.append((q["inb"].get("bytesReceived", 0) - p["inb"].get("bytesReceived", 0)) * 8 / dt_s / 1000.0)
            nd = q["inb"].get("framesDecoded", 0) - p["inb"].get("framesDecoded", 0)
            if nd > 0:
                dec_ms.append((q["inb"].get("totalDecodeTime", 0) - p["inb"].get("totalDecodeTime", 0)) / nd * 1000)
            ne = q["inb"].get("jitterBufferEmittedCount", 0) - p["inb"].get("jitterBufferEmittedCount", 0)
            if ne > 0:
                jb_ms.append((q["inb"].get("jitterBufferDelay", 0) - p["inb"].get("jitterBufferDelay", 0)) / ne * 1000)
        s2["rtc_decoded_fps_per_s"] = dist(fps_series, 2)
        s4_kbps = dist(kbps_series, 0)
        s3_dec, s3_jb = dist(dec_ms, 2), dist(jb_ms, 1)
    else:
        s4_kbps = s3_dec = s3_jb = dist([])
        s2["rtc"] = None
    if samples and samples[-1].get("vq") and DATA["base"].get("vq"):
        bq, eq = DATA["base"]["vq"], samples[-1]["vq"]
        tot = (eq["total"] or 0) - (bq["total"] or 0)
        s2["video_element"] = {"total_frames": tot, "dropped": (eq["dropped"] or 0) - (bq["dropped"] or 0), "corrupted": (eq["corrupted"] or 0) - (bq["corrupted"] or 0),
                               "dropped_pct": round(100.0 * ((eq["dropped"] or 0) - (bq["dropped"] or 0)) / tot, 3) if tot else None, "video_w": eq["w"], "video_h": eq["h"],
                               "avg_presented_fps": round(tot / ((samples[-1]["t"] - DATA["base"]["t"]) / 1000.0), 2),
                               "paused_samples": sum(1 for s in samples if s.get("vq", {}).get("paused")), "lifetime_total": eq["total"], "lifetime_dropped": eq["dropped"], "lifetime_corrupted": eq["corrupted"]}
    # frame cadence from rVFC (presented frames)
    rv = [r for r in DATA["rvfc"] if r[1] >= t0]
    if len(rv) > 3:
        pres = sorted(r[1] for r in rv)
        gaps = [b2 - a for a, b2 in zip(pres, pres[1:])]
        s2["presented_cadence_ms"] = {"gap": dist(gaps, 1), "gaps_over_2x_frame": sum(1 for g in gaps if g > 2 * FRAME_MS * 1.1), "frames_seen": len(rv),
                                      "fps_from_rvfc": round((len(rv) - 1) / ((pres[-1] - pres[0]) / 1000.0), 2)}
    # presented-frame skipped by presentedFrames counter
    pf = [r[6] for r in rv if r[6] is not None]
    if len(pf) > 3:
        s2["rvfc_presentedFrames_span"] = pf[-1] - pf[0] + 1
        s2["rvfc_callbacks"] = len(pf)
        s2["rvfc_missed_callbacks"] = (pf[-1] - pf[0] + 1) - len(pf)
    R["s2_fps"] = s2

    # ---- inference frames not processed (by design: inference input is frame-dropped)
    for m in ("ppe", "idss"):
        dm = sorted((d for d in dets if d["m"] == m), key=lambda d: d["pts"] or 0)
        gaps = [b2["pts"] - a["pts"] for a, b2 in zip(dm, dm[1:]) if a["pts"] is not None and b2["pts"] is not None]
        sec1[m]["pts_gap_ms"] = dist(gaps, 1)
        sec1[m]["pts_gaps_over_1_5_frames"] = sum(1 for g in gaps if g > 1.5 * FRAME_MS)
        sec1[m]["pts_non_monotonic"] = sum(1 for g in gaps if g <= 0)

    # ---- 3 latency
    s3 = {"host": {}, "page": {"decode_ms_per_frame": s3_dec, "jitter_buffer_delay_ms": s3_jb}}
    for m in ("ppe", "idss"):
        dm = [d for d in dets if d["m"] == m]
        s3["host"][m] = {"latency_ms": dist([d["lat"] for d in dm], 1), "inference_ms_event": dist([d["ms"] for d in dm], 1),
                          "queue_plus_other_ms": dist([(d["lat"] - d["ms"]) for d in dm if d["lat"] is not None and d["ms"] is not None], 1),
                          "arrival_minus_ts_ms (page receive - frame ts)": dist([d["recv"] - d["ts"] for d in dm if d.get("ts")], 1)}
    # rVFC <-> detections matching
    s3["sync"] = {}
    rows = [r for r in DATA["rvfc"] if r[5] is not None]
    s3["rvfc_fields_available"] = {"rtpTimestamp": bool(rows), "receiveTime": any(r[3] is not None for r in DATA["rvfc"]), "captureTime": any(r[4] is not None for r in DATA["rvfc"]),
                                   "processingDuration": any(r[7] is not None for r in DATA["rvfc"]), "frames": len(DATA["rvfc"])}
    if DATA["rvfc"]:
        dly = [r[1] - r[3] for r in DATA["rvfc"] if r[3] is not None]
        s3["page"]["present_minus_receive_ms (rVFC presentationTime-receiveTime)"] = dist(dly, 1)
        s3["page"]["processing_duration_ms (rVFC)"] = dist([r[7] for r in DATA["rvfc"] if r[7] is not None], 1)
        s3["page"]["capture_to_present_ms (if abs-capture-time)"] = dist([r[1] - r[4] for r in DATA["rvfc"] if r[4] is not None], 1)
        s3["page"]["display_lead_ms (expectedDisplayTime-presentationTime)"] = dist([r[2] - r[1] for r in DATA["rvfc"]], 1)
    M32 = 0xFFFFFFFF
    for m in ("ppe", "idss"):
        dm = [d for d in dets if d["m"] == m and d.get("rtp") is not None]
        if not dm or not rows:
            s3["sync"][m] = {"matched": 0, "note": "no detections with rtp or no rVFC rtpTimestamp"}
            continue
        by_rtp = {r[5] & M32: r for r in rows}
        exact = [(d, by_rtp[d["rtp"] & M32]) for d in dm if (d["rtp"] & M32) in by_rtp]
        mode_name, matched, offset = "exact rtp equality", exact, 0
        if len(exact) < 0.5 * len(dm):
            votes = collections.Counter()
            rows_sorted = sorted(rows, key=lambda r: r[1])
            ts_list = [r[1] for r in rows_sorted]
            import bisect
            for d in dm:
                lo = bisect.bisect_left(ts_list, d["ts"] - 200)
                hi = bisect.bisect_right(ts_list, d["ts"] + 2500)
                for r in rows_sorted[lo:hi]:
                    votes[(r[5] - d["rtp"]) & M32] += 1
            if votes:
                offset, cnt = votes.most_common(1)[0]
                by_off = {(r[5] - offset) & M32: r for r in rows}
                matched = [(d, by_off[d["rtp"] & M32]) for d in dm if (d["rtp"] & M32) in by_off]
                mode_name = f"constant rtp offset {offset} (mode of candidate offsets, {cnt} votes)"
        if len(matched) < 0.3 * len(dm):
            s3["sync"][m] = {"matched": len(matched), "of": len(dm), "note": f"could not associate detections with video frames ({mode_name}); sync not measured"}
            continue
        arrive_minus_present = [d["recv"] - r[1] for d, r in matched]
        age_at_present = [r[1] - d["ts"] for d, r in matched if d.get("ts")]
        lag = [max(0.0, x) for x in arrive_minus_present]
        s3["sync"][m] = {"matched": len(matched), "of": len(dm), "method": mode_name,
                         "detection_arrival_minus_frame_presentation_ms": dist(arrive_minus_present, 1),
                         "detection_lag_frames (arrival after frame shown, >=0)": dist([x / FRAME_MS for x in lag], 2),
                         "detection_early_ms (arrival before frame shown)": dist([max(0.0, -x) for x in arrive_minus_present], 1),
                         "frame_age_at_presentation_ms (presentation - pipeline frame ts)": dist(age_at_present, 1),
                         "frames_with_lag_over_1_frame_pct": round(100.0 * sum(1 for x in lag if x > FRAME_MS) / len(lag), 1)}
    R["s3_latency"] = s3

    # ---- 4 bandwidth
    s4 = {"webrtc_inbound_kbps": s4_kbps, "pipeline_rtsp_kbps": dist([o["kbps"] for o in st], 0)}
    if samples and samples[-1].get("msg") and DATA["base"].get("msg"):
        b2, e2 = DATA["base"]["msg"], samples[-1]["msg"]
        sec = (samples[-1]["t"] - DATA["base"]["t"]) / 1000.0
        per = {}
        for k in e2["cnt"]:
            per[k] = {"msgs_per_s": round((e2["cnt"][k] - b2["cnt"].get(k, 0)) / sec, 2), "bytes_per_s": round((e2["by"][k] - b2["by"].get(k, 0)) / sec, 0),
                      "avg_bytes": round((e2["by"][k] - b2["by"].get(k, 0)) / max(1, e2["cnt"][k] - b2["cnt"].get(k, 0)), 0)}
        s4["bridge_events"] = {"total_msgs_per_s": round((e2["n"] - b2["n"]) / sec, 2), "total_bytes_per_s": round((e2["bytes"] - b2["bytes"]) / sec, 0),
                               "total_kbps": round((e2["bytes"] - b2["bytes"]) * 8 / sec / 1000.0, 1), "per_event_type": per,
                               "note": "bytes = JSON.stringify length of each message seen by the page (ASCII ~ UTF-8 size); a page-side estimate of the WebView2 bridge payload"}
        # per second distribution of message rate
        rates = []
        for p, q in zip(samples, samples[1:]):
            dt_s = (q["t"] - p["t"]) / 1000.0
            if dt_s > 0.2:
                rates.append((q["msg"]["n"] - p["msg"]["n"]) / dt_s)
        s4["bridge_events"]["msgs_per_s_distribution"] = dist(rates, 1)
    s4["resources"] = MON.summary()
    s4["gpu"] = {k: dist([g[k] for g in GPU if g.get(k) is not None], 1) for k in ("util", "mem_util", "mem_used_mb", "power_w", "temp_c", "enc", "dec")} if GPU else None
    s4["gpu_total_mb"] = GPU[0]["mem_total_mb"] if GPU else None
    R["s4_bandwidth"] = s4

    # ---- facts / alarms / expert classes (from the same window)
    facts = [o for o in other if o.get("ev") == "facts" and o["recv"] >= t0]
    s6 = {"facts_events": len(facts), "facts_pct": {}}
    for n in FACT_NAMES:
        vals = [f["pct"].get(n) for f in facts if f.get("pct")]
        s6["facts_pct"][n] = {"mean": round(statistics.fmean(vals), 1) if vals else None, "max": max(vals) if vals else None, "n": len(vals)}
    s6["facts_last"] = {k: facts[-1].get(k) for k in ("zoomed", "scale_mode", "frames", "w", "h", "machine_present", "link_tilt_present", "link_tilt_angled", "elevator_present", "elevator_near_platform", "angle_deg", "derrickman_cx", "machine_cx", "roi")} if facts else None
    s6["facts_pct_no_roi_mean"] = {n: round(statistics.fmean([f["pct_no_roi"].get(n) for f in facts if f.get("pct_no_roi")] or [0]), 1) for n in FACT_NAMES}
    s6["alarm_events_live"] = [{k: o.get(k) for k in ("cam", "state", "previous", "severity", "condition", "place", "frame", "alertId", "phase", "episode", "escalated") if k in o} for o in other if o.get("ev") == "alarm"]
    s6["other_event_counts"] = dict(collections.Counter(o["ev"] for o in other))
    s6["evidence_clip_events"] = [{k: o.get(k) for k in ("ev", "cam", "stamp", "phase", "count")} for o in other if o.get("ev") in ("evidence", "clip")]
    s6["errors_events"] = [o.get("message") for o in other if o.get("ev") == "error"][:10]
    R["s6_expert"] = s6

    # ---- 5a roi from events
    roi_rows = [d for d in dets if d["m"] == "idss"]
    scale = (sec1["idss"].get("scale") or [1.2])[-1]
    tol = 3
    s5 = {"idss_events": len(roi_rows), "scale_used": scale}
    with_platform = [d for d in roi_rows if d.get("platform")]
    geom_ok = geom_bad = 0
    worst = 0.0
    bad_examples = []
    for d in with_platform:
        if d.get("roi") is None:
            geom_bad += 1
            bad_examples.append({"why": "platform present but roi null"})
            continue
        x1, y1, x2, y2 = d["platform"]
        w, h = (x2 - x1) * scale, (y2 - y1) * scale
        cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
        exp = [max(0, cx - w / 2), max(0, cy - h / 2), min(d["w"] - 1, cx + w / 2), min(d["h"] - 1, cy + h / 2)]
        err = max(abs(a - b) for a, b in zip(exp, d["roi"]))
        worst = max(worst, err)
        if err <= tol:
            geom_ok += 1
        else:
            geom_bad += 1
            if len(bad_examples) < 3:
                bad_examples.append({"platform": d["platform"], "roi": d["roi"], "expected": [round(v) for v in exp], "err_px": round(err, 1)})
    s5.update({"events_with_platform": len(with_platform), "roi_matches_scaled_platform": geom_ok, "roi_mismatch": geom_bad, "worst_error_px": round(worst, 1), "tolerance_px": tol, "mismatch_examples": bad_examples})
    # in_roi
    inroi_ok = inroi_bad = 0
    inout = collections.defaultdict(lambda: [0, 0])
    inbad_ex = []
    for d in roi_rows:
        roi = d.get("roi")
        for it in d["items"]:
            if it["r"] is None:
                continue
            lab = (it["l"] or "").lower()
            b = it["b"]
            cx, cy = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
            if roi is None:
                exp_true = exp_false = it["r"] is True
                ok = it["r"] is True
            elif lab == "platform":
                ok = it["r"] is True
            else:
                inside = roi[0] <= cx <= roi[2] and roi[1] <= cy <= roi[3]
                near = min(abs(cx - roi[0]), abs(cx - roi[2]), abs(cy - roi[1]), abs(cy - roi[3])) <= tol
                ok = (it["r"] == inside) or near
            inout[it["l"]][0 if it["r"] else 1] += 1
            if ok:
                inroi_ok += 1
            else:
                inroi_bad += 1
                if len(inbad_ex) < 3:
                    inbad_ex.append({"label": it["l"], "box": b, "roi": roi, "in_roi": it["r"]})
    s5.update({"in_roi_items_ok": inroi_ok, "in_roi_items_wrong": inroi_bad, "in_roi_wrong_examples": inbad_ex, "in_roi_by_class [in, out]": {k: v for k, v in inout.items()}})
    # hold behaviour (observed): roi non-null while platform missing
    run = best_run = null_run = 0
    held = 0
    for d in sorted(roi_rows, key=lambda d: d["pts"] or 0):
        if not d.get("platform"):
            if d.get("roi"):
                run += 1
                held += 1
                best_run = max(best_run, run)
            else:
                run = 0
        else:
            run = 0
    s5.update({"events_without_platform": len(roi_rows) - len(with_platform), "held_roi_events_without_platform": held, "longest_held_run_events": best_run, "hold_frames_cfg": R.get("inference_cfg", {}).get("roi", {}).get("hold_frames")})
    R["s5_roi_events"] = s5


# ------------------------------------------------------------------------------------------------ config + unit tests
def load_cfg() -> None:
    cfgp = Path(A.app_dir) / "models" / "inference.cfg"
    src = Path(REPO / "models" / "inference.cfg")
    try:
        out = subprocess.run([sys.executable, str(REPO / "scripts" / "inference_cfg.py"), "decode", str(cfgp)], capture_output=True, text=True, timeout=30).stdout
        R["inference_cfg"] = json.loads(out)
        R["inference_cfg_source"] = str(cfgp)
        R["inference_cfg_same_as_repo"] = cfgp.read_bytes() == src.read_bytes() if src.exists() else None
    except Exception as e:  # noqa: BLE001
        R["inference_cfg"] = {}
        R["errors"].append({"phase": "cfg", "error": str(e)})


def run_unit_tests() -> None:
    res = {}
    for exe in ("roi_facts_test.exe", "platform_overlay_test.exe"):
        p = BUILD / exe
        if not p.exists():
            res[exe] = "not built"
            continue
        try:
            r = subprocess.run([str(p)], capture_output=True, text=True, timeout=120, cwd=str(REPO))
            tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
            res[exe] = {"exit": r.returncode, "tail": tail}
        except Exception as e:  # noqa: BLE001
            res[exe] = f"error {e}"
    R["unit_tests"] = res


# ------------------------------------------------------------------------------------------------ 5b zones
def phase_zones(S: Session) -> None:
    pg = S.page
    S.where = "zones"
    s = {"steps": []}
    R["s5_zones"] = s
    cam = R["cam"]
    S.banner("Validation: Zone editor - add, edit, save, reload, verify, delete")
    base = call_ok(pg, "records.get", {"key": "zones:v1"})
    s["records_before"] = base
    S.nav("Zone editor", admin=True)
    shot(pg, "05-zone-empty")
    svg = pg.locator("svg[viewBox='0 0 100 100']").first
    box = svg.bounding_box()
    add_btn = pg.get_by_role("button", name="Add point")
    pts = [(0.30, 0.70), (0.35, 0.30), (0.70, 0.30), (0.75, 0.70)]
    for fx, fy in pts:
        S.click(add_btn.first, wait=0.3)
        pg.mouse.click(box["x"] + box["width"] * fx, box["y"] + box["height"] * fy)
        pause(0.4)
    S.fill(pg.locator("label:has-text('Zone name') input").first, "Validation zone A")
    verts = pg.get_by_role("button", name=re.compile(r"Zone vertex")).count()
    s["steps"].append({"step": "drew 4 points", "vertices_shown": verts})
    shot(pg, "05-zone-drawn")
    save = pg.get_by_role("button", name=re.compile("Save zone draft")).first
    s["save_enabled"] = save.is_enabled()
    S.click(save, wait=1.5)
    toast = S.body()
    s["toast_text"] = [t for t in re.findall(r"Zone draft saved[^\n]*", toast)]
    rec = call_ok(pg, "records.get", {"key": "zones:v1"})
    s["records_after_save"] = rec
    check("5 ROI", "zone saved to host record zones:v1 (records.get)", bool(rec) and cam in rec and len(rec[cam]["points"]) == 4 and rec[cam]["name"] == "Validation zone A", "ZoneEditor.tsx -> records.set 'zones:v1'", json.dumps(rec)[:200])
    # edit: move vertex 1 with the keyboard, rename, add a point
    v1 = pg.get_by_role("button", name="Zone vertex 1").first
    v1.focus()
    for _ in range(5):
        pg.keyboard.press("ArrowRight")
        pause(0.1)
    S.fill(pg.locator("label:has-text('Zone name') input").first, "Validation zone A (edited)")
    S.click(add_btn.first, wait=0.3)
    pg.mouse.click(box["x"] + box["width"] * 0.5, box["y"] + box["height"] * 0.85)
    pause(0.5)
    shot(pg, "05-zone-edited")
    S.click(pg.get_by_role("button", name=re.compile("Save zone draft")).first, wait=1.5)
    rec2 = call_ok(pg, "records.get", {"key": "zones:v1"})
    s["records_after_edit"] = rec2
    moved = rec and rec2 and rec[cam]["points"][0][0] != rec2[cam]["points"][0][0]
    check("5 ROI", "zone edit (renamed, vertex moved, 5th point) persisted", bool(rec2) and rec2[cam]["name"].endswith("(edited)") and len(rec2[cam]["points"]) == 5 and bool(moved), "records.get", f"{len(rec2[cam]['points']) if rec2 else '?'} points; vertex1 x {rec[cam]['points'][0][0]:.1f}->{rec2[cam]['points'][0][0]:.1f}" if rec and rec2 else "")
    # reload and verify persisted + shown
    S.nav("Fleet overview")  # leave and re-enter: the editor remounts and re-reads records.get (a real page reload is not possible, see probe_reload)
    S.nav("Zone editor", admin=True)
    pause(1.5)
    name_v = pg.locator("label:has-text('Zone name') input").first.input_value()
    verts2 = pg.get_by_role("button", name=re.compile(r"Zone vertex")).count()
    rec3 = call_ok(pg, "records.get", {"key": "zones:v1"})
    s["after_reload"] = {"name_in_ui": name_v, "vertices_in_ui": verts2, "record": rec3}
    shot(pg, "05-zone-after-reload")
    check("5 ROI", "zone survives leaving and re-entering the editor (remount; UI shows saved name + vertices, records.get equal)", name_v == "Validation zone A (edited)" and verts2 == 5 and rec3 == rec2, "records.get / UI", f"name='{name_v}', vertices={verts2}")
    # Reset button semantic: drag a vertex then Reset returns to saved geometry
    v2 = pg.get_by_role("button", name="Zone vertex 2").first
    v2.focus()
    pg.keyboard.press("ArrowDown")
    pg.keyboard.press("ArrowDown")
    before = pg.locator("svg polygon").last.get_attribute("points")
    S.click(pg.get_by_role("button", name=re.compile(r"^\s*Reset\s*$")).first, wait=0.8)
    after = pg.locator("svg polygon").last.get_attribute("points")
    s["reset_button"] = {"points_before_reset": before, "points_after_reset": after}
    check("8 SETTINGS", "Zone editor 'Reset' restores the saved geometry (discards unsaved vertex moves)", before != after, "ZoneEditor.tsx reset()", f"{before} -> {after}")
    # vertex deletion (UI minimum 3), then the 'Delete zone' control
    ctrl_names = sorted({(b.get_attribute("aria-label") or b.inner_text() or "").strip() for b in pg.locator("main button").all()})[:40]
    s["controls_seen"] = ctrl_names
    for _ in range(2):
        pg.get_by_role("button", name="Zone vertex 1").first.click()
        d = pg.get_by_role("button", name="Delete selected vertex").first
        if d.is_enabled():
            d.click()
            pause(0.3)
    s["vertices_after_deletes"] = pg.get_by_role("button", name=re.compile(r"Zone vertex")).count()
    S.click(pg.get_by_role("button", name=re.compile("Save zone draft")).first, wait=1.2)
    rec4 = call_ok(pg, "records.get", {"key": "zones:v1"})
    s["records_after_vertex_delete"] = rec4
    check("5 ROI", "vertex deletion persists (UI minimum 3 vertices)", bool(rec4) and len(rec4[cam]["points"]) == 3, "records.get", f"{len(rec4[cam]['points']) if rec4 else '?'} points")
    dz = pg.get_by_role("button", name="Delete zone")
    has_delete_zone = dz.count() > 0
    if has_delete_zone:
        S.click(dz.first, wait=1.2)
    rec5 = call_ok(pg, "records.get", {"key": "zones:v1"})
    s["records_after_delete_zone"] = rec5
    verts_after = pg.get_by_role("button", name=re.compile(r"Zone vertex")).count()
    check("5 ROI", "UI offers 'Delete zone' and it removes the saved zone record", has_delete_zone and cam not in (rec5 or {}) and verts_after == 0, "ZoneEditor.tsx Delete zone", f"button present={has_delete_zone}; record after={json.dumps(rec5)}; vertices in UI={verts_after}")
    # does a saved zone influence detection / EWS?  Code evidence, not fabricated
    hits = []
    for root, pat in ((REPO.parent / "Rigmind_v2" / "src", r"zones:v1|\"zones\"|zones\.value"), (REPO / "src", r"zones:v1|\"zone\"|zones")):
        for f in root.rglob("*"):
            if f.suffix in (".ts", ".tsx", ".cpp", ".h") and "test" not in f.name.lower() and ".gen." not in f.name:
                try:
                    for i, line in enumerate(f.read_text(encoding="utf-8", errors="ignore").splitlines(), 1):
                        if re.search(pat, line):
                            hits.append(f"{f.relative_to(REPO.parent)}:{i}: {line.strip()[:110]}")
                except Exception:  # noqa: BLE001
                    pass
    s["code_references"] = hits[:30]
    consumers_cpp = [h for h in hits if h.replace("\\", "/").startswith("RigMind_Desktop/src") and "zones:v1" in h]
    s["cpp_reads_zones_record"] = bool(consumers_cpp)
    check("5 ROI", "GAP: a saved user zone influences detection / EWS", False if not consumers_cpp else None, "code search: ZoneEditor.tsx toast says 'detection is unchanged'; no C++ or pipeline reference to 'zones:v1'",
          "no consumer found: zones are stored only (host record zones:v1); the only dynamic region the pipeline uses is the Platform-derived ROI")


# ------------------------------------------------------------------------------------------------ 6/7 expert outputs, alarms, footage, summaries
def phase_experts(S: Session) -> None:
    pg = S.page
    S.where = "experts"
    s = R.setdefault("s6_outputs", {})
    cam = R["cam"]
    S.banner("Validation: expert outputs - bell > Test notification (scripted red episode), alerts, audit, footage")
    S.nav("Live monitoring")
    wait_video(pg, 60)
    t_before = int(time.time() * 1000)
    base_alerts = call_ok(pg, "alerts.feed", {"limit": 500})
    base_audit = call_ok(pg, "audit.recent", {"action": "alarm.transition", "limit": 500})
    base_foot = call_ok(pg, "footage.list", {"limit": 500})
    n_other_before = len(DATA["other"])
    # UI path: bell > Test notification (runs alarm.simulate red with the camera, raises alerts, asks event.describe)
    S.click(pg.get_by_role("button", name=re.compile(r"Notifications")).first, wait=0.8)
    shot(pg, "06-bell-open")
    t_click = time.time()
    S.click(pg.get_by_role("button", name="Test notification").first, wait=0.5)
    toast_seen = None
    for _ in range(45):
        txt = S.body()
        m = re.search(r"(EV-\d+|ALM-[\w-]+) in plain words\s*\n?(.*)", txt)
        if m and m.group(2).strip():
            toast_seen = {"alert": m.group(1), "text": m.group(2).strip().split("\n")[0], "after_s": round(time.time() - t_click, 1)}
            break
        time.sleep(1)
    s["ui_toast_plain_words"] = toast_seen
    shot(pg, "06-after-test-notification")
    pg.keyboard.press("Escape")
    # wait for the clip: pipeline saves footage then emits 'clip'
    clip_seen = None
    t0 = time.time()
    while time.time() - t0 < 90:
        drain(pg, DATA)
        evs = [o for o in DATA["other"][n_other_before:] if o.get("ev") == "clip"]
        errs = [o for o in DATA["other"][n_other_before:] if o.get("ev") == "error" and "footage" in str(o.get("message"))]
        if evs:
            clip_seen = evs[-1]
            break
        if errs:
            s["clip_error"] = errs[-1].get("message")
            break
        time.sleep(2)
    s["clip_event"] = {k: clip_seen.get(k) for k in ("cam", "stamp", "count")} if clip_seen else None
    s["clip_wait_s"] = round(time.time() - t0, 1)
    droot = Path(APP["local"]) / "RigMind"
    s["data_dir_listing"] = [f"{p.relative_to(droot)} ({p.stat().st_size} B)" for p in sorted(droot.rglob("*")) if p.is_file() and "WebView2" not in p.parts][:40] if droot.exists() else []
    pause(2)
    new_events = DATA["other"][n_other_before:]
    s["events_since_test"] = dict(collections.Counter(o["ev"] for o in new_events))
    alerts = call_ok(pg, "alerts.feed", {"limit": 500})
    audit = call_ok(pg, "audit.recent", {"action": "alarm.transition", "limit": 500})
    trans = call_ok(pg, "events.recent", {"limit": 500})
    foot = call_ok(pg, "footage.list", {"limit": 500})
    audit_all = call_ok(pg, "audit.recent", {"action": "footage.save", "limit": 50})
    s["alerts_feed_sample"] = alerts[:6]
    s["audit_sample"] = audit[:6]
    s["events_recent_sample"] = trans[:6]
    s["footage_list"] = foot[:5]
    new_alerts = [a for a in alerts if a["raisedMs"] >= t_before - 1000]
    new_trans = [t for t in trans if t["timestampMs"] >= t_before - 1000 and str(t.get("alertId", "")).startswith("EV-")]  # scripted rows only; real pipeline transitions may interleave
    real_trans = [t for t in trans if t["timestampMs"] >= t_before - 1000 and not str(t.get("alertId", "")).startswith("EV-")]
    s["real_transitions_during_test"] = len(real_trans)
    new_audit = [a for a in audit if a["tsMs"] >= t_before - 1000]
    s["counts"] = {"new_alerts": len(new_alerts), "new_alarm_transitions": len(new_trans), "new_audit_alarm_transition_rows": len(new_audit), "alerts_before": len(base_alerts), "audit_before": len(base_audit), "footage_before": len(base_foot), "footage_after": len(foot)}
    check("6 EXPERT", "bell > Test notification produced a scripted episode (3 transitions Yellow/Orange/Red)", len(new_trans) == 3 and [t["to"].title() for t in sorted(new_trans, key=lambda x: x["id"])] == ["Yellow", "Orange", "Red"], "bridge.cpp simulate()", f"{[t['to'] for t in sorted(new_trans, key=lambda x: x['id'])]}")
    sim_audit = [a for a in call_ok(pg, "audit.recent", {"action": "alarm.simulate", "limit": 20}) if a["tsMs"] >= t_before - 1000]
    s["audit_alarm_simulate_rows"] = sim_audit
    check("6 EXPERT", "every alarm transition has an audit row (alarm.transition)", len(new_audit) >= len(new_trans) and len(new_trans) > 0, "bridge audit.recent vs events.recent", f"transitions={len(new_trans)} alarm.transition audit rows={len(new_audit)} (scripted transitions; {len(sim_audit)} 'alarm.simulate' row(s) also written)")
    ids_tr = {t["alertId"] for t in new_trans}
    ids_al = {a["id"] for a in new_alerts}
    check("6 EXPERT", "alarm transition rows (events.recent) carry the alertId of their alert", all(ids_tr) and len(ids_tr) == len(new_trans) and len(ids_tr) > 0, "event_store.cpp alarm_events.alert_id", f"transition alertIds={sorted(ids_tr)} (simulate path stores no id on the transition; alerts raised: {sorted(ids_al)})")
    check("6 EXPERT", "one new EV-* alerts.feed row per simulated transition", len([a for a in new_alerts if a["id"].startswith("EV-")]) == len(new_trans) and len(new_trans) > 0, "alerts.feed vs events.recent", f"{len([a for a in new_alerts if a['id'].startswith('EV-')])} EV alerts / {len(new_trans)} transitions; other new alert rows (real pipeline episodes): {[a['id'] for a in new_alerts if not a['id'].startswith('EV-')]}")
    per_episode = collections.Counter(a["id"] for a in new_alerts)
    check("6 EXPERT", "exactly one alerts.feed row per alertId", all(v == 1 for v in per_episode.values()) and bool(per_episode), "alerts.feed", json.dumps(per_episode))
    s["note_sim_vs_episode"] = ("alarm.simulate stores ONE alert per transition (3 ids for Yellow/Orange/Red), unlike the pipeline path "
                                "(AlarmRecorder: one alert id per episode, escalated). Per-episode uniqueness is therefore checked on real pipeline alarm events below.")
    # real pipeline alarm episodes (if the feed produced any)
    real = [a for a in R["s6_expert"]["alarm_events_live"] if a.get("alertId")]
    if real:
        eps = collections.defaultdict(list)
        for a in real:
            eps[a["alertId"]].append(a["state"])
        feed_ids = collections.Counter(a["id"] for a in alerts)
        s["real_episodes"] = {k: {"states": v, "alerts_feed_rows": feed_ids.get(k, 0)} for k, v in eps.items()}
        check("6 EXPERT", "real pipeline episodes: exactly one alerts.feed row per episode", all(feed_ids.get(k, 0) == 1 for k in eps), "AlarmRecorder", json.dumps(s["real_episodes"])[:200])
    else:
        check("6 EXPERT", "real pipeline alarm episode observed in the feed", None, "live stream", "no alarm event came from the real stream during the run (EWS stayed None); per-episode check covered by the scripted episode and unit tests only")
    # footage rows vs files vs disk
    new_foot = foot[: max(0, len(foot) - len(base_foot))]  # the clip row is stamped with the episode start, which can predate this test
    check("6 EXPERT", "clip event -> footage row (footage.list) written by the host", bool(clip_seen) and len(new_foot) >= 1, "pipeline 'clip' -> footage.record", f"clip_event={s['clip_event']} pipeline error={s.get('clip_error')} new rows={len(new_foot)}")
    frows = []
    video_test = None
    for f in foot[:5]:
        files = call_ok(pg, "footage.files", {"path": f["path"]})
        disk_dir = Path(files["dir"])
        disk = sorted(p.name for p in disk_dir.glob("*.mp4")) if disk_dir.exists() else []
        sizes = {p.name: p.stat().st_size for p in disk_dir.glob("*.mp4")} if disk_dir.exists() else {}
        frows.append({"path": f["path"], "clipCount": f["clipCount"], "bridge_files": files["files"], "disk_files": disk, "sizes": sizes,
                      "match": f["clipCount"] == len(files["files"]) == len(disk) and bool(disk) and all(v > 1000 for v in sizes.values())})
        if video_test is None and files["files"]:
            url = f"https://footage.rigmind.app/{f['path']}/{files['files'][0]}"
            video_test = pg.evaluate("""async (url) => {
              const v = document.createElement('video'); v.muted = true; v.preload = 'auto'; v.src = url; v.style.cssText='position:fixed;right:8px;top:8px;width:320px;z-index:99999;border:2px solid #ff2d95'; document.body.appendChild(v);
              const t0 = performance.now();
              const r = await new Promise((res) => { v.onloadeddata = () => res('ok'); v.onerror = () => res('error:' + (v.error && v.error.code)); setTimeout(() => res('timeout'), 20000); });
              let played = null;
              if (r === 'ok') { try { await v.play(); await new Promise(r2 => setTimeout(r2, 1500)); played = v.currentTime; } catch (e) { played = 'play error ' + e; } }
              const out = { url, result: r, w: v.videoWidth, h: v.videoHeight, duration: v.duration, currentTime_after_play: played, load_ms: Math.round(performance.now() - t0) };
              v.pause(); v.remove(); return out; }""", url)
            S.banner("Validation: footage clip played from footage.rigmind.app")
    s["footage_rows"] = frows
    s["footage_playback"] = video_test
    check("6 EXPERT", "footage rows == files via footage.files == mp4 files on disk (non-empty)", bool(frows) and all(r["match"] for r in frows), "footage.list/footage.files/disk", json.dumps([{k: r[k] for k in ("path", "clipCount", "match")} for r in frows])[:240])
    check("6 EXPERT", "a clip plays in the page from https://footage.rigmind.app/ (videoWidth, duration)", bool(video_test) and video_test["result"] == "ok" and video_test["w"] > 0 and (video_test["duration"] or 0) > 0, "footage virtual host",
          json.dumps(video_test)[:240] if video_test else "no clip to play")
    # disk layout
    foot_root = Path(APP["local"]) / "RigMind" / "footage"
    s["disk_root"] = str(foot_root)
    s["disk_listing"] = [str(p.relative_to(foot_root)) for p in foot_root.rglob("*.mp4")][:20] if foot_root.exists() else []
    # pages show the data
    for label, path in (("Alert inbox", "/alerts"), ("Event history", "/events"), ("Evidence library", "/evidence")):
        S.nav(label)
        pause(1.5)
        txt = S.body()
        s.setdefault("page_text_ids", {})[label] = {"mentions_new_alert_ids": [i for i in sorted(ids_al) if i in txt], "chars": len(txt)}
        shot(pg, f"06-{label.lower().replace(' ', '-')}")
    check("6 EXPERT", "new alert ids visible in Alert inbox / Event history", bool(ids_al) and all(i in json.dumps(s["page_text_ids"]) for i in list(ids_al)[:1]), "UI", json.dumps(s["page_text_ids"])[:240])
    # open an event detail if the history has a link
    S.nav("Event history")
    link = pg.locator("main a[href*='/events/']").first
    if link.count():
        S.click(link, wait=1.5)
        det = S.body()
        s["event_detail"] = {"path": S.path(), "chars": len(det), "has_plain_section": bool(re.search(r"plain", det, re.I)), "snippet": det[:600]}
        shot(pg, "06-event-detail")
    R["alert_ids_new"] = sorted(ids_al)
    R["t_before_test"] = t_before


def phase_summaries(S: Session) -> None:
    pg = S.page
    S.where = "summaries"
    s = {}
    R["s7_summary"] = s
    ids = R.get("alert_ids_new") or []
    s["slm_binaries_in_app"] = R["meta"].get("slm_present")
    s["expected_paths"] = [str(Path(A.app_dir) / "slm" / "llama-server.exe"), str(Path(A.app_dir) / "slm" / "ggml-model-i2_s.gguf")]
    s["bridge_methods_with_describe_or_summary"] = sorted(set(re.findall(r'method == "([a-z.]*(?:describe|summar)[a-z.]*)"', (REPO / "src" / "backend" / "bridge.cpp").read_text(encoding="utf-8"))))
    s["bridge_methods_all"] = sorted(set(re.findall(r'method == "([a-z.]+)"', (REPO / "src" / "backend" / "bridge.cpp").read_text(encoding="utf-8"))))
    # port 8791 reachable?
    import socket
    try:
        sock = socket.create_connection(("127.0.0.1", 8791), timeout=1.5)
        sock.close()
        s["port_8791_open"] = True
    except Exception:  # noqa: BLE001
        s["port_8791_open"] = False
    now_ms = int(time.time() * 1000)
    results = []
    cam = R["cam"]
    for idx, (sev, cond) in enumerate([("Critical", "Platform-elevator collision"), ("Warning", "Elevator near platform")]):
        aid = ids[-1 - idx] if len(ids) > idx else f"EV-VAL{idx}"
        n0 = len(DATA["other"])
        t0 = time.time()
        r = call(pg, "event.describe", {"alertId": aid, "severity": sev, "condition": cond, "place": f"Camera {cam}", "camId": cam, "tsMs": now_ms})
        t_reply = time.time() - t0
        desc = None
        while time.time() - t0 < 20:
            drain(pg, DATA)
            ev = [o for o in DATA["other"][n0:] if o.get("ev") == "description" and o.get("alertId") == aid]
            if ev:
                desc = ev[-1]
                break
            time.sleep(0.3)
        text = (r.get("result") or {}).get("text", "") if r.get("ok") else ""
        shown = (desc or {}).get("text") or text
        when = time.strftime("%d %b %Y %H:%M", time.localtime(now_ms / 1000))
        results.append({"alertId": aid, "reply_ms": round(t_reply * 1000), "reply": r.get("result"), "description_event": {k: desc.get(k) for k in ("text", "source")} if desc else None,
                        "description_wait_s": round(time.time() - t0, 2), "text_shown": shown,
                        "grounded": {"mentions_camera": cam in shown, "mentions_condition": cond in shown, "mentions_severity": sev in shown, "mentions_time": when in shown,
                                     "mentions_place": f"Camera {cam}" in shown or "Camera" in shown},
                        "sample_names": [x for x in SAMPLE_STRINGS if x in shown], "non_empty": bool(shown.strip())})
    s["requests"] = results
    s["status_summary_request"] = "none: the bridge has no camera-status summary method (only event.describe, which takes severity+condition+place+camId+tsMs); camera status is not summarised by any model"
    ok = all(r["non_empty"] and r["grounded"]["mentions_camera"] and r["grounded"]["mentions_condition"] and r["grounded"]["mentions_time"] and not r["sample_names"] for r in results)
    check("7 SUMMARY", "event.describe returns non-empty text grounded in camera, condition, severity and time, no sample names", ok, "bridge.cpp plain_facts()", json.dumps([r["text_shown"] for r in results])[:300])
    src = {(r["description_event"] or {}).get("source") for r in results}
    check("7 SUMMARY", "BitNet (llama-server) summary service reachable", R["meta"].get("slm_present") and "slm" in src, "main.cpp g_slm (<app>\\slm\\llama-server.exe + ggml-model-i2_s.gguf, port 8791)",
          f"slm binaries in app dir: {R['meta'].get('slm_present')}; description source(s): {sorted(x for x in src if x)}; port 8791 open: {s['port_8791_open']}")
    # UI rendering: stored plain summaries and the report table
    recs = call_ok(pg, "records.list", {"prefix": "plain:"})
    s["stored_plain_records"] = recs if isinstance(recs, list) else recs
    S.nav("Reports")
    s["ui_note"] = "When no model runs the UI shows the template sentence (source 'Template' in the report table; toast title '<id> in plain words'); the CSV column 'Source' says BitNet or Template."


# ------------------------------------------------------------------------------------------------ 8 pages, reports, settings
PAGES = [
    ("Fleet overview", "/fleet", False), ("Rig workspace", "/rig", False), ("Live monitoring", "/live", False), ("Alert inbox", "/alerts", False),
    ("Event history", "/events", False), ("Evidence library", "/evidence", False), ("Analytics", "/analytics", False), ("Actions & handover", "/actions", False),
    ("Reports", "/reports", False), ("Cameras", "/admin/cameras", True), ("Safety rules", "/admin/rules", True), ("Zone editor", "/admin/zones", True),
    ("System health", "/admin/health", True), ("License", "/admin/license", True), ("Integrations", "/admin/integrations", True), ("Users & access", "/admin/users", True),
    ("Audit trail", "/admin/audit", True),
]


def phase_pages(S: Session) -> None:
    pg = S.page
    s = {"pages": [], "reports": []}
    R["s8_pages"] = s
    demo = pg.evaluate("()=>window.__RIGMIND_CONFIG__ ? window.__RIGMIND_CONFIG__.demoData===true : null")
    s["demo_data"] = demo
    for label, path, admin in PAGES:
        n_err = len(S.errors)
        S.nav(label, admin=admin)
        pause(1.5)
        body = S.body()
        cur = S.path()
        found = [x for x in SAMPLE_STRINGS if x in body]
        row = {"label": label, "path": cur, "chars": len(body), "route_ok": path in cur, "error_text": (ERR_PAGE_RE.search(body) or [None])[0], "sample_strings": found, "console_errors": [e["text"][:120] for e in S.errors[n_err:] if not e["benign"]]}
        s["pages"].append(row)
        shot(pg, f"08-page-{re.sub(r'[^a-z0-9]+', '_', label.lower())}")
        check("8 PAGES", f"{label}: renders, no error page, no sample data", row["route_ok"] and not row["error_text"] and row["chars"] > 80 and (not found or demo), "UI sidebar click", f"{path} chars={len(body)} sample={found} errs={len(row['console_errors'])}")
    # reports
    S.nav("Reports")
    radios = pg.locator("input[type=radio][name=report-type]")
    count = radios.count()
    s["report_types"] = count
    rdir = HERE / "reports"
    rdir.mkdir(exist_ok=True)
    for i in range(count):
        S.nav("Reports")
        r = radios.nth(i)
        label = (r.evaluate("e=>e.closest('label')?.innerText||''") or f"type{i}").split("\n")[0].strip()[:40]
        S.click(r.locator("xpath=ancestor::label[1]"), wait=0.6)
        S.click(pg.get_by_role("button", name=re.compile(r"Generate report")).first, wait=0.5)
        try:
            pg.wait_for_selector(".rm-report", timeout=20000)
        except Exception:  # noqa: BLE001
            s["reports"].append({"type": label, "opened": False})
            check("8 REPORT", f"report '{label}' opens", False, "Reports page", "report viewer did not open")
            continue
        pause(1.5)
        txt = pg.inner_text(".rm-report")
        slug = re.sub(r"[^a-z0-9]+", "_", label.lower())
        (rdir / f"{slug}.txt").write_text(txt, encoding="utf-8")
        shot(pg, f"08-report-{slug}")
        pdf_ok = None
        try:
            sess = pg.context.new_cdp_session(pg)
            pdf = sess.send("Page.printToPDF", {"printBackground": True, "preferCSSPageSize": True})
            (rdir / f"{slug}.pdf").write_bytes(base64.b64decode(pdf["data"]))
            pdf_ok = (rdir / f"{slug}.pdf").stat().st_size
        except Exception:  # noqa: BLE001
            pass
        found = [x for x in SAMPLE_STRINGS if x in txt]
        mentions_new = [i for i in (R.get("alert_ids_new") or []) if i in txt]
        row = {"type": label, "opened": True, "path": S.path(), "chars": len(txt), "sample_strings": found, "has_plain_summary_section": bool(re.search(r"plain", txt, re.I)),
               "mentions_new_alert_ids": mentions_new, "pdf_bytes": pdf_ok, "toolbar_print": pg.get_by_role("button", name=re.compile("Print")).count() > 0, "toolbar_csv": pg.get_by_role("button", name=re.compile("CSV")).count() > 0}
        s["reports"].append(row)
        check("8 REPORT", f"report '{label}': renders from real data, no sample strings", len(txt) > 100 and not ERR_PAGE_RE.search(txt) and (not found or demo), "report.$reportId route", f"{len(txt)} chars, pdf={pdf_ok}, new alert ids in report={mentions_new}, sample={found}")
        S.click(pg.get_by_role("button", name=re.compile("Back")).first, wait=0.8)
        pg.wait_for_selector("aside", timeout=15000)
    # analysis connects the newly inferred data to the analytics/report pages
    S.nav("Analytics")
    pause(1.5)
    atxt = S.body()
    s["analytics_mentions_new_alert_ids"] = [i for i in (R.get("alert_ids_new") or []) if i in atxt]
    s["analytics_text_snippet"] = atxt[:500]


def read_state(S: Session) -> dict:
    pg = S.page
    st = {}
    st["dark"] = pg.evaluate("()=>document.documentElement.classList.contains('dark')")
    S.nav("Live monitoring")
    st["muted"] = pg.locator("button:has(svg.lucide-volume-x), button[aria-pressed='true'][aria-label*='lert sounds']").count() > 0
    S.click(pg.get_by_role("button", name="User account").first, wait=0.8)
    S.click(pg.get_by_role("button", name=re.compile("View profile")).first, wait=0.8)
    txt = pg.locator("[role=dialog]").last.inner_text()
    m = re.search(r"Work email\s*\n?\s*(\S+@\S+|Not set)", txt)
    st["email"] = m.group(1) if m else txt[:60]
    for _ in range(3):
        pg.keyboard.press("Escape")
        pause(0.3)
    S.nav("Safety rules", admin=True)
    for _ in range(10):
        st["rules_text_has_rule"] = "VAL-RULE-1" in S.body()
        if st["rules_text_has_rule"]:
            break
        time.sleep(1)
    return st


def set_email(S: Session, email: str):
    pg = S.page
    S.click(pg.get_by_role("button", name="User account").first, wait=0.8)
    S.click(pg.get_by_role("button", name=re.compile("View profile")).first, wait=0.8)
    S.click(pg.get_by_role("button", name="Edit").first, wait=0.6)
    S.fill(pg.get_by_label("Work email").last, email)
    S.click(pg.get_by_role("button", name="Save").first, wait=1.2)
    txt = pg.locator("[role=dialog]").last.inner_text()
    for _ in range(3):
        pg.keyboard.press("Escape")
        pause(0.3)
    return email in txt


def phase_settings(S: Session, pw) -> Session:
    pg = S.page
    s = {"steps": []}
    R["s8_settings"] = s
    S.banner("Validation: settings - change, restart, verify, reset to defaults")
    S.ensure_shell()
    s["defaults"] = read_state(S)
    S.nav("Fleet overview")
    S.click(pg.get_by_role("button", name=re.compile(r"(Dark|Light) theme")).first, wait=0.8)
    changed = {"dark": pg.evaluate("()=>document.documentElement.classList.contains('dark')")}
    S.nav("Fleet overview")
    changed["email_ok"] = set_email(S, NOTIFY_EMAIL)
    S.nav("Live monitoring")
    mute = pg.locator("button[aria-label*='lert sounds'], button[title*='lert sounds'], button[title*='lert sound'], button:has(svg.lucide-volume-2), button:has(svg.lucide-volume-x)")
    S.click(mute.first, wait=0.8)
    changed["muted"] = pg.locator("button:has(svg.lucide-volume-x), button[aria-pressed='true'][aria-label*='lert sounds']").count() > 0
    S.nav("Safety rules", admin=True)
    rid = pg.locator("label:has-text('Rule ID') input").first
    changed["rule_saved"] = False
    if rid.count() and rid.is_editable():
        S.fill(rid, "VAL-RULE-1")
        S.click(pg.get_by_role("button", name=re.compile(r"Save draft")).first, wait=1.2)
        changed["rule_saved"] = "VAL-RULE-1" in S.body()
    S.nav("Zone editor", admin=True)
    cam = R["cam"]
    # a zone as a persisted setting too
    zn = pg.locator("label:has-text('Zone name') input").first
    svg = pg.locator("svg[viewBox='0 0 100 100']").first
    box = svg.bounding_box()
    for fx, fy in ((0.2, 0.8), (0.3, 0.2), (0.8, 0.2)):
        S.click(pg.get_by_role("button", name="Add point").first, wait=0.2)
        pg.mouse.click(box["x"] + box["width"] * fx, box["y"] + box["height"] * fy)
        pause(0.3)
    S.fill(zn, "Validation setting zone")
    S.click(pg.get_by_role("button", name=re.compile("Save zone draft")).first, wait=1.2)
    changed["zone_saved"] = bool((call_ok(pg, "records.get", {"key": "zones:v1"}) or {}).get(cam))
    s["changed"] = changed
    shot(pg, "08-settings-changed")
    check("8 SETTINGS", "settings changed in the UI (theme, email, mute, rule draft, zone)", changed["email_ok"] and changed["muted"] and changed["zone_saved"], "UI", json.dumps(changed))
    # ---- restart with the same LOCALAPPDATA
    if not A.no_restart:
        S.disconnect()
        stop_app()
        launch_app()
        S = Session(pw, "restart")
        assert S.connect(90), "app did not come back"
        reload_and_login(S)
        pg = S.page
        s["after_restart"] = read_state(S)
        st = s["after_restart"]
        check("8 SETTINGS", "theme persisted after app restart", st["dark"] == changed["dark"], "same LOCALAPPDATA", f"dark={st['dark']}")
        check("8 SETTINGS", "notification email persisted after app restart", st["email"] == NOTIFY_EMAIL, "same LOCALAPPDATA", st["email"])
        check("8 SETTINGS", "alert-sound mute persisted after app restart", st["muted"] == changed["muted"], "same LOCALAPPDATA", f"muted={st['muted']}")
        if changed["rule_saved"]:
            check("8 SETTINGS", "rule draft persisted after app restart", st["rules_text_has_rule"], "same LOCALAPPDATA", "")
        z = call_ok(pg, "records.get", {"key": "zones:v1"}) or {}
        check("8 SETTINGS", "zone persisted after app restart (records.get)", bool(z.get(R['cam'])), "same LOCALAPPDATA", json.dumps(z)[:160])
        shot(pg, "08-after-restart")
    # ---- reset to defaults
    S.banner("Validation: reset to defaults")
    S.nav("Zone editor", admin=True)
    s["reset_controls"] = {"zone_editor_Reset": "restores the saved polygon only (verified above); does not clear the saved record or the name field",
                           "rules_Discard": None, "global_reset": "none in the UI (no 'Reset to defaults' button found on any page)"}
    body_all = []
    for lab, adm in (("Safety rules", True), ("Users & access", True), ("Integrations", True), ("License", True), ("System health", True)):
        S.nav(lab, admin=adm)
        names = [(b.inner_text() or b.get_attribute("aria-label") or "").strip() for b in pg.locator("main button").all()]
        body_all.append({lab: [n for n in names if n][:25]})
        if lab == "Safety rules":
            s["reset_controls"]["rules_Discard"] = any(re.search(r"discard", n, re.I) for n in names)
    s["buttons_scan"] = body_all
    s["reset_buttons_found"] = [n for d in body_all for v in d.values() for n in v if re.search(r"reset|default", n, re.I)]
    # reset to defaults through the user-menu button 'Reset settings to defaults'
    S.nav("Fleet overview")
    S.click(pg.get_by_role("button", name="User account").first, wait=0.8)
    rb = pg.get_by_role("button", name=re.compile(r"Reset settings to defaults"))
    s["reset_button_present"] = rb.count() > 0
    if rb.count():
        S.click(rb.first, wait=1.2)
        pg.keyboard.press("Escape")
    call_ok(pg, "records.set", {"key": "zones:v1", "value": {}})
    if changed["rule_saved"]:
        S.nav("Safety rules", admin=True)
        disc = pg.get_by_role("button", name=re.compile(r"Discard"))
        if disc.count():
            S.click(disc.first, wait=0.8)
            s["rule_discard_clicked"] = True
    s["after_reset"] = read_state(S)
    ar, df = s["after_reset"], s["defaults"]
    check("8 SETTINGS", "after reverting: theme / mute equal the defaults", ar["dark"] == df["dark"] and ar["muted"] == df["muted"], "UI", f"defaults={ {k: df[k] for k in ('dark','muted')} } now={ {k: ar[k] for k in ('dark','muted')} }")
    check("8 SETTINGS", "after reset: email equals the default (Not set)", ar["email"] == df["email"] or ar["email"] == "Not set", "UI", f"{df['email']} -> {ar['email']}")
    check("8 SETTINGS", "UI offers a global 'Reset settings to defaults' (user menu) and it works", bool(s.get("reset_button_present")) and ar["dark"] == df["dark"] and ar["muted"] == df["muted"], "NavTools.tsx UserMenu resetSettings()",
          f"button present={s.get('reset_button_present')}; other reset-like controls: {', '.join(s['reset_buttons_found']) or 'none'}")
    shot(pg, "08-after-reset")
    # final restart: reset values persist
    if not A.no_restart:
        S.disconnect()
        stop_app()
        launch_app()
        S = Session(pw, "restart2")
        assert S.connect(90)
        reload_and_login(S)
        fin = read_state(S)
        s["after_second_restart"] = fin
        check("8 SETTINGS", "reset-to-default values persist after a second restart", fin["dark"] == df["dark"] and fin["muted"] == df["muted"], "same LOCALAPPDATA", json.dumps(fin))
    return S


# ------------------------------------------------------------------------------------------------ targets + report
def targets() -> None:
    s1, s2, s3, s4, s5 = R.get("s1_inference", {}), R.get("s2_fps", {}), R.get("s3_latency", {}), R.get("s4_bandwidth", {}), R.get("s5_roi_events", {})
    prov = {m: s1.get(m, {}).get("providers", []) for m in ("ppe", "idss")}
    check("TARGETS", "inference provider is cuda (both models)", all(p and all("cuda" in x.lower() for x in p) for p in prov.values()), "task: 'ONNX Runtime CUDA'; inference event 'provider'", json.dumps(prov))
    for m in ("ppe", "idss"):
        f = (s1.get(m, {}).get("fps") or {}).get("median")
        check("TARGETS", f"{m.upper()} inference >= ~10 fps (median of 2-s samples >= 9.5)", None if f is None else f >= 9.5, "task: earlier measured ~10/s at 46/75 ms; 9.5 = 10 minus 5% rounding", f"median {f}, min {(s1.get(m, {}).get('fps') or {}).get('min')}, avg_ms median {(s1.get(m, {}).get('avg_ms') or {}).get('median')}")
    fr = (s2.get("rtc") or {}).get("framesPerSecond_reported", {}).get("median")
    dec = (s2.get("rtc") or {}).get("avg_decoded_fps")
    w, h = (s2.get("video_element") or {}).get("video_w"), (s2.get("video_element") or {}).get("video_h")
    check("TARGETS", "output stream ~12.5 fps (decoded fps within 12.5 +/- 1.0)", None if dec is None else abs(dec - OUT_FPS) <= 1.0, "models\\inference.cfg stream.output_fps = 12.5", f"avg decoded {dec}, getStats framesPerSecond median {fr}, pipeline stats fps median {(s2.get('pipeline_stats', {}).get('fps') or {}).get('median')}")
    check("TARGETS", "output stream is 1280x720", (w, h) == (1280, 720), "task: 720p output target", f"video element {w}x{h}, inbound-rtp {(s2.get('rtc') or {}).get('frameWidth')}x{(s2.get('rtc') or {}).get('frameHeight')}")
    ve, rt = s2.get("video_element") or {}, s2.get("rtc") or {}
    check("TARGETS", "dropped / corrupted frames == 0 (video element + RTP framesDropped)", None if not ve else (ve.get("dropped") == 0 and ve.get("corrupted") == 0 and rt.get("framesDropped") == 0), "task: 0", f"video dropped={ve.get('dropped')} corrupted={ve.get('corrupted')}; rtp framesDropped={rt.get('framesDropped')}, freezes={rt.get('freezeCount')}, packetsLost={rt.get('packetsLost')}")
    sync = s3.get("sync", {})
    vals = []
    for m, d in sync.items():
        lag = d.get("detection_lag_frames (arrival after frame shown, >=0)")
        if lag:
            vals.append((m, lag["p95"], lag["median"]))
    if vals:
        check("TARGETS", "detection-to-frame sync error small (p95 lag <= 2 frames = 160 ms; my threshold, none was specified)", all(v[1] <= 2.0 for v in vals), "estimate from rVFC rtpTimestamp vs detections rtp", json.dumps(vals))
    else:
        check("TARGETS", "detection-to-frame sync error small", None, "estimate", "could not associate detections with presented frames: " + json.dumps({m: d.get('note') for m, d in sync.items()})[:200])
    nerr = [e for e in R["console"] if not e["benign"]]
    check("TARGETS", "no console errors (whole run, console.error + pageerror)", len(nerr) == 0, "task", f"{len(nerr)} errors; first: {nerr[0]['text'][:140] if nerr else ''}")


def md_table(rows, head) -> str:
    out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for r in rows:
        out.append("| " + " | ".join(str(x).replace("|", "/").replace("\n", " ") for x in r) + " |")
    return "\n".join(out)


def D(d):  # distribution -> "min / median / p95 / max"
    if not d or d.get("n", 0) == 0:
        return "n/a"
    return f"{d['min']} / {d['median']} / {d['p95']} / {d['max']}"


def write_report() -> None:
    s1, s2, s3, s4 = R.get("s1_inference", {}), R.get("s2_fps", {}), R.get("s3_latency", {}), R.get("s4_bandwidth", {})
    s5, s6 = R.get("s5_roi_events", {}), R.get("s6_expert", {})
    L = []
    a = L.append
    m = R["meta"]
    a("# RigMind validation report")
    a("")
    a(f"Generated by `tests/validation/perf_validate.py` on {m.get('started')} from MEASURED numbers (raw data: `perf_validate_raw.json`). Nothing in this file is hand-written except labels.")
    a("")
    a("## 0. Run conditions")
    a("")
    a(f"- App: scratch installed-layout copy `{m.get('app_dir')}` staged from `{m.get('staged_from')}`; fresh `LOCALAPPDATA` `{m.get('local')}`; debug port {m.get('port')}; visible window.")
    a(f"- Source: `{m.get('uri')}` (2688x1512 H.264 per task), output target 12.5 fps 1280x720.")
    a(f"- Measurement window: {m.get('duration_s')} s on the Live page after {m.get('warmup_s')} s warm-up. Machine: {m.get('cpu_count')} logical CPUs, GPU {m.get('gpu_name')}.")
    wi = m.get("wait_idle", {})
    if R.get("contention_during_measure"):
        wi = dict(wi, contended=True, seen=sorted(set(wi.get("seen", [])) | set(R["contention_during_measure"])))
    a(f"- Idle wait: {wi.get('waited_s')} s; contention: **{'YES - another RigMind process was running: ' + ', '.join(wi.get('seen', [])) + ' (numbers measured under contention)' if wi.get('contended') else 'no other RigMind pipeline/app was running'}**.")
    a(f"- Inference config (`{R.get('inference_cfg_source')}`, decoded with scripts/inference_cfg.py; identical to repo copy: {R.get('inference_cfg_same_as_repo')}): ppe conf/iou {R.get('inference_cfg', {}).get('models', {}).get('ppe', {}).get('conf')}/{R.get('inference_cfg', {}).get('models', {}).get('ppe', {}).get('iou')}, idss conf/iou {R.get('inference_cfg', {}).get('models', {}).get('idss', {}).get('conf')}/{R.get('inference_cfg', {}).get('models', {}).get('idss', {}).get('iou')}, ROI scale wide 1.2 (force={R.get('inference_cfg', {}).get('auto_scale', {}).get('force')}), hold_frames {R.get('inference_cfg', {}).get('roi', {}).get('hold_frames')}, output_fps {R.get('inference_cfg', {}).get('stream', {}).get('output_fps')}.")
    a("")
    a("## Headline numbers (min / median / p95 / max)")
    a("")
    rows = []
    for mname in ("ppe", "idss"):
        x = s1.get(mname, {})
        rows.append([f"{mname.upper()} inference fps (2-s samples)", D(x.get("fps")), f"provider {x.get('providers')}"])
        rows.append([f"{mname.upper()} inference avg_ms", D(x.get("avg_ms")), f"load {x.get('load_s')} s"])
        rows.append([f"{mname.upper()} host latency_ms (frame -> event)", D(s3.get("host", {}).get(mname, {}).get("latency_ms")), "measured, host"])
    rows.append(["Pipeline output fps (stats, every 2 s)", D(s2.get("pipeline_stats", {}).get("fps")), "target 12.5"])
    rows.append(["Page decoded fps (per second, getStats framesDecoded)", D(s2.get("rtc_decoded_fps_per_s")), f"avg {(s2.get('rtc') or {}).get('avg_decoded_fps')}"])
    rows.append(["WebRTC inbound kbps (per second)", D(s4.get("webrtc_inbound_kbps")), "page bandwidth"])
    rows.append(["RTSP input kbps (pipeline stats)", D(s4.get("pipeline_rtsp_kbps")), "camera -> host"])
    rows.append(["Page decode ms / frame", D(s3.get("page", {}).get("decode_ms_per_frame")), "getStats totalDecodeTime"])
    rows.append(["Page jitter-buffer delay ms", D(s3.get("page", {}).get("jitter_buffer_delay_ms")), "getStats jitterBufferDelay"])
    rows.append(["Jitter (ms)", D((s2.get("rtc") or {}).get("jitter_ms")), ""])
    a(md_table(rows, ["Metric", "min / median / p95 / max", "note"]))
    a("")
    ve, rt = s2.get("video_element") or {}, s2.get("rtc") or {}
    a(f"Frames: video element total {ve.get('total_frames')}, dropped {ve.get('dropped')} ({ve.get('dropped_pct')} %), corrupted {ve.get('corrupted')}; RTP framesReceived {rt.get('framesReceived')}, decoded {rt.get('framesDecoded')}, framesDropped {rt.get('framesDropped')} ({rt.get('drop_pct')} %), freezeCount {rt.get('freezeCount')} ({rt.get('totalFreezesDuration_s')} s), packetsLost {rt.get('packetsLost')} ({rt.get('loss_pct')} %), nack {rt.get('nackCount')}, PLI {rt.get('pliCount')}, keyframes decoded {rt.get('keyFramesDecoded')}.")
    a("")
    a("## PASS / FAIL against targets")
    a("")
    a(md_table([[c["name"], c["status"], c["source"], c["evidence"]] for c in R["checks"] if c["section"] == "TARGETS"], ["Check", "Result", "Target source", "Measured"]))
    a("")
    a("## 1. Inference")
    a("")
    for mname in ("ppe", "idss"):
        x = s1.get(mname, {})
        a(f"**{mname.upper()}** - fps {D(x.get('fps'))}, avg_ms {D(x.get('avg_ms'))}, provider {x.get('providers')} {x.get('provider_note') or ''}, conf in events {x.get('conf_events')} (cfg {x.get('conf_cfg')}), iou {x.get('iou_events')} (cfg {x.get('iou_cfg')}), scale {x.get('scale')} {x.get('scale_mode')}.")
        a(f"detection events {x.get('detection_events')} ({x.get('detection_events_per_s')}/s), items {x.get('items_per_s')}/s, items per event {D(x.get('items_per_event'))}; frame-pts gap ms {D(x.get('pts_gap_ms'))}, gaps > 1.5 frames {x.get('pts_gaps_over_1_5_frames')}, non-monotonic {x.get('pts_non_monotonic')}.")
        a(f"Class counts (all items in window): `{json.dumps(x.get('class_counts'))}`; events containing the class: `{json.dumps(x.get('frames_with_class'))}`.")
        if mname == "idss":
            a(f"Mask polygons: {x.get('polygon_items')} of {x.get('items_total')} idss items carry a polygon.")
        a("")
    conf_ok = all(set(s1.get(k, {}).get("conf_events", [])) <= {s1.get(k, {}).get("conf_cfg")} and set(s1.get(k, {}).get("iou_events", [])) <= {s1.get(k, {}).get("iou_cfg")} for k in ("ppe", "idss"))
    a(f"Event conf/iou equal the decoded inference.cfg values: **{'PASS' if conf_ok else 'FAIL'}**. Note: inference runs on a frame-dropped copy of the 12.5 fps stream (\"Frames are dropped, never duplicated\"), so a model fps below 12.5 means frames are skipped for inference only; the video itself is unaffected.")
    a("")
    a("## 2. FPS and frame drops")
    a("")
    ps = s2.get("pipeline_stats", {})
    a(f"Pipeline stats (n={ps.get('n')}): fps {D(ps.get('fps'))}, reconnects {ps.get('reconnects_total')}, resyncs {ps.get('resyncs_start')} -> {ps.get('resyncs_end')} (in window {ps.get('resyncs_in_window')}), keyframes {ps.get('keyframes_start')} -> {ps.get('keyframes_end')}, overlay frames {ps.get('overlay_frames_end')}, drawn {ps.get('overlay_drawn_end')}, overlay avg ms {D(ps.get('overlay_avg_ms'))}, live={ps.get('live')}, gate_open={ps.get('gate_open')}.")
    a("")
    a(f"Presented cadence (rVFC): {json.dumps(s2.get('presented_cadence_ms'))}; presentedFrames span {s2.get('rvfc_presentedFrames_span')} vs callbacks {s2.get('rvfc_callbacks')} (missed callbacks {s2.get('rvfc_missed_callbacks')}).")
    a("")
    a(f"Video element: {json.dumps(s2.get('video_element'))}")
    a("")
    a(f"RTCPeerConnection inbound-rtp: {json.dumps(s2.get('rtc'))}")
    a("")
    a(f"Status events (pipeline): `{json.dumps(s2.get('status_events'))}`")
    a("")
    a("## 3. Latency")
    a("")
    a("**Measured (host):** `detections.latency_ms` = pipeline wall clock at event emit minus the frame's arrival timestamp (`ts`); includes queue wait + preprocessing + inference + postprocessing; `ms` = inference+postprocess duration.")
    a("")
    hrows = []
    for mname in ("ppe", "idss"):
        h = s3.get("host", {}).get(mname, {})
        for k, v in h.items():
            hrows.append([mname, k, D(v)])
    a(md_table(hrows, ["model", "quantity (ms)", "min / median / p95 / max"]))
    a("")
    a("**Measured (page):** decode time and jitter buffer from getStats; receive->present from requestVideoFrameCallback.")
    a("")
    a(md_table([[k, D(v)] for k, v in s3.get("page", {}).items()], ["quantity", "min / median / p95 / max"]))
    a("")
    a(f"rVFC fields available in this WebView2: {json.dumps(s3.get('rvfc_fields_available'))}.")
    a("")
    a("**Estimated (glass-to-glass / overlay sync):** detections events are associated with presented video frames through the RTP timestamp (`detections.rtp` vs rVFC `rtpTimestamp`); both clocks are the same machine, so `frame_age_at_presentation = presentationTime - pipeline frame ts` is the pipeline-in to glass delay (excludes camera encode and the RTSP network leg, which cannot be measured without camera clock access). The overlay is burned into the video by the pipeline (`burned_in: true`), so on-screen boxes cannot drift from the picture; the sync figure below describes when the detection DATA (lists, EWS, bell) reaches the page relative to the frame it describes.")
    a("")
    srows = []
    for mname, d in s3.get("sync", {}).items():
        if d.get("note"):
            srows.append([mname, d.get("matched"), d.get("note"), "", "", ""])
            continue
        srows.append([mname, f"{d['matched']}/{d['of']} ({d['method']})", D(d["detection_arrival_minus_frame_presentation_ms"]), D(d["detection_lag_frames (arrival after frame shown, >=0)"]), D(d["frame_age_at_presentation_ms (presentation - pipeline frame ts)"]), d["frames_with_lag_over_1_frame_pct"]])
    a(md_table(srows, ["model", "matched frames", "arrival - presentation ms", "lag frames", "frame age at presentation ms", "% frames lag > 1 frame"]))
    a("")
    a("## 4. Bandwidth and resources")
    a("")
    be = s4.get("bridge_events", {})
    a(f"WebRTC inbound kbps {D(s4.get('webrtc_inbound_kbps'))}; RTSP input kbps {D(s4.get('pipeline_rtsp_kbps'))}. Bridge/event traffic into the page: {be.get('total_msgs_per_s')} msg/s (per-second {D(be.get('msgs_per_s_distribution'))}), {be.get('total_bytes_per_s')} B/s = {be.get('total_kbps')} kbps.")
    a("")
    a(md_table([[k, v["msgs_per_s"], v["bytes_per_s"], v["avg_bytes"]] for k, v in (be.get("per_event_type") or {}).items()], ["event", "msgs/s", "bytes/s", "avg bytes"]))
    a("")
    a(be.get("note", ""))
    a("")
    rr = []
    for k, v in (s4.get("resources") or {}).items():
        if k == "_system":
            continue
        rr.append([k, D(v["cpu_pct_of_one_core"]), D(v["cpu_pct_of_machine"]), D(v["rss_mb"])])
    a(md_table(rr, ["process (RigMind tree)", "CPU % of one core", "CPU % of machine", "RSS MB (sum of processes)"]))
    a("")
    sysd = (s4.get("resources") or {}).get("_system", {})
    a(f"System CPU {D(sysd.get('cpu_pct'))} %, free RAM {D(sysd.get('free_ram_mb'))} MB ({sysd.get('logical_cpus')} logical CPUs). GPU ({s4.get('gpu_total_mb')} MB total): `{json.dumps(s4.get('gpu'))}` (whole-GPU nvidia-smi, 5-s samples).")
    a("")
    a("## 5. ROIs")
    a("")
    a("### 5a. Platform-based ROI (from live idss events)")
    a("")
    a(f"idss events {s5.get('idss_events')}; with a Platform {s5.get('events_with_platform')}; ROI equals the Platform box scaled by {s5.get('scale_used')} about its centre (clamped to the frame): {s5.get('roi_matches_scaled_platform')} match, {s5.get('roi_mismatch')} mismatch, worst error {s5.get('worst_error_px')} px (tolerance {s5.get('tolerance_px')} px for integer rounding). Examples of mismatch: `{json.dumps(s5.get('mismatch_examples'))}`.")
    a("")
    a(f"`in_roi` flags re-computed from each item's centroid vs the event ROI (Platform always in): {s5.get('in_roi_items_ok')} correct, {s5.get('in_roi_items_wrong')} wrong. In/out by class `[in, out]`: `{json.dumps(s5.get('in_roi_by_class [in, out]'))}`.")
    a("")
    a(f"hold_frames behaviour observed: events without a Platform {s5.get('events_without_platform')}, of which a held ROI was still sent {s5.get('held_roi_events_without_platform')} (longest run {s5.get('longest_held_run_events')} events, cfg hold_frames {s5.get('hold_frames_cfg')}). The hold/release logic itself is covered by the unit test `tests/roi_facts_test.cpp` (update_roi); run result here: `{json.dumps(R.get('unit_tests'))}`.")
    a("")
    z = R.get("s5_zones", {})
    a("### 5b. User-drawn zones (Administration > Zone editor)")
    a("")
    for c in R["checks"]:
        if c["section"] == "5 ROI":
            a(f"- **{c['status']}** {c['name']} - {c['evidence']}")
    a("")
    a(f"Controls seen: `{z.get('controls_seen')}`. Code references to the zone record (searched `Rigmind_v2/src` and `RigMind_Desktop/src`):")
    a("")
    for h in z.get("code_references", [])[:14]:
        a(f"    {h}")
    a("")
    a("Screenshots: `screenshots/05-zone-*.png`.")
    a("")
    a("## 6. Expert outputs")
    a("")
    a("**PPE classes** (live): " + json.dumps(s1.get("ppe", {}).get("class_counts")) + ". **IDSS classes** (live): " + json.dumps(s1.get("idss", {}).get("class_counts")) + ".")
    a("")
    a(f"**EWS facts** ({s6.get('facts_events')} diagnostic events, percentage of frames the fact was true, mean / max over the window; first column is what the EWS rules saw after the ROI, the last column with no ROI):")
    a("")
    a(md_table([[n, v["mean"], v["max"], s6.get("facts_pct_no_roi_mean", {}).get(n)] for n, v in s6.get("facts_pct", {}).items()], ["fact", "mean %", "max %", "mean % (no ROI)"]))
    a("")
    a(f"Last facts snapshot: `{json.dumps(s6.get('facts_last'))}`.")
    a("")
    a(f"Real alarm events from the live stream: `{json.dumps(s6.get('alarm_events_live'))}`. Evidence/clip events during the live window: `{json.dumps(s6.get('evidence_clip_events'))}`. Event counts: `{json.dumps(s6.get('other_event_counts'))}`. Pipeline error events: `{json.dumps(s6.get('errors_events'))}`.")
    a("")
    o = R.get("s6_outputs", {})
    a("**Scripted episode (bell > Test notification = `alarm.simulate` red with the camera) and cross-checks:**")
    a("")
    for c in R["checks"]:
        if c["section"] == "6 EXPERT":
            a(f"- **{c['status']}** {c['name']} - {c['evidence']}")
    a("")
    a(f"Counts: `{json.dumps(o.get('counts'))}`; clip event `{json.dumps(o.get('clip_event'))}` after {o.get('clip_wait_s')} s; events after the test `{json.dumps(o.get('events_since_test'))}`; footage playback `{json.dumps(o.get('footage_playback'))}`; disk `{o.get('disk_root')}` -> `{json.dumps(o.get('disk_listing'))}`.")
    a("")
    a(f"Sample rows: alerts.feed `{json.dumps(o.get('alerts_feed_sample', [])[:3])}`; audit `{json.dumps(o.get('audit_sample', [])[:3])}`; footage `{json.dumps(o.get('footage_list', [])[:2])}`.")
    a("")
    a(o.get("note_sim_vs_episode", ""))
    a("")
    a("## 7. Model summaries")
    a("")
    s7 = R.get("s7_summary", {})
    a(f"BitNet binaries in app dir ({s7.get('expected_paths')}): **{s7.get('slm_binaries_in_app')}**; port 8791 open: {s7.get('port_8791_open')}. Bridge methods that describe/summarise: `{s7.get('bridge_methods_with_describe_or_summary')}`. Camera status summary: {s7.get('status_summary_request')}.")
    a("")
    for r in s7.get("requests", []):
        a(f"- `{r['alertId']}`: reply {r['reply_ms']} ms (host returns template text at once), description event after {r['description_wait_s']} s, source `{(r.get('description_event') or {}).get('source')}`; non-empty {r['non_empty']}; grounded {json.dumps(r['grounded'])}; sample names {r['sample_names']}.  \n  Text: _{r['text_shown']}_")
    a("")
    a(f"UI toast after bell > Test notification: `{json.dumps(o.get('ui_toast_plain_words'))}`. Stored plain summaries (`plain:` records): `{json.dumps(s7.get('stored_plain_records'))[:400]}`. {s7.get('ui_note', '')}")
    a("")
    for c in R["checks"]:
        if c["section"] == "7 SUMMARY":
            a(f"- **{c['status']}** {c['name']} - {c['evidence']}")
    a("")
    a("## 8. Pages, reports, settings")
    a("")
    s8 = R.get("s8_pages", {})
    a(f"Sample/demo data flag in the page: {s8.get('demo_data')}.")
    a("")
    a(md_table([[p["label"], p["path"], p["chars"], "ok" if p["route_ok"] and not p["error_text"] else "PROBLEM", p["sample_strings"] or "-", len(p["console_errors"])] for p in s8.get("pages", [])], ["page", "route", "text chars", "render", "sample strings", "console errors"]))
    a("")
    a(md_table([[r.get("type"), r.get("opened"), r.get("path"), r.get("chars"), r.get("pdf_bytes"), r.get("has_plain_summary_section"), r.get("sample_strings") or "-", r.get("mentions_new_alert_ids")] for r in s8.get("reports", [])], ["report", "opened", "route", "chars", "pdf bytes", "'plain' text", "sample strings", "new alert ids in report"]))
    a("")
    a(f"Analytics page mentions the new alert ids: {s8.get('analytics_mentions_new_alert_ids')}.")
    a("")
    st = R.get("s8_settings", {})
    a(f"Defaults read: `{json.dumps(st.get('defaults'))}`; changed: `{json.dumps(st.get('changed'))}`; after restart: `{json.dumps(st.get('after_restart'))}`; after reset: `{json.dumps(st.get('after_reset'))}`; after second restart: `{json.dumps(st.get('after_second_restart'))}`.")
    a("")
    a(f"Reset controls: `{json.dumps(st.get('reset_controls'))}`; buttons matching reset/default found on admin pages: `{st.get('reset_buttons_found')}`.")
    a("")
    for c in R["checks"]:
        if c["section"] in ("8 SETTINGS", "8 REPORT"):
            a(f"- **{c['status']}** {c['name']} - {c['evidence']}")
    a("")
    a("## All checks")
    a("")
    a(md_table([[c["section"], c["name"], c["status"], c["source"], c["evidence"][:160]] for c in R["checks"]], ["section", "check", "result", "source / target", "evidence"]))
    a("")
    a("## Console / page errors")
    a("")
    cs = R["console"]
    a(f"{len([e for e in cs if not e['benign']])} errors, {len([e for e in cs if e['benign']])} benign (fonts/favicon/offline).")
    for e in cs[:25]:
        a(f"- `{e['where']}` {'(benign) ' if e['benign'] else ''}{e['kind']}: {e['text'][:200]}")
    a("")
    a("## Harness errors (phases that failed)")
    a("")
    for e in R["errors"] or [{"phase": "none", "error": "none"}]:
        a(f"- {e['phase']}: {e['error']}")
    a("")
    a("## Notes / what could not be measured")
    a("")
    for n in R["notes"]:
        a(f"- {n}")
    (HERE / "VALIDATION_REPORT.md").write_text("\n".join(L), encoding="utf-8")


# ------------------------------------------------------------------------------------------------ main
def main() -> int:
    global A, SHOTS, PACE
    ap = argparse.ArgumentParser()
    ap.add_argument("--app-dir", default=str(DEFAULT_APP))
    ap.add_argument("--seed-dir", default=str(DEFAULT_SEED))
    ap.add_argument("--run-root", default=str(DEFAULT_RUNROOT))
    ap.add_argument("--port", type=int, default=9733)
    ap.add_argument("--duration", type=int, default=330, help="stream measurement seconds (>= 300 for the real run)")
    ap.add_argument("--warmup", type=int, default=20)
    ap.add_argument("--uri", default=RTSP_URI)
    ap.add_argument("--skip-wait", action="store_true")
    ap.add_argument("--wait-idle-max", type=int, default=2400)
    ap.add_argument("--no-restart", action="store_true")
    ap.add_argument("--no-stage", action="store_true")
    ap.add_argument("--pace", type=float, default=1.0)
    ap.add_argument("--from-raw", action="store_true", help="only regenerate VALIDATION_REPORT.md from perf_validate_raw.json")
    ap.add_argument("--keep-open", action="store_true", help="leave the app running at the end")
    A = ap.parse_args()
    PACE = A.pace
    if A.from_raw:
        R.update(json.loads((HERE / "perf_validate_raw.json").read_text(encoding="utf-8")))
        write_report()
        return 0
    SHOTS = HERE / "screenshots"
    SHOTS.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    APP["local"] = Path(A.run_root) / f"validate_{stamp}" / "Local"
    APP["local"].mkdir(parents=True, exist_ok=True)
    R["meta"].update({"started": dt.datetime.now().isoformat(timespec="seconds"), "app_dir": A.app_dir, "local": str(APP["local"]), "port": A.port, "uri": A.uri, "duration_s": A.duration, "warmup_s": A.warmup,
                      "cpu_count": psutil.cpu_count(), "python": sys.version.split()[0]})
    try:
        out = subprocess.run([NVSMI, "--query-gpu=name,driver_version", "--format=csv,noheader"], capture_output=True, text=True, timeout=10).stdout.strip() if NVSMI else "no nvidia-smi"
        R["meta"]["gpu_name"] = out
    except Exception:  # noqa: BLE001
        R["meta"]["gpu_name"] = "unknown"
    if A.duration < 300:
        R["notes"].append(f"SHORT RUN: duration {A.duration} s < 300 s; numbers are a dry run, not the required >= 5 minute measurement.")
    from playwright.sync_api import sync_playwright

    R["meta"]["wait_idle"] = wait_idle()
    if R["meta"]["wait_idle"].get("contended"):
        R["notes"].append("Numbers were measured under CONTENTION (another RigMind process was running): " + ", ".join(R["meta"]["wait_idle"]["seen"]))
    if not A.no_stage:
        stage_app()
    load_cfg()
    guarded("unit tests", run_unit_tests)
    S = None
    try:
        with sync_playwright() as pw:
            launch_app()
            S = Session(pw, "first")
            if not S.connect(90):
                raise RuntimeError("could not reach the WebView2 debug port")
            reload_and_login(S)
            shot(S.page, "00-after-login")
            guarded("add camera", add_camera, S)
            # camera id from the first status / stats event
            S.nav("Live monitoring")
            t0 = time.time()
            R["cam"] = None
            while time.time() - t0 < 90 and not R["cam"]:
                drain(S.page, DATA)
                for o in DATA["other"]:
                    if o.get("cam"):
                        R["cam"] = o["cam"]
                        break
                time.sleep(1)
            log(f"camera id {R['cam']}")
            if not R["cam"]:
                raise RuntimeError("no camera events seen")
            guarded("measure", measure, S)
            if DATA["samples"]:
                guarded("analyze", analyze_measurement)
            guarded("zones", phase_zones, S)
            guarded("experts", phase_experts, S)
            guarded("summaries", phase_summaries, S)
            guarded("pages", phase_pages, S)
            S2 = guarded("settings", phase_settings, S, pw)
            if isinstance(S2, Session):
                S = S2
            guarded("reload probe", probe_reload, S)
            guarded("targets", targets)
            R["notes"] += [
                "Camera-to-pipeline (RTSP + camera encode) latency cannot be measured: the camera clock is not accessible; 'frame age at presentation' starts at the pipeline's frame arrival timestamp.",
                "Model fps/avg_ms come from the pipeline's own 'inference' events (2-s windows); GPU utilisation is whole-GPU from nvidia-smi, not per process.",
                "RigMind.exe / pipeline / msedgewebview2 CPU+RSS are psutil samples of the process tree started by this script (RSS counts shared pages once per process).",
                "Bridge byte counts are JSON.stringify lengths of the messages the page received (an estimate of the bridge payload).",
                "The scripted red episode comes from alarm.simulate (host state machine), not from the camera feed; a real alarm episode is only reported if the stream produced one.",
            ]
    finally:
        if S:
            S.disconnect()
        R["meta"]["finished"] = dt.datetime.now().isoformat(timespec="seconds")
        for k in ("det", "other", "rvfc"):
            R.setdefault("raw_counts", {})[k] = len(DATA[k])
        R["raw_sample_events"] = {"other_tail": DATA["other"][-40:], "det_head": DATA["det"][:20], "samples_head": DATA["samples"][:3]}
        R["gpu_series"] = GPU
        R["samples_series"] = [{"t": s["t"], "vq": s.get("vq"), "inb": {k: s["inb"].get(k) for k in ("bytesReceived", "framesReceived", "framesDecoded", "framesDropped", "framesPerSecond", "freezeCount", "packetsLost", "jitter", "totalDecodeTime", "jitterBufferDelay", "jitterBufferEmittedCount", "nackCount", "pliCount")} if s.get("inb") else None} for s in DATA["samples"]]
        (HERE / "perf_validate_raw.json").write_text(json.dumps(R, indent=1, default=str), encoding="utf-8")
        try:
            write_report()
        except Exception as e:  # noqa: BLE001
            log(f"report generation failed: {e}\n{traceback.format_exc()}")
        if not A.keep_open:
            stop_app()
    fails = [c for c in R["checks"] if c["status"] == "FAIL"]
    log(f"done: {len(R['checks'])} checks, {len(fails)} FAIL, report {HERE / 'VALIDATION_REPORT.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
