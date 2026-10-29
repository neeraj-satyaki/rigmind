"""End-to-end UI test through the native path: UI -> bridge shim -> host.py -> rigmind-bridge-cli -> SQLite.

Checks in the SQLite file, not only in the page:
  - acknowledging an alert writes alert_responses
  - the bell's "Test notification" writes a raised alert row
  - theme toggle writes the preferences record

Run from a venv (Playwright). Needs the Linux build output:
    /tmp/pwenv/bin/python tests/ui/native_test.py <web_dir> <bridge_cli> <host.py> <screenshot_dir>
"""

import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

WEB, CLI, HOST, SHOTS = sys.argv[1], sys.argv[2], sys.argv[3], Path(sys.argv[4])
PORT = 8767
BASE = f"http://127.0.0.1:{PORT}"


def query(db: str, sql: str):
    con = sqlite3.connect(db)
    try:
        return con.execute(sql).fetchall()
    finally:
        con.close()


def wait_for(check, timeout: float = 8.0):
    end = time.time() + timeout
    while time.time() < end:
        result = check()
        if result:
            return result
        time.sleep(0.2)
    return None


def main() -> int:
    tmp = tempfile.mkdtemp(prefix="rigmind-native-")
    db = str(Path(tmp) / "rigmind.db")
    host = subprocess.Popen(["python3", HOST, WEB, str(PORT), CLI, db])
    failures: list[str] = []
    try:
        wait_for(lambda: subprocess.run(["curl", "-sf", BASE + "/"], capture_output=True).returncode == 0, 5)

        with sync_playwright() as p:
            browser = p.chromium.launch(channel="chrome", headless=True)
            page = browser.new_page(viewport={"width": 1440, "height": 900})
            # Record every media play() request so the tone can be checked without relying on audio output.
            page.add_init_script(
                "window.__played = [];"
                "const __play = HTMLMediaElement.prototype.play;"
                "HTMLMediaElement.prototype.play = function () { window.__played.push(this.src); return __play.call(this); };"
            )
            errors: list[str] = []
            page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))

            page.goto(BASE + "/alerts", wait_until="networkidle", timeout=20000)
            page.wait_for_timeout(1500)
            if not page.evaluate("!!(window.chrome && window.chrome.webview)"):
                failures.append("shim missing: page is not on the native path")

            # 1. Acknowledge an alert -> alert_responses row
            page.get_by_role("button", name="Acknowledge", exact=True).first.click()
            if not wait_for(lambda: query(db, "SELECT id FROM alert_responses WHERE status='Acknowledged'")):
                failures.append("acknowledge: no Acknowledged row in alert_responses")
            else:
                print("acknowledge -> alert_responses: ok")

            # 1b. The click itself is audited in SQLite (UI capture -> audit.write -> audit_log)
            if not wait_for(lambda: query(db, "SELECT id FROM audit_log WHERE source='ui' AND action='click' AND target LIKE 'Acknowledge%'")):
                failures.append("audit: acknowledge click not in audit_log")
            else:
                print("click -> audit_log: ok")

            # 2. Bell -> Test notification -> raised alert row
            page.get_by_role("button", name="Notifications").first.click()
            page.get_by_role("button", name="Test notification").first.click()
            if not wait_for(lambda: query(db, "SELECT id FROM alerts WHERE severity='Critical'")):
                failures.append("test notification: no raised alert row in alerts")
            else:
                print("test notification -> alerts: ok")
            try:
                page.wait_for_function("window.__played.some(s => s.includes('redAlert'))", timeout=8000)
                print("test notification -> red tone requested: ok")
            except Exception:
                failures.append("test notification: red tone was not played")
            page.keyboard.press("Escape")

            # 3. Theme toggle -> preferences record
            page.get_by_role("button", name="Dark theme").first.click()
            if not wait_for(lambda: query(db, "SELECT value FROM app_records WHERE key='preferences' AND value LIKE '%\"dark\":true%'")):
                failures.append("theme: preferences record not written with dark=true")
            else:
                print("theme -> app_records: ok")

            page.screenshot(path=str(SHOTS / "native_alerts.png"))
            for err in errors:
                failures.append(f"alerts: {err}")
            browser.close()
    finally:
        host.terminate()
        host.wait(timeout=5)

    if failures:
        print("\n".join(failures))
        return 1
    print("native ui test ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
