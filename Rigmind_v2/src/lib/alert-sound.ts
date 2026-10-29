import redAlert from "@/assets/sounds/redAlert.mp3";
import orangeAlert from "@/assets/sounds/orangeAlert.mp3";

// Same tones as IDSS: RED for critical, ORANGE for warning. Advisory stays silent.
const SOUND_BY_SEVERITY: Record<string, string> = {
  Critical: redAlert,
  Warning: orangeAlert,
};

let player: HTMLAudioElement | undefined;

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
  if (value) player?.pause();
  listeners.forEach((fn) => fn());
}

/** Plays the alert tone for a severity. Silent when muted, when the platform blocks audio or has no tone. */
export function playAlertSound(severity: string) {
  if (muted) return;
  const src = SOUND_BY_SEVERITY[severity];
  if (!src) return;
  try {
    player ??= new Audio();
    player.src = src;
    player.currentTime = 0;
    void player.play().catch((e) => console.warn("alert sound blocked", e));
  } catch (e) {
    console.warn("alert sound unavailable", e);
  }
}
