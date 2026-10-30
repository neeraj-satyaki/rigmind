import redAlert from "@/assets/sounds/redAlert.wav";
import orangeAlert from "@/assets/sounds/orangeAlert.wav";

// RED for critical, ORANGE for warning (the IDSS tones, converted to lossless 24-bit WAV and levelled: Red about -12 dBFS
// average, Orange about -15.5 dBFS, both peaking below -1 dBFS). Advisory stays silent.
const SOUND_BY_SEVERITY: Record<string, string> = {
  Critical: redAlert,
  Warning: orangeAlert,
};

// One preloaded player per tone: a repeat starts at once, with no reload and no decoder start-up gap.
const players = new Map<string, HTMLAudioElement>();
let lastEndedAt = 0;

function playerFor(severity: string): HTMLAudioElement | undefined {
  const src = SOUND_BY_SEVERITY[severity];
  if (!src) return undefined;
  let p = players.get(severity);
  if (!p) {
    p = new Audio();
    p.preload = "auto";
    p.src = src;
    p.addEventListener?.("ended", () => {
      lastEndedAt = Date.now();
    });
    players.set(severity, p);
  }
  return p;
}

const anyPlaying = () => [...players.values()].some((p) => !p.paused && !p.ended);
const stopAll = () => players.forEach((p) => p.pause());

const MUTE_KEY = "rigmind.alerts.muted";
const listeners = new Set<() => void>();
let muted = false;
try {
  muted = typeof localStorage !== "undefined" && localStorage.getItem(MUTE_KEY) === "1";
} catch {
  /* storage unavailable: alerts stay audible */
}

export function isAlertMuted(): boolean {
  return muted;
}

export function subscribeAlertMuted(fn: () => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

/** Mutes or unmutes the alert tones for this device; a tone that is playing stops at once. */
export function setAlertMuted(value: boolean) {
  muted = value;
  try {
    localStorage.setItem(MUTE_KEY, value ? "1" : "0");
  } catch {
    /* kept for this session only */
  }
  if (value) stopAll();
  listeners.forEach((fn) => fn());
}

/**
 * Looping alarm: while any camera's alarm is Orange (Warning) or Red (Critical) the tone repeats; it stops when every alarm is back
 * to Yellow/clear, when the camera goes offline, and while muted (it resumes on unmute if an alarm is still active).
 * The first tone is the one-shot that comes with the toast; the loop only repeats it, after a short pause (Critical 1.2 s, Warning 2.5 s).
 */
const LOOP_GAP_MS: Record<string, number> = { Critical: 1200, Warning: 2500 };
const LOOP_FIRST_AFTER_MS = 2400; // the toast's own tone starts about 1.8 s after the alarm; the loop waits for it
const LOOP_TICK_MS = 400;
const loops = new Map<string, { severity: string; since: number }>();
let loopTimer: ReturnType<typeof setInterval> | undefined;

function topSeverity(): string | undefined {
  let top: string | undefined;
  for (const { severity } of loops.values()) if (severity === "Critical" || (severity === "Warning" && top !== "Critical")) top = severity;
  return top;
}

function loopTick() {
  const top = topSeverity();
  if (!top || muted) return;
  const now = Date.now();
  const earliest = Math.min(...[...loops.values()].map((l) => l.since));
  if (anyPlaying() || now - earliest < LOOP_FIRST_AFTER_MS) return;
  if (lastEndedAt && now - lastEndedAt < (LOOP_GAP_MS[top] ?? 2500)) return;
  playAlertSound(top);
}

/** Sets (Warning | Critical) or clears (null / any other severity) the looping alarm of one camera or source. */
export function setAlertLoop(key: string, severity: string | null) {
  if (severity === "Critical" || severity === "Warning") {
    const had = loops.get(key);
    loops.set(key, { severity, since: had?.since ?? Date.now() });
    loopTimer ??= setInterval(loopTick, LOOP_TICK_MS);
  } else {
    loops.delete(key);
  }
  if (loops.size === 0) {
    if (loopTimer) clearInterval(loopTimer);
    loopTimer = undefined;
    stopAll(); // back to Yellow: the tone stops at once
  }
}

/** True while at least one alarm is looping (for the Live page and tests). */
export const isAlertLooping = () => loops.size > 0;

/** Plays the alert tone for a severity. Silent when muted, when the platform blocks audio or has no tone. */
export function playAlertSound(severity: string) {
  if (muted) return;
  try {
    const p = playerFor(severity);
    if (!p) return;
    p.currentTime = 0;
    void p.play().catch((e) => console.warn("alert sound blocked", e));
  } catch (e) {
    console.warn("alert sound unavailable", e);
  }
}
