"""Summarise a run_pipeline.py jsonl: PPE box flicker, track ids, 'ppe' and 'ppe_status' events, alarms.
usage: python ppe_transitions.py <run.jsonl>
Transitions = a class going from present (at least one box in the PPE detections event) to absent or back, per minute."""
import collections, json, sys

ev = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8") if l.startswith("{")]
ppe = [e for e in ev if e.get("ev") == "detections" and e.get("model") == "ppe"]
if not ppe:
    sys.exit("no ppe detections")
span = (ppe[-1]["ts"] - ppe[0]["ts"]) / 60000.0
print(f"ppe frames {len(ppe)} over {span*60:.0f} s ({len(ppe)/max(span*60,1):.1f}/s)")
classes = sorted({i["label"].lower() for e in ppe for i in e["items"]})
for c in classes:
    prev = None; trans = 0; present = 0; held = 0; ids = set(); boxes = 0
    for e in ppe:
        its = [i for i in e["items"] if i["label"].lower() == c]
        now = bool(its)
        present += now
        boxes += len(its)
        held += sum(1 for i in its if i.get("held"))
        ids.update(i["track_id"] for i in its if "track_id" in i)
        if prev is not None and now != prev:
            trans += 1
        prev = now
    print(f"{c:12} frames with box {100*present/len(ppe):5.1f}%  transitions/min {trans/max(span,1e-9):6.1f}  "
          f"held boxes {held}/{boxes}  distinct track ids {len(ids) if ids else '-'}")
for e in ev:
    if e.get("ev") == "ppe":
        print("PPE", json.dumps(e))
st = [e for e in ev if e.get("ev") == "ppe_status"]
for e in st[-3:]:
    print("STATUS", json.dumps(e))
if st:
    print("track_us_avg over windows:", [round(e["track_us_avg"], 1) for e in st])
al = [e for e in ev if e.get("ev") == "alarm"]
print("alarms:", collections.Counter(e["state"] for e in al))
for e in al:
    print("ALARM", e["state"], e.get("condition", ""))
inf = [e for e in ev if e.get("ev") == "inference"]
for m in ("ppe", "idss"):
    r = [e for e in inf if e.get("model") == m]
    if r:
        print(m, "fps", round(sum(e["fps"] for e in r) / len(r), 1), "avg_ms", round(sum(e["avg_ms"] for e in r) / len(r), 1))
