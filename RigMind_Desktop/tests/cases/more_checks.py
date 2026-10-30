"""Runners for the areas in more_cases.py. Each returns a list of (name, passed, detail) sub-checks; every sub-check counts as one case."""

import json
import subprocess
import tempfile
import time
from datetime import datetime
from pathlib import Path

import more_cases as mc

GENERIC_WRONG = "Wrong username or password"


class Cli:
    """A bridge CLI process of its own (fresh database), with optional extra arguments."""

    def __init__(self, binary, extra=()):
        self.db = str(Path(tempfile.mkdtemp(prefix="rigmind-more-")) / "rigmind.db")
        self.proc = subprocess.Popen([binary, self.db, *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.n = 0

    def send(self, method, params=None):
        self.n += 1
        self.proc.stdin.write(json.dumps({"id": self.n, "method": method, "params": params if params is not None else {}}, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("bridge cli exited")
        return json.loads(line)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            self.proc.kill()


# ------------------------------------------------------------------------------------------------ auth


def norm_user(raw):
    return raw.strip(" \t\r\n").lower()


def check_auth_scenario(binary, inp):
    out = []
    cli = Cli(binary, ["--fast-auth"])
    try:
        st = cli.send("auth.status")
        out.append(("status before setup", st.get("ok") and st["result"]["hasUsers"] is False, str(st)))
        account = None  # (normalised username, password)
        # --- setup attempts
        for i, (user, pw) in enumerate(inp["setup"]):
            reply = cli.send("auth.setup", {"username": user, "password": pw})
            ub, pb = len(user.encode("utf-8")), len(pw.encode("utf-8"))
            if ub == 0 or ub > 64 or pb == 0 or pb > 128:
                exp_ok, needle = False, None  # refused by the bridge's own length rule
            elif account is not None:
                exp_ok, needle = False, "already exists"
            else:
                nu = norm_user(user)
                uerr = mc.username_problem(user)
                if uerr:
                    exp_ok, needle = False, uerr
                else:
                    perr = mc.password_problem(nu, pw)
                    if perr:
                        exp_ok, needle = False, perr
                    else:
                        exp_ok, needle = True, None
                        account = (nu, pw)
            good = bool(reply.get("ok")) == exp_ok and (exp_ok or needle is None or needle in str(reply.get("error", "")))
            out.append((f"setup attempt {i}", good, f"user={user!r} got {reply.get('ok')} {reply.get('error', '')!r} expected ok={exp_ok} {needle or ''}"))
        if account is None:
            return out + [("an account exists after the valid attempt", False, "none created")]
        st = cli.send("auth.status")
        out.append(("status after setup", st.get("ok") and st["result"]["hasUsers"] is True, str(st)))
        # --- sign-in
        user, pw = account
        fails = {}

        def login(u, p, expect):
            reply = cli.send("auth.login", {"username": u, "password": p})
            if len(p.encode("utf-8")) > 128 or len(p.encode("utf-8")) == 0 or len(u.encode("utf-8")) > 64 or len(u.encode("utf-8")) == 0:
                return (not reply.get("ok")), f"login {u!r}: refused by the bridge's length rule, got {reply.get('ok')}"  # not counted as a failed attempt
            key = norm_user(u) if not mc.username_problem(u) else None
            locked = key is not None and fails.get(key, 0) >= 5
            if locked:
                exp_ok, needle = False, "Try again in"
            else:
                exp_ok, needle = expect, (None if expect else GENERIC_WRONG)
            ok_match = bool(reply.get("ok")) == exp_ok and (exp_ok or needle in str(reply.get("error", "")))
            if key is not None:
                if reply.get("ok"):
                    fails[key] = 0
                elif not locked:
                    fails[key] = fails.get(key, 0) + 1
            return ok_match, f"login {u!r}: got {reply.get('ok')} {reply.get('error', '')!r} expected ok={exp_ok} {needle or ''}"

        variants = [user, user.upper(), "  " + user + " ", inp["variant"] if norm_user(inp["variant"]) == user else user]
        for v in variants:
            good, d = login(v, pw, True)
            out.append((f"right password as {v!r}", good, d))
        for w in inp["wrong"]:
            if w == pw:
                continue
            good, d = login(user, w, False)
            out.append(("wrong password refused", good, d))
        if norm_user(inp["unknown"]) != user:
            good, d = login(inp["unknown"], pw, False)
            out.append(("unknown user refused with the same message", good, d))
        good, d = login("not valid!", pw, False)
        out.append(("impossible username refused with the same message", good, d))
        good, d = login(user, pw, True)
        out.append(("sign in works after the failures (under the limit)", good, d))
        if inp["lockout"]:
            for k in range(5):
                good, d = login(user, f"nope-nope-{k}-xx", False)
                out.append((f"failure {k + 1} of 5", good, d))
            good, d = login(user, pw, False)  # locked: the right password is refused too
            out.append(("locked after 5 failures, even with the right password", good, d))
    finally:
        cli.close()
    return out


# -------------------------------------------------------------------------------------------- evidence

DAY = 86400000


def local_midnight_ms(now_ms):
    d = datetime.fromtimestamp(now_ms / 1000)
    return int(d.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)


def last_n(q):
    for lead in ("last ", "past "):
        at = q.find(lead)
        while at != -1:
            i = at + len(lead)
            n, digits = 0, 0
            while i < len(q) and q[i].isdigit() and q[i].isascii() and digits < 4:
                n = n * 10 + int(q[i])
                i += 1
                digits += 1
            if digits == 0:
                n = 1
            else:
                while i < len(q) and q[i] == " ":
                    i += 1
            unit, name = 0, ""
            if q[i:i + 4] == "hour":
                unit, name = 3600000, "hour"
            elif q[i:i + 3] == "min":
                unit, name = 60000, "minute"
            elif q[i:i + 3] == "day":
                unit, name = DAY, "day"
            elif q[i:i + 4] == "week":
                unit, name = 7 * DAY, "week"
            if unit > 0 and n > 0:
                return n * unit, "the last " + (name if n == 1 else f"{n} {name}s")
            at = q.find(lead, at + 1)
    return 0, ""


def has_word(q, word):
    i = q.find(word)
    while i != -1:
        left = i == 0 or not (q[i - 1].isascii() and q[i - 1].isalpha())
        right = i + len(word) >= len(q) or not (q[i + len(word)].isascii() and q[i + len(word)].isalpha())
        if left and right:
            return True
        i = q.find(word, i + 1)
    return False


def parse_question(question, now, midnight, cams):
    q = question.lower()
    kind, cam, since, until, limit, window = "", "", now - 7 * DAY, 0, 6, "the last 7 days"
    if any(w in q for w in ("orange", "image", "photo", "picture", "snapshot")):
        kind = "image"
    if has_word(q, "red"):
        kind = "video"
    if any(w in q for w in ("video", "clip", "footage")):
        kind = "video"
    if ("orange" in q or "image" in q) and ("video" in q or "clip" in q):
        kind = ""
    for c in cams:
        if c and c.lower() in q:
            cam = c
    span, label = last_n(q)
    if span:
        since, window = now - span, label
    elif "yesterday" in q:
        since, until, window = midnight - DAY, midnight, "yesterday"
    elif "today" in q or "this morning" in q or "tonight" in q:
        since, window = midnight, "today"
    if "latest" in q or "most recent" in q or "newest" in q or ("last " in q and window == "the last 7 days"):
        limit = 1
    if "all " in q or "every" in q or "how many" in q or "list" in q:
        limit = 12
    return kind, cam, since, until, limit, window


def select(rows, kind, cam, since, until, limit, now):
    hit = []
    for r in rows:
        k = r.get("kind", "video")
        if kind and k != kind:
            continue
        if cam and r["camId"] != cam:
            continue
        t = r["timestampMs"]
        if t < since or t >= (until if until else now + 1):
            continue
        hit.append(r)
    hit.sort(key=lambda r: -r["timestampMs"])
    return hit


def describe(matches, total, kind, cam, window):
    what = "Orange image set" if kind == "image" else "Red video set" if kind == "video" else "evidence set"
    where = f" for {cam}" if cam else ""
    if not matches:
        return f"No {what}s found{where} in {window}. Orange alerts save images and Red alerts save video; evidence is kept for 7 days."
    out = f"Found {total} {what}{'' if total == 1 else 's'}{where} in {window}."
    if len(matches) < total:
        out += f" Showing the newest {len(matches)}."
    return out


SEED = [(5, "CAM-01", "image"), (12, "CAM-02", "video"), (40, "CAM-01", "video"), (100, "CAM-10", "image"), (200, "CAM-02", "image"),
        (400, "CAM-01", "image"), (700, "CAM-10", "video"), (1300, "CAM-01", "video"), (1700, "CAM-02", "image"), (2500, "CAM-01", "image"),
        (3100, "CAM-10", "image"), (4200, "CAM-02", "video"), (6000, "CAM-01", "image"), (8000, "CAM-10", "video")]  # minutes ago


class Evidence:
    def __init__(self, cli):
        self.cli = cli
        self.rows = []
        self.cams = []
        now = int(time.time() * 1000)
        midnight = local_midnight_ms(now)
        boundaries = [now - 45 * 60000, now - 60 * 60000, now - 90 * 60000, now - 3 * 3600000, now - DAY, now - 2 * DAY, midnight, midnight - DAY]
        for minutes, cam, kind in SEED:
            t = now - minutes * 60000
            for _ in range(4):  # keep every row at least 10 minutes from every window edge, so the minutes the run takes cannot flip it
                if any(abs(t - b) < 600000 for b in boundaries):
                    t -= 720000
            reply = cli.send("footage.record", {"cam": cam, "stamp": str(t), "count": 3, "kind": kind})
            if not reply.get("ok"):
                raise RuntimeError(f"seeding failed: {reply}")
        listed = cli.send("footage.list", {"limit": 500})
        self.rows = listed["result"]
        self.cams = []
        for r in self.rows:  # the host walks newest first and keeps the last camera named in the question
            if r["camId"] not in self.cams:
                self.cams.append(r["camId"])

    def check(self, inp):
        now = int(time.time() * 1000)
        midnight = local_midnight_ms(now)
        kind, cam, since, until, limit, window = parse_question(inp["question"], now, midnight, self.cams)
        everything = select(self.rows, kind, cam, since, until, limit, now)
        matches = everything[:limit]
        note = describe(matches, len(everything), kind, cam, window)
        reply = self.cli.send("evidence.ask", {"question": inp["question"]})
        if not reply.get("ok"):
            return [("evidence.ask answers", False, str(reply))]
        res = reply["result"]
        got = [m["path"] for m in res["matches"]]
        want = [m["path"] for m in matches]
        checks = [
            ("matching evidence sets", got == want, f"q={inp['question']!r} got {got} expected {want}"),
            ("plain answer", res["note"] == note, f"got {res['note']!r} expected {note!r}"),
            ("no model without a sink", res["slm"] is False and str(res["chatId"]).startswith("chat-"), str(res)[:160]),
        ]
        return checks


# ------------------------------------------------------------------------------------------------ chat


def check_chat(cli, inp):
    if "bad_id" in inp:
        params = {"id": inp["bad_id"]}
        if inp["method"] == "chat.save":
            params = {"session": {"id": inp["bad_id"], "turns": []}}
        reply = cli.send(inp["method"], params)
        return [(f"{inp['method']} with a bad id is refused", not reply.get("ok"), f"id={inp['bad_id']!r} {reply}")]
    sid, turns = inp["id"], inp["turns"]
    want = mc.normalise_session(turns)
    saved = cli.send("chat.save", {"session": {"id": sid, "turns": turns}})
    out = [("chat.save", bool(saved.get("ok")) and saved["result"]["turns"] == len(want), str(saved)[:200])]
    loaded = cli.send("chat.load", {"id": sid})
    if not loaded.get("ok"):
        return out + [("chat.load", False, str(loaded)[:200])]
    got = loaded["result"]
    out.append(("turns round trip", got["turns"] == want, f"got {json.dumps(got['turns'], ensure_ascii=False)[:300]} expected {json.dumps(want, ensure_ascii=False)[:300]}"))
    title = ""
    for t in want:
        if t["role"] == "you":
            title = mc._cut(t["text"], 60)
            break
    out.append(("title", got["title"] == title and got["id"] == sid, f"title {got['title']!r} expected {title!r}"))
    toon = cli.send("chat.toon", {"id": sid})
    out.append(("toon text", bool(toon.get("ok")) and toon["result"]["toon"].startswith(f"id: {sid}\n") and f"turns[{len(want)}]" in toon["result"]["toon"], str(toon)[:160]))
    listing = cli.send("chat.list")
    row = [r for r in listing["result"] if r["id"] == sid] if listing.get("ok") else []
    out.append(("listed with its turn count", len(row) == 1 and row[0]["turns"] == len(want), str(row)[:160]))
    return out


def check_trim(cli, inp):
    for n in range(inp["n"]):
        cli.send("chat.save", {"session": {"id": f"chat-trim-{n:03d}", "turns": [{"t": n, "role": "you", "text": f"q{n}"}]}})
        time.sleep(0.002)  # distinct updated times
    listing = cli.send("chat.list")["result"]
    ids = [r["id"] for r in listing]
    gone = cli.send("chat.load", {"id": "chat-trim-000"})
    return [("only the newest 50 sessions stay", len(listing) == 50, f"{len(listing)} listed"),
            ("the newest is kept", "chat-trim-054" in ids, "chat-trim-054 missing"),
            ("the oldest is gone", not gone.get("ok"), str(gone)[:120])]


# ------------------------------------------------------------------------------------------ bridge_new


def check_bridge_new(cli, inp):
    method, params = inp["method"], inp["params"]
    reply = cli.send(method, params)
    ok_shape = True
    for field, kind, required, max_bytes in mc.NEW_METHODS[method]:
        v = params.get(field, "__missing__")
        if v == "__missing__" or v is None:
            ok_shape = False
        elif kind == "bool":
            ok_shape = ok_shape and isinstance(v, bool)
        else:
            ok_shape = ok_shape and isinstance(v, str) and len(v.encode("utf-8")) > 0 and (not max_bytes or len(v.encode("utf-8")) <= max_bytes)
    if method == "evidence.ask":
        expected = ok_shape
    elif method == "chat.delete":
        expected = ok_shape and all(c.isalnum() or c in "-_" for c in params.get("id", "")) and params.get("id", "x").isascii()
    elif method in ("chat.load", "chat.toon"):
        expected = False  # a valid id that was never saved
    else:  # stream.overlay: no pipeline behind the CLI
        expected = False
    return [(f"{method} {inp.get('mutation', '')}", bool(reply.get("ok")) == expected, f"params={json.dumps(params)[:120]} got {reply.get('ok')} {reply.get('error', '')!r} expected {expected}")]


class MoreChecks:
    AREAS = {"auth_scenario", "evidence_ask", "chat_roundtrip", "chat_trim", "bridge_new"}

    def __init__(self, binary):
        self.binary = binary
        self.cli = Cli(binary)  # one long-lived process for the evidence, chat and request-validation areas
        self.evidence = None

    def run(self, area, inp, exp):
        if area == "auth_scenario":
            return check_auth_scenario(self.binary, inp)
        if area == "evidence_ask":
            if self.evidence is None:
                self.evidence = Evidence(self.cli)
            return self.evidence.check(inp)
        if area == "chat_roundtrip":
            return check_chat(self.cli, inp)
        if area == "chat_trim":
            return check_trim(self.cli, inp)
        if area == "bridge_new":
            return check_bridge_new(self.cli, inp)
        raise KeyError(area)

    def close(self):
        self.cli.close()
