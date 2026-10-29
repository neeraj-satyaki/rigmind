"""Wiring test for the plain-language path: bridge CLI -> describe command -> HTTP model -> guard -> description event.

Uses stub_llama_server.py, so this proves the wiring and the fallbacks, not the quality of the real BitNet model.
The host steps (chat call, first_sentence, reject_reason, description event) mirror scripts/host.py describe_async
and src/slm/slm_client.cpp, with the same guard rules as tests/slm/prompt_harness.py.

    python tests/slm/wiring_test.py <path-to-rigmind-bridge-cli>

Exit code 0 when every scenario passes.
"""

import json
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.request
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent.parent / "scripts"))
sys.path.insert(0, str(HERE))
from slm_guard import first_sentence, reject_reason  # noqa: E402
from stub_llama_server import serve  # noqa: E402

CLI = str(Path(sys.argv[1] if len(sys.argv) > 1 else HERE.parent.parent / "build" / "Release" / "rigmind-bridge-cli.exe").resolve())
SLM_SYSTEM = ("You write short plain-language safety summaries for rig supervisors. "
              "Reply with exactly one sentence and nothing else.")
EVENT = {"alertId": "EV-2001", "severity": "Critical", "condition": "Platform-elevator collision",
         "place": "Demo rig 01 · Derrick board", "camId": "CAM-01", "tsMs": 1793203115196}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def host_describe(command, port):
    """What the host does with a describe command. Returns the description event."""
    text = None
    try:
        body = json.dumps({"messages": [{"role": "system", "content": SLM_SYSTEM},
                                        {"role": "user", "content": command["prompt"]}],
                           "max_tokens": 80, "temperature": 0}).encode("utf-8")
        req = urllib.request.Request(f"http://127.0.0.1:{port}/v1/chat/completions", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            content = json.load(resp)["choices"][0]["message"]["content"]
        text = first_sentence(content)
        if reject_reason(text, command):
            text = None
    except (OSError, ValueError, KeyError) as exc:
        getattr(exc, "close", lambda: None)()  # an HTTPError holds the response open
        text = None
    return {"ev": "description", "alertId": command["alertId"], "text": text, "source": "slm" if text else "template"}


class Session:
    """One rigmind-bridge-cli --stream process."""

    def __init__(self, workdir):
        self.db = str(Path(workdir) / "rigmind.db")
        self.requests = []

    def run(self, requests):
        stdin = "".join(json.dumps(r) + "\n" for r in requests)
        proc = subprocess.run([CLI, self.db, "--stream"], input=stdin, capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0, proc.stderr
        replies, forwards = [], []
        for line in proc.stdout.splitlines():
            message = json.loads(line)
            (forwards if "forward" in message else replies).append(message.get("forward", message))
        return replies, forwards

    def audit_rows(self):
        log = Path(self.db).parent / "audit.log"
        return log.read_text(encoding="utf-8").splitlines() if log.exists() else []


class WiringTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.session = Session(self.tmp.name)
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            server.shutdown()
            server.server_close()

    def stub(self, mode):
        server = serve(0, mode)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.servers.append(server)
        return server.server_address[1]

    def describe(self, port):
        replies, forwards = self.session.run([
            {"id": 1, "method": "event.describe", "params": EVENT},
            {"id": 2, "method": "app.version"},  # the backend must still answer after a describe
        ])
        self.assertEqual(replies[0]["ok"], True, replies)
        self.assertEqual(replies[1]["result"]["name"], "RigMind")
        self.assertEqual(len(forwards), 1)
        self.assertEqual(forwards[0]["cmd"], "describe")
        self.assertEqual(forwards[0]["alertId"], EVENT["alertId"])
        return replies[0]["result"], host_describe(forwards[0], port)

    def test_slm_up_gives_slm_sentence_that_passes_guard(self):
        result, event = self.describe(self.stub("ok"))
        self.assertTrue(result["slm"])
        self.assertIn("CAM-01", result["text"])  # template text is returned at once
        self.assertEqual(event["source"], "slm")
        self.assertIsNone(reject_reason(event["text"], EVENT))
        self.assertIn("CAM-01", event["text"])
        self.assertIn("Derrick board", event["text"])
        details = [row for row in self.session.audit_rows() if "event.describe" in row]
        self.assertEqual(len(details), 1)
        self.assertIn("slm", details[0])

    def test_slm_down_falls_back_to_template(self):
        result, event = self.describe(free_port())  # nothing listens there
        self.assertEqual(event["source"], "template")
        self.assertIsNone(event["text"])
        self.assertIn("This Critical alert needs immediate action.", result["text"])

    def test_guard_rejected_text_falls_back_to_template(self):
        for mode in ("leak", "wrong"):
            with self.subTest(mode=mode):
                _, event = self.describe(self.stub(mode))
                self.assertEqual(event["source"], "template")
                self.assertIsNone(event["text"])

    def test_model_loading_or_garbage_falls_back_to_template(self):
        for mode in ("loading", "garbage"):
            with self.subTest(mode=mode):
                _, event = self.describe(self.stub(mode))
                self.assertEqual(event["source"], "template")
                self.assertIsNone(event["text"])

    def test_without_stream_sink_bridge_reports_template(self):
        proc = subprocess.run([CLI, self.session.db], input=json.dumps({"id": 1, "method": "event.describe", "params": EVENT}) + "\n",
                              capture_output=True, text=True, timeout=30)
        reply = json.loads(proc.stdout.splitlines()[0])
        self.assertFalse(reply["result"]["slm"])
        rows = [row for row in self.session.audit_rows() if "event.describe" in row]
        self.assertIn("template", rows[0])

    def test_bad_event_is_rejected_not_crashed(self):
        replies, forwards = self.session.run([{"id": 1, "method": "event.describe", "params": {"alertId": "EV-1"}},
                                              {"id": 2, "method": "app.version"}])
        self.assertFalse(replies[0]["ok"])
        self.assertEqual(forwards, [])
        self.assertTrue(replies[1]["ok"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
