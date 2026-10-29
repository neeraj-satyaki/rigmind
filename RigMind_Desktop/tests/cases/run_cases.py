"""Runs every row of tests/cases/test_cases.csv against rigmind-bridge-cli (C++ backend).

Usage: python3 tests/cases/run_cases.py <rigmind-bridge-cli> [results.csv]
Exit code 0 only if every case passes. One CLI process and a fresh SQLite file per run.
"""

import csv
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).parent
CASES = HERE / "test_cases.csv"


class Cli:
    def __init__(self, binary: str, db: str):
        self.proc = subprocess.Popen([binary, db], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.next_id = 0

    def send(self, request) -> dict:
        self.next_id += 1
        line = request if isinstance(request, str) else json.dumps(
            {"id": self.next_id, **request}, ensure_ascii=False)
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline()
        if not reply:
            raise RuntimeError("bridge cli exited")
        return json.loads(reply)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def check_alarm(cli, inp, exp):
    cli.send({"method": "alarm.reset", "params": {"zoom": inp["zoom"]}})
    states = []
    for frame in inp["frames"]:
        reply = cli.send({"method": "alarm.step", "params": frame})
        if not reply.get("ok"):
            return False, f"alarm.step error: {reply.get('error')}"
        states.append(reply["result"]["state"])
    if states != exp["states"]:
        first = next((i for i, (a, b) in enumerate(zip(states, exp["states"])) if a != b), None)
        return False, f"states differ at frame {first}: got {states[first] if first is not None else '-'}"
    return True, ""


def check_bridge(cli, inp, exp):
    reply = cli.send(inp["request"])
    if reply.get("ok") != exp["ok"]:
        return False, f"ok={reply.get('ok')} expected {exp['ok']}: {reply.get('error', '')}"
    return True, ""


def check_records(cli, inp, exp):
    key, value = inp["key"], inp["value"]
    set_reply = cli.send({"method": "records.set", "params": {"key": key, "value": value}})
    if set_reply.get("ok") != exp["set_ok"]:
        return False, f"records.set ok={set_reply.get('ok')} expected {exp['set_ok']}"
    get_reply = cli.send({"method": "records.get", "params": {"key": key}})
    if get_reply.get("ok") != exp["get_ok"]:
        return False, f"records.get ok={get_reply.get('ok')} expected {exp['get_ok']}"
    if exp["get_ok"] and get_reply["result"] != exp["value"]:
        return False, f"records.get value {get_reply['result']!r} != {exp['value']!r}"
    return True, ""


def check_alerts(cli, inp, exp):
    reply = cli.send({"method": "alert.respond",
                      "params": {"id": inp["id"], "status": inp["status"], "owner": inp["owner"]}})
    if reply.get("ok") != exp["ok"]:
        return False, f"alert.respond ok={reply.get('ok')} expected {exp['ok']}: {reply.get('error', '')}"
    if exp["ok"]:
        rows = cli.send({"method": "alerts.list"})
        if not rows.get("ok"):
            return False, f"alerts.list error: {rows.get('error')}"
        match = [r for r in rows["result"] if r["id"] == inp["id"]]
        if not match or match[0]["status"] != inp["status"] or match[0]["owner"] != inp["owner"]:
            return False, "stored response does not match request"
    return True, ""


def main() -> int:
    binary = sys.argv[1]
    results_path = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "test_results.csv"
    db = str(Path(tempfile.mkdtemp(prefix="rigmind-cases-")) / "rigmind.db")
    cli = Cli(binary, db)

    summary = {}
    failures = 0
    started = time.time()
    with CASES.open(newline="", encoding="utf-8") as src, results_path.open("w", newline="", encoding="utf-8") as out:
        writer = csv.writer(out)
        writer.writerow(["case_id", "area", "status", "detail"])
        for row in csv.DictReader(src):
            area = row["area"]
            inp = json.loads(row["input_json"])
            exp = json.loads(row["expected_json"])
            try:
                if area.startswith("alarm_"):
                    passed, detail = check_alarm(cli, inp, exp)
                elif area == "bridge_validation":
                    passed, detail = check_bridge(cli, inp, exp)
                elif area == "records_roundtrip":
                    passed, detail = check_records(cli, inp, exp)
                elif area == "alerts_response":
                    passed, detail = check_alerts(cli, inp, exp)
                else:
                    passed, detail = False, f"no runner for area {area}"
            except Exception as exc:  # a crash in one case must not hide the rest
                passed, detail = False, f"exception: {exc}"
            status = "PASS" if passed else "FAIL"
            failures += 0 if passed else 1
            counts = summary.setdefault(area, [0, 0])
            counts[0 if passed else 1] += 1
            writer.writerow([row["case_id"], area, status, detail])

    cli.close()
    elapsed = time.time() - started
    for area, (passed, failed) in sorted(summary.items()):
        print(f"{area}: pass={passed} fail={failed}")
    total = sum(p + f for p, f in summary.values())
    print(f"total={total} failed={failures} elapsed={elapsed:.1f}s results={results_path}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
