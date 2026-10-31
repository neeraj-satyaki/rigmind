"""Regression check for "streaming stops once I change the page" (leave /live, come back).
usage: python nav_resume_check.py <app_dir> <port> <cycles> <out.json> [KEY=VAL env ...]
  <app_dir>  folder with RigMind.exe (a private copy of the installed layout; never the installed folder itself)
  <port>     WebView2 remote-debugging port (not one used by another run)
Env: OFFSECS (seconds away from Live, default 15), ONSECS (seconds on Live between cycles, default 5), MINDET (minimum
detection messages while away, default 2 x OFFSECS).
Per cycle it leaves Live for /alerts or /fleet, counts 'detections' events that keep arriving (monitoring must run with nobody
watching), returns to /live and requires the video to play again within 3 s and keep advancing. Exit code 1 = FAIL.
Needs the camera at rtsp://192.168.1.8:8554/feed0 (the default source) and Playwright. Only the processes it started are stopped.
Slow-model check: pass RIGMIND_TEST_INFER_DELAY_MS=2500 (and MINDET=2): the video must still resume and events keep flowing."""
import os, subprocess, sys, time, json, urllib.request, shutil
from playwright.sync_api import sync_playwright
app, port, cycles, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
loc = os.path.join(os.path.dirname(os.path.abspath(out)), f"loc{port}")
shutil.rmtree(loc, ignore_errors=True); os.makedirs(loc)
env = {**os.environ, "LOCALAPPDATA": loc, "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS": f"--remote-debugging-port={port} --remote-allow-origins=*"}
for kv in sys.argv[5:]:
    k, v = kv.split("=", 1); env[k] = v
p = subprocess.Popen([os.path.join(app, "RigMind.exe")], cwd=app, env=env)
INIT = r"""(() => { if (window.__nv) return; const nv = window.__nv = {det: 0, ev: {}, alarm: []};
 const hook = () => { try { window.chrome.webview.addEventListener('message', e => { const d = e.data; if (!d || !d.ev) return;
   nv.ev[d.ev] = (nv.ev[d.ev]||0)+1; if (d.ev === 'detections') nv.det++; if (d.ev === 'alarm') nv.alarm.push(d.state); }); } catch(x) { setTimeout(hook, 50); } };
 hook(); })();"""
for i in range(40):
    try: urllib.request.urlopen(f"http://127.0.0.1:{port}/json/version", timeout=2).read(); break
    except Exception: time.sleep(1)
res = {"cycles": []}
def go(pg, r): pg.evaluate("r=>{history.pushState({},'',r);dispatchEvent(new PopStateEvent('popstate'))}", r)
def vid(pg): return pg.evaluate("(()=>{const v=document.querySelector('video');return v?{w:v.videoWidth,t:v.currentTime,paused:v.paused}:null})()")
def playing(pg):
    v = vid(pg); return bool(v and v["w"] > 0 and not v["paused"])
with sync_playwright() as pw:
    b = pw.chromium.connect_over_cdp(f"http://127.0.0.1:{port}")
    pg = [x for c in b.contexts for x in c.pages if "rigmind.app" in x.url][0]
    pg.evaluate(INIT); time.sleep(2)
    if "/login" in pg.url:
        pg.get_by_role("button", name="Sign in to workspace").first.click(timeout=60000); pg.wait_for_timeout(1500)
    go(pg, "/live"); t0 = time.time()
    while time.time() - t0 < 90 and not playing(pg): time.sleep(0.5)
    res["first_play_s"] = round(time.time() - t0, 1); print("first play", res["first_play_s"], flush=True)
    time.sleep(8)
    for c in range(cycles):
        d0 = pg.evaluate("window.__nv.det"); a0 = len(pg.evaluate("window.__nv.alarm"))
        go(pg, ["/alerts", "/fleet"][c % 2]); time.sleep(float(os.environ.get("OFFSECS","15")))
        d1 = pg.evaluate("window.__nv.det")
        go(pg, "/live"); t1 = time.time(); ok = False
        while time.time() - t1 < 20:
            if playing(pg): ok = True; break
            time.sleep(0.25)
        resume = round(time.time() - t1, 2)
        t_a = vid(pg)["t"] if ok else None; time.sleep(3); t_b = vid(pg)["t"] if ok else None
        r = {"cycle": c + 1, "detections_off_page_15s": d1 - d0, "resumed": ok, "resume_s": resume, "advancing": (t_b is not None and t_a is not None and t_b > t_a)}
        res["cycles"].append(r); print(r, flush=True)
        time.sleep(float(os.environ.get("ONSECS","5")))
    res["alarms"] = pg.evaluate("window.__nv.alarm"); res["ev"] = pg.evaluate("window.__nv.ev")
def stop_own_tree():
    try:
        import psutil
        root = psutil.Process(p.pid)
        for child in root.children(recursive=True):
            try: child.kill()
            except psutil.Error: pass
        root.kill()
    except Exception:
        p.terminate()
stop_own_tree()
json.dump(res, open(out, "w"))
bad = [r for r in res["cycles"] if r["detections_off_page_15s"] < float(os.environ.get("MINDET", 2*float(os.environ.get("OFFSECS","15")))) or not r["resumed"] or r["resume_s"] > 3 or not r["advancing"]]
print("alarms", res["alarms"]); print("NAV REGRESSION CHECK:", "FAIL %s" % bad if bad else "PASS")
sys.exit(1 if bad else 0)
