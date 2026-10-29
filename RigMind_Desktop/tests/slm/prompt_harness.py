"""Prompt harness for the BitNet plain-language module.

Drives the real prompt path: rigmind-bridge-cli --stream builds the prompt in C++ (event.describe),
the harness takes the forwarded prompt, sends it to a running llama-server, and checks the answer.

Usage (venv not needed: stdlib only):
    python3 tests/slm/prompt_harness.py <rigmind-bridge-cli> <llama-server-url> [cases.json] [results.csv]

Outcome per case:
  PASS      the model sentence passes scripts/slm_guard.py (one sentence, severity meaning, camera, place,
            no prompt leakage) and would be shown on the page
  FALLBACK  the guard rejects it; the page keeps the template text (reason recorded)
  FAIL      harness or server error
Exit code 0 unless a case FAILs. The acceptance rate is the measure of prompt quality.
"""

import csv
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent.parent / "scripts"))
from slm_guard import first_sentence, reject_reason  # the same rules the host applies


def bridge_forward(cli, event):
    """Runs event.describe through the real CLI and returns the prompt it forwards to the SLM."""
    request = {"id": 1, "method": "event.describe", "params": event}
    proc = subprocess.run([cli, "/tmp/slm-harness-" + str(event["alertId"]) + ".db", "--stream"],
                          input=json.dumps(request) + "\n", capture_output=True, text=True, timeout=30)
    for line in proc.stdout.splitlines():
        message = json.loads(line)
        if "forward" in message and message["forward"].get("cmd") == "describe":
            return message["forward"]["prompt"], message["forward"]["alertId"]
    raise RuntimeError("no describe forward from bridge: " + proc.stdout[:200] + proc.stderr[:200])


SLM_SYSTEM = ("You write short plain-language safety summaries for rig supervisors. "
              "Reply with exactly one sentence and nothing else.")


def ask_slm(server, prompt):
    """Same chat call the host makes (scripts/host.py describe_async)."""
    body = json.dumps({"messages": [{"role": "system", "content": SLM_SYSTEM},
                                    {"role": "user", "content": prompt}],
                       "max_tokens": 80, "temperature": 0}).encode("utf-8")
    request = urllib.request.Request(server + "/v1/chat/completions", data=body,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=90) as response:
        content = json.load(response)["choices"][0]["message"]["content"]
    lines = [line.strip() for line in content.splitlines() if line.strip()]
    return lines[0] if lines else ""


def outcome(case, raw):
    """What the page would get: the first sentence if the guard accepts it, else the template (FALLBACK)."""
    sentence = first_sentence(raw)
    reason = reject_reason(sentence, case["event"])
    return ("PASS", sentence, "") if reason is None else ("FALLBACK", sentence, reason)


def main() -> int:
    cli, server = sys.argv[1], sys.argv[2].rstrip("/")
    cases_path = Path(sys.argv[3]) if len(sys.argv) > 3 else HERE / "cases.json"
    results_path = Path(sys.argv[4]) if len(sys.argv) > 4 else HERE / "results.csv"
    cases = json.loads(cases_path.read_text(encoding="utf-8"))

    failures = 0
    accepted = 0
    with results_path.open("w", newline="", encoding="utf-8") as out:
        writer = csv.writer(out)
        writer.writerow(["case", "status", "sentence", "reason"])
        for case in cases:
            try:
                prompt, _ = bridge_forward(cli, case["event"])
                raw = ask_slm(server, prompt)
                status, sentence, reason = outcome(case, raw)
            except Exception as exc:  # a broken case must not hide the rest
                status, sentence, reason = "FAIL", "", f"exception: {exc}"
            failures += 1 if status == "FAIL" else 0
            accepted += 1 if status == "PASS" else 0
            writer.writerow([case["name"], status, sentence, reason])
            print(f"{status} {case['name']}" + (f": {reason}" if reason else ""))

    print(f"cases={len(cases)} accepted={accepted} fallback={len(cases) - accepted - failures} failed={failures} results={results_path}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
