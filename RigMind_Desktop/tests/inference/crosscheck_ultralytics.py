"""Compares rigmind-pipeline detections with python ultralytics on the same frames.

    python crosscheck_ultralytics.py <events.jsonl from pipeline_inference_test.py --dump> <frames dir (RIGMIND_DUMP_FRAMES)>

Frames are the PPM files the pipeline saved for the frames it ran (name carries the event ts). Each is run through
ultralytics (ONNX model, same conf/iou/imgsz as inference.cfg) and boxes are matched greedily at IoU >= 0.5, same class.
"""
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import inference_cfg  # noqa: E402
from ultralytics import YOLO  # noqa: E402


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(b"\n", 1)[0].split()
    w, h = int(parts[1]), int(parts[2])
    off = len(data.split(b"\n", 1)[0]) + 1
    return np.frombuffer(data, np.uint8, offset=off).reshape(h, w, 3)  # RGB


def iou(a, b):
    ix = max(0, min(a[2], b[2]) - max(a[0], b[0])); iy = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    return inter / ((a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter + 1e-9)


def main():
    events = [json.loads(l) for l in Path(sys.argv[1]).read_text().splitlines() if l.strip()]
    frames = Path(sys.argv[2])
    total = {"ppe": [0, 0, 0], "idss": [0, 0, 0]}  # matched, mine only, ultralytics only
    for model, onnx in (("ppe", "ppe.onnx"), ("idss", "idss_model.onnx")):
        s = inference_cfg.load(model)
        yolo = YOLO(str(ROOT / "models" / onnx), task=s["task"] if "task" in s else None)
        for ppm in sorted(frames.glob(f"frame_{model}_*.ppm")):
            ts = int(ppm.stem.split("_")[-1])
            mine = next((e for e in events if e.get("ev") == "detections" and e["model"] == model and e["ts"] == ts), None)
            if not mine:
                continue
            img = read_ppm(ppm)[:, :, ::-1]  # ultralytics takes BGR numpy
            r = yolo.predict(img, conf=s["conf"], iou=s["iou"], imgsz=s["imgsz"], max_det=s["max_det"], verbose=False, device="cpu")[0]
            ref = [(b.xyxy[0].tolist(), int(b.cls), float(b.conf)) for b in r.boxes]
            used, matched = set(), 0
            for it in mine["items"]:
                best, bi = 0, None
                for i, (box, c, conf) in enumerate(ref):
                    if i in used or c != it["cls"]:
                        continue
                    v = iou(it["box"], box)
                    if v > best:
                        best, bi = v, i
                if bi is not None and best >= 0.5:
                    used.add(bi); matched += 1
                    print(f"  [{model}] {it['label']:<11} conf mine={it['conf']:.3f} ref={ref[bi][2]:.3f} iou={best:.3f}")
            mo, uo = len(mine["items"]) - matched, len(ref) - matched
            print(f"[{model}] frame {ts}: mine={len(mine['items'])} ultralytics={len(ref)} matched={matched}")
            for k, v in enumerate((matched, mo, uo)):
                total[model][k] += v
    for m, (a, b, c) in total.items():
        print(f"TOTAL {m}: matched={a} only-mine={b} only-ultralytics={c}")


main()
