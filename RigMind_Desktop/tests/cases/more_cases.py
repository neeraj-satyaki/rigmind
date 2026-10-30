"""Extra generated cases for the newer host features. Expected values come from independent Python rules written from the written
specification (docs/STRUCTURE.md, src/backend/auth_service.h, evidence_query.h, chat_sessions.h), not from the C++ code.

Areas (rows are (area, name, input, expected)):
  auth_scenario   a fresh database: first-run setup attempts (policy), then sign-ins (right, wrong, unknown, lockout)
  evidence_ask    a question against seeded evidence rows; expected matches and plain answer are computed at run time
  chat_roundtrip  save / list / load of random sessions (tricky text, bad turns, bad evidence paths) and bad ids
  chat_trim       55 sessions: only the newest 50 stay
  bridge_new      request validation of the new bridge methods
"""

import random
import string

# ------------------------------------------------------------------------------------------------ auth


def username_problem(raw):
    s = raw.strip(" \t\r\n").lower()
    if not (3 <= len(s.encode("utf-8")) <= 64):
        return "3 to 64"
    if not all((c.isascii() and c.isalnum()) or c in "._@-" for c in s):
        return "letters, digits"
    return ""


def password_problem(username, password):
    n = len(password.encode("utf-8"))
    if n < 10:
        return "at least 10"
    if n > 128:
        return "at most 128"
    if username in password.lower():
        return "must not contain the username"
    has_letter = any(c.isascii() and c.isalpha() for c in password.encode("utf-8").decode("utf-8"))
    has_other = any(not (c.isascii() and c.isalpha()) for c in password)
    if not (has_letter and has_other):
        return "letters and at least one digit or symbol"
    return ""


USER_POOL = ["alice", "Bob.Smith", "  carol  ", "d", "ab", "dave@rig.example", "e e", "frank_01", "ünïcode", "x" * 64, "y" * 65,
             "gina-hse", "HARRY", "ivy.", "jo", "kim@", "-leo-", "mia.w", "no$ign", "olive.o", "", "   "]
PASS_POOL = ["short1", "alllettersonly", "1234567890123", "correct horse 7", "Rig-Mind-2026!", "p" * 129, "pässword-ünï-9", "a1" * 40,
             "alice-is-here-1", "Mixed Case 99!", "tab\tin\tit-5-abc", "        1       ", "x" * 10, "x9" * 64, "emoji-✓-ok-7", "!!!!!!!!!!!!",
             "Valid-Pass-123", "n0-spaces-here-ok", "bob.smith-pw-1", "9876543210abcdef"]


def auth_scenarios(rng, count):
    rows = []
    for s in range(count):
        attempts = []
        for _ in range(rng.randint(4, 9)):
            attempts.append((rng.choice(USER_POOL), rng.choice(PASS_POOL)))
        # one valid pair ends the setup phase; the sign-in phase uses it
        good_user = rng.choice(["alice", "Bob.Smith", "dave@rig.example", "frank_01", "gina-hse", "mia.w", "olive.o", "HARRY"])
        good_pass = rng.choice(["Rig-Mind-2026!", "correct horse 7", "Valid-Pass-123", "Mixed Case 99!", "n0-spaces-here-ok"])
        attempts.append((good_user, good_pass))
        wrongs = [rng.choice(PASS_POOL + ["wrong-wrong-1", "Rig-Mind-2026", "correct horse 8"]) for _ in range(rng.randint(2, 4))]
        wrongs = [w for w in wrongs if w != good_pass]
        unknown = rng.choice(["nobody.here", "root", "admin", "ünï", "zz"])
        variant = rng.choice([good_user, good_user.upper(), "  " + good_user + " ", good_user.title()])
        rows.append(("auth_scenario", f"scenario #{s}",
                     {"setup": attempts, "good": [good_user, good_pass], "wrong": wrongs, "unknown": unknown, "variant": variant,
                      "lockout": rng.random() < 0.5},
                     {}))
    return rows


# -------------------------------------------------------------------------------------------- evidence


KINDS = ["", "", "Orange", "orange images", "Orange evidence", "Red video", "red clips", "clips", "photos", "snapshot", "footage",
         "images and video", "pictures", "the required report", "stored data", "Is there any video"]
CAMS = ["", "", "CAM-01", "cam-02", "CAM-10", "for CAM-02", "from cam-01"]
TIMES = ["", "", "today", "yesterday", "in the last hour", "last 3 hours", "past 45 minutes", "last 2 days", "latest", "most recent",
         "list all", "how many", "this morning", "last week", "past 90 minutes", "in the last day", "newest"]
LEADS = ["Show", "Find", "Any", "What", "Is there", "Give me", "", "Tell me about", "Open", "Did we save"]


def evidence_questions(rng, count):
    rows = []
    for i in range(count):
        parts = [rng.choice(LEADS), rng.choice(KINDS), rng.choice(CAMS), rng.choice(TIMES)]
        q = " ".join(p for p in parts if p)
        if rng.random() < 0.2:
            q = q.upper()
        elif rng.random() < 0.2:
            q = q.lower()
        if rng.random() < 0.3:
            q += rng.choice(["?", "!", " please", "...", "  "])
        if rng.random() < 0.1:
            q = "  " + q
        q = q.strip() or "evidence"
        rows.append(("evidence_ask", f"ask #{i}", {"question": q[:300]}, {}))
    return rows


# ------------------------------------------------------------------------------------------------ chat

TEXT_POOL = ["Latest Orange evidence", "Found 2 Orange image sets, in today.", 'tricky: "quotes", commas,\nnew line, back\\slash, colon: ok',
             "café ✓ 日本語", " leading and trailing space ", "", "a", "line1\r\nline2", ",", "\"", "\\", ":", "x" * 2100,
             "é" * 1100, "pipe | inside", "[brackets] {braces}", "tab\there", "12345", "true", "null"]
EV_POOL = ["CAM-01/1793349677358", "CAM-02/1793349000000", "image:CAM-01/1", "video:CAM-02/2", "bad|path", "a,b", 'q"uote', "x" * 97, "", "CAM-01/ok_1.2-3"]
ROLES = ["you", "assistant", "you", "assistant", "system", "", "user"]
ID_POOL_BAD = ["", "../x", "a b", "chat/1", "x" * 49, "id with space", "chat:1", "ünï"]


def _cut(s, limit):
    """At most `limit` bytes, never inside a character: step back while the byte AFTER the cut is a continuation byte."""
    b = s.encode("utf-8")
    if len(b) <= limit:
        return s
    cut = limit
    while cut > 0 and (b[cut] & 0xC0) == 0x80:
        cut -= 1
    return b[:cut].decode("utf-8")


def normalise_session(turns):
    """What the host keeps: valid roles only, text capped at 2000 bytes, only well formed evidence paths, newest 200 turns."""
    out = []
    for t in turns:
        if not isinstance(t, dict):
            continue
        role = t.get("role", "")
        if role not in ("you", "assistant"):
            continue
        text = t.get("text", "") if isinstance(t.get("text", ""), str) else ""
        ev = [p for p in t.get("ev", []) if isinstance(p, str) and 0 < len(p.encode("utf-8")) <= 96 and not any(c in p for c in ',|\n"')] if isinstance(t.get("ev", []), list) else []
        out.append({"t": t.get("t", 0), "role": role, "text": _cut(text, 2000), "ev": ev})
    return out[-200:]


def chat_cases(rng, count):
    rows = []
    for i in range(count):
        turns = []
        for k in range(rng.randint(0, 8)):
            turn = {"t": rng.randint(0, 2 ** 40), "role": rng.choice(ROLES), "text": rng.choice(TEXT_POOL)}
            if rng.random() < 0.5:
                turn["ev"] = [rng.choice(EV_POOL) for _ in range(rng.randint(0, 3))]
            if rng.random() < 0.03:
                turn = "not an object"
            turns.append(turn)
        sid = "chat-" + "".join(rng.choice(string.ascii_lowercase + string.digits + "-_") for _ in range(rng.randint(3, 30)))
        rows.append(("chat_roundtrip", f"session #{i} turns={len(turns)}", {"id": sid, "turns": turns}, {}))
    for j, bad in enumerate(ID_POOL_BAD):
        for method in ("chat.save", "chat.load", "chat.delete", "chat.toon"):
            rows.append(("chat_roundtrip", f"bad id {j} {method}", {"bad_id": bad, "method": method}, {"ok": False}))
    rows.append(("chat_roundtrip", "huge session keeps the newest 200 turns",
                 {"id": "chat-huge", "turns": [{"t": n, "role": "you" if n % 2 else "assistant", "text": f"turn {n}"} for n in range(260)]}, {}))
    return rows


def chat_trim_case():
    return [("chat_trim", "55 sessions keep the newest 50", {"n": 55}, {"kept": 50})]


# -------------------------------------------------------------------------------------- new bridge methods

NEW_METHODS = {
    # method: (field, kind, required, max_bytes)
    "evidence.ask": [("question", "str", True, 300)],
    "chat.load": [("id", "str", True, 48)],
    "chat.toon": [("id", "str", True, 48)],
    "chat.delete": [("id", "str", True, 48)],
    "stream.overlay": [("cam", "str", True, 32), ("ppe", "bool", True, 0), ("idss", "bool", True, 0)],
}


def bridge_new_cases(rng, per_field=6):
    rows = []
    for method, fields in NEW_METHODS.items():
        for field, kind, required, max_bytes in fields:
            for mutation in ("valid", "missing", "wrong_type", "empty", "too_long", "null"):
                for v in range(per_field):
                    params = {}
                    for f, k, _, _ in fields:
                        params[f] = (rng.random() < 0.5) if k == "bool" else (f"{'CAM' if f == 'cam' else 'q'}-{rng.randint(0, 99999)}" if f != "question" else rng.choice(TEXT_POOL[:6] or ["x"]))
                    if mutation == "missing":
                        params.pop(field)
                    elif mutation == "wrong_type":
                        params[field] = 12345 if kind == "str" else "yes"
                    elif mutation == "empty":
                        params[field] = ""
                    elif mutation == "too_long":
                        params[field] = "x" * (max_bytes + 5 if max_bytes else 600)
                    elif mutation == "null":
                        params[field] = None
                    rows.append(("bridge_new", f"{method} {field} {mutation} #{v}", {"method": method, "params": params, "mutation": mutation}, {}))
    return rows
