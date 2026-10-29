"""Listen to the running RigMind page for N s (argv[1], default 60): alarm events and the 'facts' diagnostic; prints the
per-condition % of frames (ROI on = what the EWS sees, ROI off = same detections unfiltered) and the angle distribution."""
import json, sys, time, statistics
from playwright.sync_api import sync_playwright
secs = int(sys.argv[1]) if len(sys.argv) > 1 else 60
with sync_playwright() as p:
    b = p.chromium.connect_over_cdp("http://127.0.0.1:9222")
    page = [x for c in b.contexts for x in c.pages if "rigmind.app" in x.url][0]
    if "/login" in page.url:
        page.get_by_role("button", name="Sign in to workspace").first.click(); page.wait_for_timeout(1500)
    page.evaluate("r=>{history.pushState({},'',r);dispatchEvent(new PopStateEvent('popstate'))}", "/live")
    page.evaluate("""() => { window.__fp = {alarm: [], facts: []};
      window.chrome.webview.addEventListener('message', e => { const d = e.data; if (!d || !d.ev) return;
        if (d.ev === 'alarm') window.__fp.alarm.push(d);
        if (d.ev === 'facts') window.__fp.facts.push(d); }); }""")
    time.sleep(secs)
    q = page.evaluate("window.__fp")
fs = q["facts"]; n = sum(f["frames"] for f in fs)
print("idss frames:", n, "in", len(fs), "windows; alarm events:", len(q["alarm"]))
for a in q["alarm"]: print("ALARM", json.dumps(a))
names = list(fs[0]["pct"]) if fs else []
print(f"{'fact':32}{'ROI on %':>10}{'ROI off %':>11}")
for k in names:
    on = sum(f["pct"][k] * f["frames"] for f in fs) / max(n, 1); off = sum(f["pct_no_roi"][k] * f["frames"] for f in fs) / max(n, 1)
    print(f"{k:32}{on:10.1f}{off:11.1f}")
ang = sorted(a for f in fs for a in f["angles"])
if ang: print("angle_deg (centroid link-tilt->machine, no ROI): n=%d min %.1f median %.1f max %.1f" % (len(ang), ang[0], statistics.median(ang), ang[-1]))
print("last facts:", json.dumps(fs[-1]) if fs else None)
