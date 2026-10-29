"""Stand-in for llama-server: the HTTP subset the RigMind host uses, with a canned one-sentence answer.

It validates wiring (prompt -> HTTP -> guard -> description event), NOT model quality.

    python stub_llama_server.py --port 8791 --mode ok

Modes:
  ok       one sentence built from the facts in the prompt (names camera and place, passes the guard)
  leak     a sentence that repeats the prompt ("Event: ..."): the guard must reject it
  wrong    a sentence naming another camera: the guard must reject it
  loading  HTTP 503 with llama-server's "Loading model" error body
  garbage  HTTP 200 with a body that is not JSON
Also answers GET /health like llama-server.
"""

import argparse
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FACTS = re.compile(r"camera (?P<cam>\S+?)(?: at (?P<place>.*?))? recorded: (?P<cond>.*?)\. This (?P<sev>\w+) alert (?P<meaning>[^.]*)\.")


def canned_sentence(prompt, mode):
    m = FACTS.search(prompt)
    if not m:
        return "Something happened on site and a supervisor should check it."
    cam, place, cond = m["cam"], m["place"] or "the site", m["cond"].lower()
    if mode == "leak":
        return "Event: " + prompt.splitlines()[0]
    if mode == "wrong":
        return f"Camera CAM-99 saw {cond} somewhere else, and this alert {m['meaning']}."
    return f"Camera {cam} at {place} saw {cond}, so this {m['sev'].lower()} alert {m['meaning']}."


def make_handler(mode):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, status, body, content_type="application/json"):
            data = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/health":
                return self._send(503 if mode == "loading" else 200, json.dumps({"status": "ok"}))
            self._send(404, "{}")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length) or b"{}")
            if self.path != "/v1/chat/completions":
                return self._send(404, "{}")
            if mode == "loading":
                return self._send(503, json.dumps({"error": {"code": 503, "message": "Loading model"}}))
            if mode == "garbage":
                return self._send(200, "not json at all", "text/plain")
            prompt = request["messages"][-1]["content"]
            reply = {"choices": [{"index": 0, "message": {"role": "assistant", "content": canned_sentence(prompt, mode)}}]}
            self._send(200, json.dumps(reply))

    return Handler


def serve(port, mode):
    return ThreadingHTTPServer(("127.0.0.1", port), make_handler(mode))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--mode", default="ok", choices=["ok", "leak", "wrong", "loading", "garbage"])
    args, _ = ap.parse_known_args()  # llama-server flags (-m, -c, -t) are accepted and ignored
    print(f"stub llama-server on 127.0.0.1:{args.port} mode={args.mode}", file=sys.stderr)
    serve(args.port, args.mode).serve_forever()
