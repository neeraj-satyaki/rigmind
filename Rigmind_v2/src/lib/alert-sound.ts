import redAlert from "@/assets/sounds/redAlert.mp3";
import orangeAlert from "@/assets/sounds/orangeAlert.mp3";

// Same tones as IDSS: RED for critical, ORANGE for warning. Advisory stays silent.
const SOUND_BY_SEVERITY: Record<string, string> = {
  Critical: redAlert,
  Warning: orangeAlert,
};

let player: HTMLAudioElement | undefined;

/** Plays the alert tone for a severity. Silent when the platform blocks audio or has no tone. */
export function playAlertSound(severity: string) {
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
