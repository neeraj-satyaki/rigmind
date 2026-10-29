"""Inference settings (confidence and IoU thresholds, image size, ...) for the detection models.

The editable source is models/inference.json. The file the app reads is models/inference.cfg: the same JSON,
base64-encoded, behind a one-line header. Base64 is an encoding, not encryption: it stops casual edits, not a
determined reader.

    python scripts/inference_cfg.py encode      inference.json -> inference.cfg
    python scripts/inference_cfg.py decode      print the settings held in inference.cfg
    python scripts/inference_cfg.py check       validate inference.json and that inference.cfg matches it

As a module:  from inference_cfg import load;  settings = load("ppe")  ->  {"conf": 0.25, "iou": 0.45, "imgsz": 640, ...}
"""
import base64
import json
import sys
from pathlib import Path

MODELS = Path(__file__).resolve().parent.parent / "models"
SOURCE = MODELS / "inference.json"
ENCODED = MODELS / "inference.cfg"
HEADER = "RIGMIND-INFERENCE-CFG 1"


def _validate(cfg: dict) -> None:
    if cfg.get("version") != 1:
        raise ValueError("version must be 1")
    for name, model in {**{"defaults": cfg.get("defaults", {})}, **cfg.get("models", {})}.items():
        for key, low, high in (("conf", 0.0, 1.0), ("iou", 0.0, 1.0)):
            if key in model and not (isinstance(model[key], (int, float)) and low <= model[key] <= high):
                raise ValueError(f"{name}.{key} must be a number between {low} and {high}")
        if "imgsz" in model and not (isinstance(model["imgsz"], int) and 32 <= model["imgsz"] <= 2048 and model["imgsz"] % 32 == 0):
            raise ValueError(f"{name}.imgsz must be a multiple of 32 between 32 and 2048")
        if "max_det" in model and not (isinstance(model["max_det"], int) and 1 <= model["max_det"] <= 3000):
            raise ValueError(f"{name}.max_det must be an integer between 1 and 3000")
    if not cfg.get("models"):
        raise ValueError("no models listed")
    roi = cfg.get("roi")
    if roi is not None:
        if not isinstance(roi.get("enabled"), bool):
            raise ValueError("roi.enabled must be true or false")
        if not (isinstance(roi.get("hold_frames"), int) and 0 <= roi["hold_frames"] <= 600):
            raise ValueError("roi.hold_frames must be an integer between 0 and 600")
        if not (isinstance(roi.get("reference_class"), str) and roi["reference_class"]):
            raise ValueError("roi.reference_class must be a class name")
    stream = cfg.get("stream")
    if stream is not None and not (isinstance(stream.get("output_fps"), (int, float)) and 1 <= stream["output_fps"] <= 60):
        raise ValueError("stream.output_fps must be a number between 1 and 60")
    auto = cfg.get("auto_scale")
    if auto is not None:
        if not isinstance(auto.get("enabled"), bool):
            raise ValueError("auto_scale.enabled must be true or false")
        for key in ("wide", "zoomed"):
            if not (isinstance(auto.get(key), (int, float)) and 1.0 <= auto[key] <= 3.0):
                raise ValueError(f"auto_scale.{key} must be a number between 1.0 and 3.0")
        if auto.get("force") not in (None, "wide", "zoomed"):
            raise ValueError("auto_scale.force must be null, \"wide\" or \"zoomed\"")
        if not auto["wide"] <= auto["zoomed"]:
            raise ValueError("auto_scale.wide must not exceed auto_scale.zoomed")
        t = auto.get("zoomed_if_platform_width_fraction_at_least")
        if not (isinstance(t, (int, float)) and 0.05 <= t <= 0.95):
            raise ValueError("auto_scale.zoomed_if_platform_width_fraction_at_least must be between 0.05 and 0.95")
        if not (isinstance(auto.get("hysteresis"), (int, float)) and 0 <= auto["hysteresis"] < t):
            raise ValueError("auto_scale.hysteresis must be >= 0 and below the threshold")
        if not (isinstance(auto.get("median_frames"), int) and 1 <= auto["median_frames"] <= 60):
            raise ValueError("auto_scale.median_frames must be an integer between 1 and 60")
    tr = cfg.get("tracking")
    if tr is not None:
        if not isinstance(tr.get("enabled"), bool):
            raise ValueError("tracking.enabled must be true or false")
        if tr.get("algorithm", "bytetrack") != "bytetrack":
            raise ValueError("tracking.algorithm must be \"bytetrack\"")
        for key in ("high_thresh", "low_thresh", "new_track_thresh", "match_thresh"):
            if not (isinstance(tr.get(key), (int, float)) and 0.0 <= tr[key] <= 1.0):
                raise ValueError(f"tracking.{key} must be a number between 0 and 1")
        if not tr["low_thresh"] <= tr["high_thresh"]:
            raise ValueError("tracking.low_thresh must not exceed tracking.high_thresh")
        if not (isinstance(tr.get("track_buffer_s"), (int, float)) and 0 <= tr["track_buffer_s"] <= 10):
            raise ValueError("tracking.track_buffer_s must be a number between 0 and 10")
        if not (isinstance(tr.get("min_hits"), int) and 1 <= tr["min_hits"] <= 10):
            raise ValueError("tracking.min_hits must be an integer between 1 and 10")
    ppe = cfg.get("ppe_check")
    if ppe is not None:
        if not isinstance(ppe.get("enabled"), bool):
            raise ValueError("ppe_check.enabled must be true or false")
        req = ppe.get("required")
        if not (isinstance(req, list) and req and all(isinstance(r, str) and r for r in req)):
            raise ValueError("ppe_check.required must be a non-empty list of class names")
        if not (isinstance(ppe.get("person_class"), str) and ppe["person_class"]):
            raise ValueError("ppe_check.person_class must be a class name")
        if not (isinstance(ppe.get("person_box_pad"), (int, float)) and 0 <= ppe["person_box_pad"] <= 1):
            raise ValueError("ppe_check.person_box_pad must be a number between 0 and 1")
        for key in ("missing_seconds", "clear_seconds"):
            if not (isinstance(ppe.get(key), (int, float)) and 0 <= ppe[key] <= 600):
                raise ValueError(f"ppe_check.{key} must be a number between 0 and 600")


def encode(source: Path = SOURCE, target: Path = ENCODED) -> None:
    cfg = json.loads(source.read_text(encoding="utf-8"))
    _validate(cfg)
    body = base64.b64encode(json.dumps(cfg, separators=(",", ":"), sort_keys=True).encode("utf-8")).decode("ascii")
    lines = [body[i:i + 76] for i in range(0, len(body), 76)]
    target.write_text("\n".join([HEADER, *lines]) + "\n", encoding="ascii")


def decode(path: Path = ENCODED) -> dict:
    text = path.read_text(encoding="ascii").splitlines()
    if not text or text[0].strip() != HEADER:
        raise ValueError(f"{path.name}: not a RigMind inference settings file")
    cfg = json.loads(base64.b64decode("".join(text[1:])).decode("utf-8"))
    _validate(cfg)
    return cfg


def load(model: str, path: Path = ENCODED) -> dict:
    """Settings for one model: the defaults overlaid with that model's own values."""
    cfg = decode(path)
    if model not in cfg["models"]:
        raise KeyError(f"no settings for model '{model}'")
    return {**cfg["defaults"], **cfg["models"][model]}


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else "check"
    if command == "encode":
        encode()
        print(f"wrote {ENCODED}")
    elif command == "decode":
        print(json.dumps(decode(), indent=2))
    elif command == "check":
        source = json.loads(SOURCE.read_text(encoding="utf-8"))
        _validate(source)
        if not ENCODED.exists() or decode() != source:
            print("inference.cfg is missing or differs from inference.json: run 'encode'")
            return 1
        print("ok: inference.json is valid and inference.cfg matches it")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
