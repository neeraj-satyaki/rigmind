"""Generates tests/cases/test_cases.csv: ~15k bridge-level test cases with expected results.

Expected values come from an independent Python spec, not from the C++ code:
  - alarm: Python re-implementation of IDSS src/safety/ews.cpp IDSS::algorithm() counter rules
  - bridge: request validation rules (required / type / length / enum / stream unavailable)
  - records and alerts: storage contract (keys, values, status enum, owner length)

Run:  python3 tests/cases/generate_cases.py
Deterministic (fixed seed). Self-checks the oracle against values derived by hand from ews.cpp.
"""

import csv
import sys
import itertools
import json
import random
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import more_cases  # noqa: E402  (newer host features: sign-in, evidence search, chat sessions)

OUT = Path(__file__).with_name("test_cases.csv")
SEED = 42
FACTS = [
    "machinePresent", "linkTiltPresent", "linkTiltAngled", "elevatorPresent",
    "elevatorNearPlatform", "platformElevatorCollision", "derrickmanOutOfSafeZone",
    "machinePastRightEdge", "linkTiltNearDerrickman",
]


# ---------- alarm oracle (ews.cpp:418-517) ----------

class AlarmOracle:
    def __init__(self, zoom=False):
        self.yellow_thr, self.orange_thr, self.red_thr = (30, 100, 200) if zoom else (4, 20, 200)
        self.y = self.o = self.r = 0
        self.state = "NONE"

    def step(self, f):
        if f["machinePresent"]:
            if f["linkTiltAngled"]:
                if self.state != "YELLOW":
                    self.y += 1
                    if self.y > self.yellow_thr:
                        self.state = "YELLOW"
                if f["elevatorNearPlatform"]:
                    if self.state != "ORANGE":
                        self.o += 1
                        if self.o > self.orange_thr:
                            self.state = "ORANGE"
                            self.y = 0
                    if f["elevatorPresent"]:
                        if f["platformElevatorCollision"] or f["derrickmanOutOfSafeZone"] or f["machinePastRightEdge"]:
                            if self.state != "RED":
                                self.r += 1
                                if self.r > self.red_thr:
                                    self.state = "RED"
                                    self.o = 0
                                    self.y = 0
                        else:
                            self.r -= 1
                            if self.r > self.red_thr:
                                self.state = "RED"
                else:
                    self.o -= 1
        else:
            if f["linkTiltPresent"] and f["linkTiltNearDerrickman"]:
                self.state = "YELLOW"
            self.y = self.o = self.r = 0
        return self.state


def run_frames(zoom, frames):
    oracle = AlarmOracle(zoom)
    return [oracle.step(f) for f in frames]


def frame(**kw):
    return {k: bool(kw.get(k, False)) for k in FACTS}


# ---------- bridge request rules ----------

# field -> (json type, required, max_bytes or None, enum or None)
RULES = {
    "alarm.step": {k: ("bool", False, None, None) for k in FACTS},
    "alarm.reset": {"zoom": ("bool", False, None, None)},
    "events.recent": {"limit": ("int", False, None, None)},
    "alert.raise": {
        "id": ("str", True, 32, None), "severity": ("str", True, 16, None),
        "condition": ("str", True, 128, None), "place": ("str", True, 128, None),
    },
    "alert.respond": {
        "id": ("str", True, 32, None),
        "status": ("str", True, 16, ["Open", "Acknowledged", "Escalated"]),
        "owner": ("str", True, 64, None),
    },
    "alerts.list": {},
    "records.get": {"key": ("str", True, 256, None)},
    "records.set": {"key": ("str", True, 256, None), "value": ("any", True, None, None)},
    "records.list": {"prefix": ("str", False, None, None)},
    "stream.open": {"cam": ("str", True, 32, None), "uri": ("str", True, 512, None)},
    "stream.offer": {"cam": ("str", True, 32, None), "sdp": ("str", True, 16384, None)},
    "stream.close": {"cam": ("str", True, 32, None)},
    "app.version": {},
}
METHODS = list(RULES) + ["bogus.method"]
STREAM_METHODS = {"stream.open", "stream.offer", "stream.close"}  # no sink in the CLI: always unavailable


def type_ok(value, kind):
    if kind == "any":
        return True
    if kind == "bool":
        return isinstance(value, bool)
    if kind == "int":
        return isinstance(value, int) and not isinstance(value, bool)
    return isinstance(value, str)


def expected_ok(method, params):
    """Spec for whether the bridge accepts a request. Params is the dict as sent."""
    if method == "bogus.method":
        return False
    if method == "app.version":
        return True
    if method in STREAM_METHODS:
        return False
    if params is None:
        params = {}
    if not isinstance(params, dict):
        return False
    for field, (kind, required, max_bytes, enum) in RULES[method].items():
        if field not in params:
            if required:
                return False
            continue
        value = params[field]
        if not type_ok(value, kind):
            return False
        if kind == "str":
            if required and len(value.encode("utf-8")) == 0:  # empty is valid only for optional fields (e.g. list-all prefix)
                return False
            if max_bytes and len(value.encode("utf-8")) > max_bytes:
                return False
            if enum and value not in enum:
                return False
        if method == "records.set" and field == "key":
            pass
    return True


def base_params(method, rng):
    if method == "alarm.step":
        return {k: rng.random() < 0.5 for k in FACTS}
    if method == "alarm.reset":
        return {"zoom": rng.random() < 0.5}
    if method == "events.recent":
        return {"limit": rng.randint(1, 500)}
    if method == "alert.raise":
        return {"id": f"EV-{rng.randint(1000, 999999)}", "severity": rng.choice(["Critical", "Warning", "Advisory"]),
                "condition": "Occupied movement zone", "place": "Demo rig 01"}
    if method == "alert.respond":
        return {"id": f"EV-{rng.randint(1000, 999999)}", "status": rng.choice(["Acknowledged", "Escalated", "Open"]),
                "owner": rng.choice(["Faisal A.", "Nasser H.", "HSE desk"])}
    if method == "records.get":
        return {"key": f"pref-{rng.randint(0, 10**6)}"}
    if method == "records.set":
        return {"key": f"rec-{rng.randint(0, 10**6)}", "value": {"n": rng.randint(0, 9)}}
    if method == "records.list":
        return {"prefix": "rec"}
    if method == "stream.open":
        return {"cam": "CAM-01", "uri": "rtsp://cam.local:554/live"}
    if method == "stream.offer":
        return {"cam": "CAM-01", "sdp": "v=0"}
    if method == "stream.close":
        return {"cam": "CAM-01"}
    return {}


MUTATIONS = ["valid", "missing", "wrong_type", "empty", "too_long", "null", "extra_field"]


def mutate(params, field, mutation, rng):
    p = json.loads(json.dumps(params))
    if mutation == "valid":
        return p
    if mutation == "extra_field":
        p["unexpected_" + str(rng.randint(0, 99))] = "x"
        return p
    if field is None:
        return p
    if mutation == "missing":
        p.pop(field, None)
    elif mutation == "wrong_type":
        p[field] = 12345 if isinstance(p.get(field), str) else "not-a-bool"
    elif mutation == "empty":
        p[field] = ""
    elif mutation == "too_long":
        p[field] = "x" * 600
    elif mutation == "null":
        p[field] = None
    return p


def bridge_cases(rng, variants):
    """One row per distinct (method, params). The runner assigns request ids, so inputs carry none."""
    rows = []
    seen = set()
    for method in METHODS:
        fields = list(RULES.get(method, {})) or [None]
        for field in fields:
            for mutation in MUTATIONS:
                if field is None and mutation not in ("valid", "extra_field"):
                    continue
                for variant in range(variants.get(mutation, 1)):
                    params = base_params(method, rng)
                    if method == "alarm.step" and field is not None and mutation == "wrong_type":
                        params[field] = "yes"
                    p = mutate(params, field, mutation, rng)
                    key = json.dumps({"method": method, "params": p}, sort_keys=True)
                    if key in seen:
                        continue
                    seen.add(key)
                    ok = expected_ok(method, p)
                    request = {"method": method, "params": p}
                    rows.append(("bridge_validation",
                                 f"{method} field={field or '-'} mutation={mutation} variant={variant}",
                                 {"request": request}, {"ok": ok}))
    # raw / malformed envelopes
    for n, (raw, ok) in enumerate([
        ("not json", False),
        ("{}", False),
        ('{"id":1}', False),
        ('{"id":1,"method":42}', False),
        ('{"id":1,"method":"app.version","params":"x"}', True),
        ('[]', False),
    ], 1):
        rows.append(("bridge_validation", f"malformed envelope #{n}", {"request": raw}, {"ok": ok}))
    return rows


# ---------- records and alerts ----------

def rand_text(rng, max_len):
    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789-_./ ·é中"
    n = rng.randint(1, max_len)
    return "".join(rng.choice(alphabet) for _ in range(n))


def rand_json(rng, depth=0):
    kinds = ["int", "str", "bool", "null"] + (["list", "dict"] if depth < 2 else [])
    kind = rng.choice(kinds)
    if kind == "int":
        return rng.randint(-10**6, 10**6)
    if kind == "str":
        return rand_text(rng, 40)
    if kind == "bool":
        return rng.random() < 0.5
    if kind == "null":
        return None
    if kind == "list":
        return [rand_json(rng, depth + 1) for _ in range(rng.randint(0, 4))]
    return {rand_text(rng, 12): rand_json(rng, depth + 1) for _ in range(rng.randint(0, 4))}


def records_cases(rng, count):
    rows, used = [], set()
    while len(rows) < count:
        if rng.random() < 0.1:
            key = "k" * rng.randint(257, 300)  # over the 256-byte limit
        else:
            key = rand_text(rng, 80)
        if key in used:
            continue
        used.add(key)
        value = rand_json(rng)
        key_bytes = len(key.encode("utf-8"))
        set_ok = 1 <= key_bytes <= 256
        rows.append(("records_roundtrip", f"set/get #{len(rows) + 1} key_bytes={key_bytes}",
                     {"key": key, "value": value},
                     {"set_ok": set_ok, "get_ok": set_ok, "value": value if set_ok else None}))
    return rows


def alerts_cases(rng, count):
    rows, used = [], set()
    statuses = ["Open", "Acknowledged", "Escalated", "Closed", "open", ""]
    while len(rows) < count:
        alert_id = rand_text(rng, 40) if rng.random() < 0.9 else "E" * rng.randint(33, 40)
        if alert_id in used:
            continue
        used.add(alert_id)
        status = rng.choice(statuses)
        owner = rand_text(rng, 70) if rng.random() < 0.9 else "o" * rng.randint(65, 70)
        ok = (
            1 <= len(alert_id.encode("utf-8")) <= 32
            and status in ("Open", "Acknowledged", "Escalated")
            and 1 <= len(owner.encode("utf-8")) <= 64
        )
        rows.append(("alerts_response", f"respond #{len(rows) + 1} id_len={len(alert_id)} status={status or 'empty'} owner_len={len(owner)}",
                     {"id": alert_id, "status": status, "owner": owner}, {"ok": ok}))
    return rows


# ---------- alarm cases ----------

def alarm_rows(rng):
    rows = []
    # Exhaustive single frame from a fresh state, both threshold sets: 2^9 * 2
    for zoom in (False, True):
        for bits in itertools.product([False, True], repeat=len(FACTS)):
            f = dict(zip(FACTS, bits))
            state = run_frames(zoom, [f])[0]
            rows.append(("alarm_exhaustive_single", f"zoom={zoom} bits={''.join('1' if b else '0' for b in bits)}",
                         {"zoom": zoom, "frames": [f]}, {"states": [state]}))

    # Boundary runs around each threshold, both zoom modes
    for zoom in (False, True):
        t = (30, 100, 200) if zoom else (4, 20, 200)
        for k in range(1, 61):
            phases = {
                "yellow_then_near": [frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True)] * k
                + [frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True,
                         elevatorPresent=True, elevatorNearPlatform=True)] * k,
                "near_then_collision": [frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True,
                                              elevatorPresent=True, elevatorNearPlatform=True)] * (t[1] + k)
                + [frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True,
                         elevatorPresent=True, elevatorNearPlatform=True, platformElevatorCollision=True)] * (t[2] - k + 1),
                "absent_then_tilt": [frame()] * k + [frame(linkTiltPresent=True, linkTiltNearDerrickman=True)] * 2,
            }
            for name, frames in phases.items():
                rows.append(("alarm_boundary", f"zoom={zoom} k={k} {name}",
                             {"zoom": zoom, "frames": frames}, {"states": run_frames(zoom, frames)}))

    # Random sequences: biased facts, lengths 1..120, deduplicated
    seen = set()
    while sum(1 for r in rows if r[0] == "alarm_random_sequence") < 9000:
        zoom = rng.random() < 0.3
        bias = {k: rng.uniform(0.05, 0.95) for k in FACTS}
        length = rng.randint(1, 120)
        frames = [{k: rng.random() < bias[k] for k in FACTS} for _ in range(length)]
        key = json.dumps([zoom, frames], sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        rows.append(("alarm_random_sequence", f"seq #{len(rows) + 1} zoom={zoom} len={length}",
                     {"zoom": zoom, "frames": frames}, {"states": run_frames(zoom, frames)}))
    return rows


def self_check():
    # Hand-derived from ews.cpp (see tests/backend_test.cpp): 5 yellow frames, then 26 near frames -> ORANGE,
    # then 227 collision frames -> RED (red counter went to -26 during the orange phase).
    yellow = frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True)
    near = frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True,
                 elevatorPresent=True, elevatorNearPlatform=True)
    collide = frame(machinePresent=True, linkTiltPresent=True, linkTiltAngled=True,
                    elevatorPresent=True, elevatorNearPlatform=True, platformElevatorCollision=True)
    states = run_frames(False, [yellow] * 5)
    assert states[-1] == "YELLOW" and states[3] == "NONE"
    oracle = AlarmOracle(False)
    for _ in range(5):
        oracle.step(yellow)
    n = 0
    while oracle.state != "ORANGE":
        oracle.step(near)
        n += 1
    assert n == 26, n
    m = 0
    while oracle.state != "RED":
        oracle.step(collide)
        m += 1
    assert m == 227, m


def assert_distinct(rows):
    """Every row must be distinct: unique title and unique (area, input). Fails the generation otherwise."""
    titles, inputs = set(), set()
    for area, title, inp, _ in rows:
        assert title not in titles, f"duplicate title: {title}"
        key = (area, json.dumps(inp, sort_keys=True))
        assert key not in inputs, f"duplicate input in {area}: {title}"
        titles.add(title)
        inputs.add(key)


def main():
    self_check()
    rng = random.Random(SEED)

    rows = []
    rows += alarm_rows(rng)
    rows += bridge_cases(rng, variants={"valid": 3, "extra_field": 3})
    rows += records_cases(rng, 2500)
    rows += alerts_cases(rng, 2500)
    assert_distinct(rows)
    # newer host features: every row below expands to several checks when it runs (see more_checks.py)
    more = random.Random(SEED + 1)
    rows += more_cases.auth_scenarios(more, 90)
    rows += more_cases.evidence_questions(more, 1000)
    rows += more_cases.chat_cases(more, 650)
    rows += more_cases.chat_trim_case()
    rows += more_cases.bridge_new_cases(more, 10)

    with OUT.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["case_id", "area", "title", "input_json", "expected_json"])
        for i, (area, title, inp, exp) in enumerate(rows, 1):
            writer.writerow([f"TC-{i:05d}", area, title,
                             json.dumps(inp, ensure_ascii=False, separators=(",", ":")),
                             json.dumps(exp, ensure_ascii=False, separators=(",", ":"))])

    counts = {}
    for area, *_ in rows:
        counts[area] = counts.get(area, 0) + 1
    print(f"wrote {len(rows)} cases to {OUT}")
    for area, n in sorted(counts.items()):
        print(f"  {area}: {n}")


if __name__ == "__main__":
    main()
