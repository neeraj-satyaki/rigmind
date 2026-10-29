"""UI smoke test for the RigMind SPA served from RigMind_Desktop/web.

Run from a venv only:
    /tmp/pwenv/bin/python tests/ui/smoke_test.py http://127.0.0.1:8765 <screenshot_dir>

Uses the system Google Chrome (channel="chrome"). Fails on any uncaught page error or
on a route that renders no body text. Google Fonts requests are ignored (offline box).
"""

import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8765"
SHOTS = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("/tmp")
ROUTES = ["/login", "/", "/live", "/alerts", "/events", "/reports", "/analytics/overview", "/admin/cameras/new", "/admin/zones"]
IGNORED_HOSTS = ("fonts.googleapis.com", "fonts.gstatic.com")


def interactions(page, failures: list[str]) -> None:
    """Acknowledge an alert from the alerts list and from the live page; check the UI reacts."""
    page.goto(BASE + "/alerts", wait_until="networkidle", timeout=20000)
    page.wait_for_timeout(1500)
    page.get_by_role("button", name="Acknowledge", exact=True).first.click()
    if not page.get_by_text("acknowledged", exact=False).first.is_visible(timeout=5000):
        failures.append("/alerts: no acknowledged toast after clicking Acknowledge")
    print("/alerts acknowledge: checked")

    page.goto(BASE + "/live", wait_until="networkidle", timeout=20000)
    page.wait_for_timeout(1500)
    button = page.get_by_role("button", name="Acknowledge alert")
    if button.count():
        button.first.click()
        if not page.get_by_text("Acknowledged by", exact=False).first.is_visible(timeout=5000):
            failures.append("/live: alert not shown as acknowledged after click")
        print("/live acknowledge: checked")
    else:
        print("/live acknowledge: button already acknowledged, skipped")


def main() -> int:
    failures: list[str] = []
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="chrome", headless=True)
        page = browser.new_page(viewport={"width": 1440, "height": 900})

        errors: list[str] = []
        page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        page.on(
            "requestfailed",
            lambda r: None if any(h in r.url for h in IGNORED_HOSTS) else errors.append(f"requestfailed: {r.url}"),
        )

        for route in ROUTES:
            errors.clear()
            try:
                page.goto(BASE + route, wait_until="networkidle", timeout=20000)
                page.wait_for_timeout(1500)  # boot splash and 700 ms page transition
                text = page.inner_text("body").strip()
                if not text:
                    failures.append(f"{route}: empty body")
                page.screenshot(path=str(SHOTS / f"{route.strip('/').replace('/', '_') or 'home'}.png"))
            except Exception as exc:  # navigation or timeout
                failures.append(f"{route}: {exc}")
                continue
            for err in errors:
                failures.append(f"{route}: {err}")
            print(f"{route}: {'FAIL' if any(f.startswith(route + ':') for f in failures) else 'ok'}")

        try:
            interactions(page, failures)
        except Exception as exc:
            failures.append(f"interactions: {exc}")
        for err in errors:
            failures.append(f"interactions: {err}")

        browser.close()

    if failures:
        print("\n".join(failures))
        return 1
    print("ui smoke ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
