"""Acceptance rules for the BitNet plain-language sentence.
Severity is already shown by the toast, so the sentence does not have to repeat it.

Used by scripts/host.py (before a model sentence reaches the page) and by tests/slm/prompt_harness.py,
so the harness checks exactly what ships. A rejected sentence means the page keeps the template text.
"""

import re

MAX_CHARS = 220
MIN_CHARS = 20
LEAK_MARKERS = ("event:", "sentence:", "answer:", "facts:", "write one", "rewrite", "##", "**")


def first_sentence(text):
    """First non-empty line, cut after its first full stop. Models often add more sentences; only one is shown."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return ""
    # Drop list numbers and stray leading punctuation the model adds ("1: ", ": ", "- ").
    head = re.sub(r"^\s*(?:\d+[:.)]\s*|[:\-#*]+\s*)", "", lines[0])
    match = re.search(r"[.!?](\s|$)", head)
    return head[: match.end()].strip() if match else head


def reject_reason(text, event):
    """Returns None when the sentence is acceptable for this event, else the reason it was rejected."""
    if not text:
        return "empty"
    if len(text) > MAX_CHARS or len(text) < MIN_CHARS:
        return f"length {len(text)}"
    lowered = text.lower()
    for marker in LEAK_MARKERS:
        if marker in lowered:
            return f"leaked '{marker}'"
    camera = str(event.get("camId", ""))
    if camera and camera.lower() not in lowered:
        return f"missing camera '{camera}'"
    parts = [part.strip().lower() for part in str(event.get("place", "")).split("·") if part.strip()]
    if parts and not any(part in lowered for part in parts):
        return f"missing place '{event.get('place')}'"
    return None
