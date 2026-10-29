"""RigMind Linux host: serves the SPA, runs the C++ backend, and relays live video commands.

Usage: python3 host.py <web_dir> <port> <bridge_cli> <db_path> [pipeline_exe]

- /bridge  POST: JSON request -> rigmind-bridge-cli (C++ store, alarms, records, stream validation)
- /events  GET : text/event-stream of unsolicited host events (stream answers, stream errors)
- /        GET : index.html with a window.chrome.webview shim injected, so the page takes its native path

Stream commands go CLI -> host -> rigmind-pipeline (GStreamer, WebRTC). Answers come back over /events.
Binds 127.0.0.1 only. Camera sources come from the RIGMIND_STREAMS env var (JSON: camera id -> rtsp:// URI).
"""

import http.server
import json
import os
import queue
import atexit
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slm_guard import first_sentence, reject_reason  # noqa: E402

WEB, PORT, CLI, DB = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
PIPELINE_EXE = sys.argv[5] if len(sys.argv) > 5 else None

SHIM = """<script>
(function () {
  var listeners = [];
  function deliver(data) { listeners.forEach(function (l) { l({ data: data }); }); }
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage: function (message) {
      var id = null;
      try { id = JSON.parse(message).id; } catch (e) { /* not JSON: reply carries no id */ }
      fetch('/bridge', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: message })
        .then(function (r) { return r.json(); })
        .then(deliver)
        .catch(function (e) { deliver({ id: id, ok: false, error: String(e) }); });
    },
    addEventListener: function (type, listener) { if (type === 'message') listeners.push(listener); }
  };
  var source = new EventSource('/events');
  source.onmessage = function (e) { deliver(JSON.parse(e.data)); };
})();
</script>"""

os.makedirs(os.path.dirname(os.path.abspath(DB)), exist_ok=True)

cli_lock = threading.Lock()
cli = subprocess.Popen([CLI, DB, "--stream"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                       text=True, bufsize=1)

pipeline = None
pipeline_lock = threading.Lock()
subscribers = []
subscribers_lock = threading.Lock()


def broadcast(event_json):
    """Sends one JSON event string to every connected /events client."""
    with subscribers_lock:
        for q in subscribers:
            q.put(event_json)


def record_clip(event):
    """A finished clip from the pipeline becomes a footage row (cam, time, count) in the backend."""
    try:
        bridge_call(json.dumps({"id": 0, "method": "footage.record",
                                "params": {"cam": event["cam"], "stamp": event["stamp"], "count": event["count"]}}))
    except (KeyError, RuntimeError) as e:
        print(f"footage record failed: {e}", file=sys.stderr)


# One alarm episode per camera: from the first alarm state until the camera returns to None.
# Every state change in the episode is one signal: one alert id, raised once, upgraded in place.
episodes = {}  # cam -> alert id; only read_pipeline's thread touches it


def raise_alarm(event):
    cam = event.get("cam", "")
    state = event.get("state")
    if state not in ("Yellow", "Orange", "Red"):
        episodes.pop(cam, None)  # episode over
        return
    alert_id = episodes.get(cam)
    if alert_id is None:
        alert_id = f"EV-{time.time_ns() // 1000}"[:32]
        episodes[cam] = alert_id
    try:
        bridge_call(json.dumps({"id": 0, "method": "alert.raise",
                                "params": {"id": alert_id, "severity": event["severity"],
                                           "condition": event["condition"] or state,
                                           "place": event["place"] or cam}}))
    except (KeyError, RuntimeError) as e:
        print(f"alert raise failed: {e}", file=sys.stderr)


def read_pipeline(proc):
    for line in proc.stdout:
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get("ev") == "clip":
            record_clip(event)
        elif event.get("ev") == "alarm":
            raise_alarm(event)
        broadcast(line)
    broadcast(json.dumps({"ev": "error", "message": "pipeline stopped"}))


def start_pipeline():
    global pipeline
    if not PIPELINE_EXE or not os.path.isfile(PIPELINE_EXE):
        print("rigmind-pipeline not found: live video disabled", file=sys.stderr)
        return
    # The pipeline keeps its segment cache and clips under the data folder (same folder as the database).
    # argv[2]: the EWS alarm plugin, which sits next to the pipeline in the bundle.
    plugin = os.path.join(os.path.dirname(PIPELINE_EXE), "librigmindews.so")
    pipeline = subprocess.Popen([PIPELINE_EXE, os.path.dirname(os.path.abspath(DB)), plugin],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, bufsize=1)
    threading.Thread(target=read_pipeline, args=(pipeline,), daemon=True).start()


# Plain-language description: BitNet b1.58 served by llama-server (ternary weights, CPU).
SLM_PORT = int(os.environ.get("RIGMIND_SLM_PORT", "8791"))
SLM_URL = f"http://127.0.0.1:{SLM_PORT}"
slm_process = None
SLM_SYSTEM = ("You write short plain-language safety summaries for rig supervisors. "
              "Reply with exactly one sentence and nothing else.")


def start_slm():
    global slm_process
    server, model = os.environ.get("RIGMIND_SLM_SERVER"), os.environ.get("RIGMIND_SLM_MODEL")
    if not server or not model or not os.path.isfile(server) or not os.path.isfile(model):
        print("SLM not configured: event descriptions use the template text", file=sys.stderr)
        return
    slm_process = subprocess.Popen([server, "-m", model, "--port", str(SLM_PORT), "-c", "512", "-t", "2"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    atexit.register(slm_process.terminate)


def describe_async(command):
    """Asks the SLM for a plain sentence. The answer (or a null text on failure) goes out as a description event."""
    def run():
        text = None
        try:
            body = json.dumps({"messages": [{"role": "system", "content": SLM_SYSTEM},
                                            {"role": "user", "content": command["prompt"]}],
                               "max_tokens": 80, "temperature": 0}).encode("utf-8")
            request = urllib.request.Request(SLM_URL + "/v1/chat/completions", data=body,
                                             headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=60) as response:
                content = json.load(response)["choices"][0]["message"]["content"]
            text = first_sentence(content)
            reason = reject_reason(text, command)
            if reason:
                # Model output that breaks the rules never reaches the page: the template text stands.
                print(f"SLM sentence rejected ({reason})", file=sys.stderr)
                text = None
        except (OSError, ValueError, KeyError) as e:
            print(f"SLM describe failed: {e}", file=sys.stderr)
            text = None
        broadcast(json.dumps({"ev": "description", "alertId": command["alertId"], "text": text,
                              "source": "slm" if text else "template"}))
    threading.Thread(target=run, daemon=True).start()


def send_to_pipeline(command):
    if command.get("cmd") == "describe":
        if slm_process is None:
            # No model running: the template text stands, so tell the page there is nothing to wait for.
            return broadcast(json.dumps({"ev": "description", "alertId": command["alertId"], "text": None,
                                         "source": "template"}))
        return describe_async(command)
    with pipeline_lock:
        if pipeline is None or pipeline.poll() is not None:
            broadcast(json.dumps({"ev": "error", "cam": command.get("cam"), "message": "pipeline not running"}))
            return
        pipeline.stdin.write(json.dumps(command) + "\n")
        pipeline.stdin.flush()


def bridge_call(line):
    """Sends one request to the backend and returns its reply. Forward lines are relayed, not returned."""
    with cli_lock:
        cli.stdin.write(line + "\n")
        cli.stdin.flush()
        while True:
            reply = cli.stdout.readline()
            if not reply:
                raise RuntimeError("bridge backend exited")
            message = json.loads(reply)
            if "forward" in message:
                send_to_pipeline(message["forward"])
                continue
            return reply.strip()


def seed_stream_sources():
    """Writes RIGMIND_STREAMS into the records store, where the Live page reads it."""
    raw = os.environ.get("RIGMIND_STREAMS")
    if not raw:
        return
    sources = json.loads(raw)
    bridge_call(json.dumps({"id": 0, "method": "records.set",
                            "params": {"key": "stream.sources", "value": sources}}))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB, **kwargs)

    def log_message(self, *args):
        pass

    def _send(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _index(self):
        with open(os.path.join(WEB, "index.html"), "r", encoding="utf-8") as f:
            html = f.read().replace("<head>", "<head>" + SHIM, 1)
        self._send(200, html.encode("utf-8"), "text/html; charset=utf-8")

    def _events(self):
        q = queue.Queue()
        with subscribers_lock:
            subscribers.append(q)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            while True:
                try:
                    payload = f"data: {q.get(timeout=15)}\n\n"
                except queue.Empty:
                    payload = ": ping\n\n"
                self.wfile.write(payload.encode("utf-8"))
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with subscribers_lock:
                subscribers.remove(q)

    def do_GET(self):
        path = self.path.split("?")[0].split("#")[0]
        if path == "/events":
            return self._events()
        if path in ("/", "/index.html") or not os.path.isfile(os.path.join(WEB, path.lstrip("/"))):
            return self._index()
        return super().do_GET()

    def do_POST(self):
        if self.path != "/bridge":
            return self._send(404, b"not found", "text/plain")
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf-8")
        try:
            reply = bridge_call(body)
        except RuntimeError as e:
            try:
                request_id = json.loads(body).get("id")
            except ValueError:
                request_id = None
            reply = json.dumps({"id": request_id, "ok": False, "error": str(e)})
        self._send(200, reply.encode("utf-8"), "application/json")


start_pipeline()
start_slm()
seed_stream_sources()
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
