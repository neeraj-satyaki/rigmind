"""Listen to the running RigMind page for 45 s: alarm events, and where derrickman / machine / platform are in frame pixels."""
import json, time
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b = p.chromium.connect_over_cdp("http://127.0.0.1:9222")
    page = [x for c in b.contexts for x in c.pages if "rigmind.app" in x.url][0]
    if "/login" in page.url:
        page.get_by_role("button", name="Sign in to workspace").first.click(); page.wait_for_timeout(1500)
    page.evaluate("r=>{history.pushState({},'',r);dispatchEvent(new PopStateEvent('popstate'))}", "/live")
    page.evaluate("""() => { window.__q = {alarm: [], frames: 0, classes: {}, cx: {}, roi: null, platform: null, w: null, h: null, bell: null};
      window.chrome.webview.addEventListener('message', e => { const d = e.data; if (!d || !d.ev) return;
        if (d.ev === 'alarm') window.__q.alarm.push({state: d.state, condition: d.condition, place: d.place});
        if (d.ev === 'detections' && d.model === 'idss') { const q = window.__q; q.frames++; q.w = d.w; q.h = d.h; q.roi = d.roi;
          q.platform = d.platform ? d.platform.box : null;
          for (const it of d.items || []) { q.classes[it.label] = (q.classes[it.label] || 0) + 1;
            const c = (it.box[0] + it.box[2]) / 2; (q.cx[it.label] = q.cx[it.label] || []).push(Math.round(c)); } } }); }""")
    time.sleep(45)
    q = page.evaluate("window.__q")
    print("idss frames seen:", q["frames"], "| frame size:", q["w"], "x", q["h"])
    print("alarm events received:", len(q["alarm"]), q["alarm"][:5])
    print("classes seen (count):", q["classes"])
    for k, v in q["cx"].items():
        print(f"  centre-x of {k:12}: min {min(v)}  max {max(v)}  mean {sum(v)//len(v)}")
    print("last ROI:", q["roi"], "| last platform box:", q["platform"])
