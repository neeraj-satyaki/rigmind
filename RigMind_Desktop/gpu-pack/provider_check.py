"""Attach to the running RigMind (debug port 9222), open Live, and report the inference provider and speed."""
import json, time
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b = p.chromium.connect_over_cdp("http://127.0.0.1:9222")
    page = [x for c in b.contexts for x in c.pages if "rigmind.app" in x.url][0]
    page.evaluate("""() => { window.__p = {providers: {}, stats: {}, n: 0};
      window.chrome.webview.addEventListener('message', e => { const d = e.data; if (!d || !d.ev) return;
        if (d.ev === 'inference') { window.__p.providers[d.model] = d.provider; window.__p.stats[d.model] = {fps: d.fps, avg_ms: d.avg_ms, load_s: d.load_s}; }
        if (d.ev === 'detections') window.__p.n++; }); }""")
    if "/login" in page.url:
        page.get_by_role("button", name="Sign in to workspace").first.click(); page.wait_for_timeout(1500)
    page.evaluate("r=>{history.pushState({},'',r);dispatchEvent(new PopStateEvent('popstate'))}", "/live")
    t0 = time.time(); ok = False
    while time.time() - t0 < 60:
        if page.evaluate("document.querySelector('video') ? document.querySelector('video').videoWidth : 0") > 0: ok = True; break
        time.sleep(1)
    time.sleep(20)
    print("video playing:", ok)
    print(json.dumps(page.evaluate("window.__p")))
