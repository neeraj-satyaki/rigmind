"""Run rigmind-pipeline.exe directly on a camera and log every stdout JSON line to a file for N seconds.
usage: python run_pipeline.py <seconds> <out.jsonl> [pipeline_dir] [uri] [extra env KEY=VAL ...]
Prints a summary: event counts, 'facts' % table (ROI on / off), angle distribution and every alarm event."""
import json, os, subprocess, sys, threading, time, statistics, collections
secs = int(sys.argv[1]); out = sys.argv[2]
d = sys.argv[3] if len(sys.argv) > 3 else r"D:\Projects\rigmind\RigMind_Desktop\build-app\Release"
uri = sys.argv[4] if len(sys.argv) > 4 else "rtsp://192.168.1.8:8556/feed0"
env = dict(os.environ)
env["PATH"] = r"C:\gstreamer\1.0\msvc_x86_64\bin;" + env["PATH"]
env.setdefault("RIGMIND_GPU_DIR", r"D:\Projects\rigmind\RigMind_Desktop\runtime-windows\gpu")
for kv in sys.argv[5:]:
    k, v = kv.split("=", 1); env[k] = v
p = subprocess.Popen([os.path.join(d, "rigmind-pipeline.exe"), os.path.join(os.environ["TEMP"], "rmdata"),
                      os.path.join(d, "librigmindews.dll"), os.path.join(d, "models")],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(out + ".err", "w"), env=env, text=True)
p.stdin.write(json.dumps({"cmd": "open", "cam": "CAM-01", "uri": uri}) + "\n"); p.stdin.flush()
f = open(out, "w", encoding="utf-8")
def pump():
    for line in p.stdout:
        f.write(line); f.flush()
threading.Thread(target=pump, daemon=True).start()
time.sleep(secs); p.kill(); f.close()
ev = [json.loads(l) for l in open(out, encoding="utf-8") if l.startswith("{")]
print(collections.Counter(e.get("ev") for e in ev))
for e in ev:
    if e.get("ev") in ("error", "alarm"): print(json.dumps(e)[:300])
fs = [e for e in ev if e.get("ev") == "facts"]; n = sum(e["frames"] for e in fs)
print("idss frames", n, "windows", len(fs))
if fs:
    print(f"{'fact':32}{'fed to EWS':>11}{'no ROI':>8}{'ROI on all':>11}")
    for k in fs[0]["pct"]:
        on = sum(e["pct"][k] * e["frames"] for e in fs) / n; off = sum(e["pct_no_roi"][k] * e["frames"] for e in fs) / n
        old = sum(e["pct_old_roi"][k] * e["frames"] for e in fs) / n
        print(f"{k:32}{on:11.1f}{off:8.1f}{old:11.1f}")
    a = sorted(x for e in fs for x in e["angles"])
    if a: print("angle_deg n=%d min %.1f median %.1f max %.1f" % (len(a), a[0], statistics.median(a), a[-1]))
