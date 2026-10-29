import { toast } from "sonner";
import { alertColors } from "@/lib/alert-colors";
import { raiseAlert, updateAlert, type AlertPhase, type RaisedAlert } from "./alert-bus";
import { isNative, onNativeEvent, type NativeEvent } from "./native-bridge";
import { describeAlert } from "./plain-summary";

/** EWS alarm state -> app alert severity (same names the alert inbox and tones use). */
export const ALARM_SEVERITY = { Yellow: "Advisory", Orange: "Warning", Red: "Critical" } as const;
type Live = keyof typeof ALARM_SEVERITY;
const LEVEL: Record<Live, number> = { Yellow: 0, Orange: 1, Red: 2 };

/*
 * Episode model: one alert per episode, escalating in place.
 *   Yellow = "monitoring" (quiet: no toast, no bell, no tone; the item sits in the inbox as Monitoring)
 *   Orange = Warning, Escalated: toast + bell entry
 *   Red    = Critical, Escalated: toast + bell entry + red tone + OS notification + plain-language description
 *   None   = the episode is Cleared (the item stays in history); the next non-None state opens a new item.
 * The host (alarm_recorder.cpp) records every transition and sends alertId / phase / escalation. When those
 * fields are missing (plain browser or an older host) the same episode logic runs here.
 */
type CamEpisode = { n: number; last: string };
const episodes = new Map<string, CamEpisode>();
/** Highest escalation level already presented per alert id (0 = opened quietly). */
const presented = new Map<string, number>();
const cleared = new Set<string>();
const boot = Date.now().toString(36);

/** Forgets de-duplication state. Tests only. */
export function resetAlarmAlerts() {
  episodes.clear();
  presented.clear();
  cleared.clear();
}

/** OS-level notification for a Red when the platform allows it. Never throws. */
function osNotify(a: RaisedAlert) {
  try {
    if (typeof Notification === "undefined") return;
    const show = () => new Notification(`Critical safety event: ${a.condition}`, { body: `${a.id} · ${a.place}`, tag: a.id });
    if (Notification.permission === "granted") show();
    else if (Notification.permission === "default")
      void Notification.requestPermission().then((p) => p === "granted" && show()).catch(() => {});
  } catch (e) {
    console.warn("OS notification unavailable", e);
  }
}

const str = (v: unknown) => (typeof v === "string" ? v : "");

/**
 * Applies one host alarm event to the episode of its camera. Returns the alert when this event presented it
 * (Orange or Red reached for the first time in the episode); Yellow, repeats and the return to None return nothing.
 */
export function handleAlarmEvent(msg: NativeEvent): RaisedAlert | undefined {
  if (msg.ev !== "alarm" || typeof msg.cam !== "string" || !msg.cam) return;
  const state = msg["state"];
  if (state !== "None" && state !== "Yellow" && state !== "Orange" && state !== "Red") return;
  const ep = episodes.get(msg.cam) ?? { n: 1, last: "None" };
  episodes.set(msg.cam, ep);
  const hostId = str(msg["alertId"]);
  const localId = `ALM-${msg.cam}-${boot}${ep.n}`.slice(0, 32);
  const id = hostId || localId;
  const place = str(msg["place"]).slice(0, 128);
  const condition = (str(msg["condition"]) || "Safety condition detected").slice(0, 128);

  if (state === "None") {
    const wasOpen = ep.last !== "None" || hostId !== ""; // a host id means the host knows an episode
    ep.last = "None";
    if (!wasOpen || cleared.has(id)) return;
    cleared.add(id);
    ep.n++; // the next non-None state starts a new episode (local ids)
    updateAlert({ id, severity: ALARM_SEVERITY[levelName(presented.get(id) ?? 0)], condition, place, phase: "Cleared" });
    return;
  }

  ep.last = state;
  const level = LEVEL[state];
  const shown = presented.get(id);
  if (shown === undefined) presented.set(id, 0); // episode opened
  if (level === 0 || (shown !== undefined && shown >= level)) return; // Yellow is quiet; repeats change nothing
  presented.set(id, level);
  const phase: AlertPhase = "Escalated";
  const alert: RaisedAlert = { id, severity: ALARM_SEVERITY[state], condition, place, phase };
  raiseAlert(alert, { persist: false }); // the host already stored the row; this is toast + bell + tone
  if (state === "Red") {
    if (!isNative()) osNotify(alert); // inside the desktop app the host raises the Windows notification from the tray icon
    const colors = alertColors(alert.severity);
    describeAlert(alert, msg.cam, alert.place, (text) =>
      toast(`${alert.id} in plain words`, {
        description: text,
        style: { background: colors.background, color: colors.color, border: `1px solid ${colors.borderColor}` },
        descriptionClassName: "!text-inherit opacity-90",
      }),
    ).catch(() => {});
  }
  return alert;
}

function levelName(level: number): Live {
  return level >= 2 ? "Red" : level === 1 ? "Orange" : "Yellow";
}

let stop: (() => void) | undefined;
/** Subscribes once to host alarm events. Idempotent; returns the unsubscribe function. */
export function startAlarmAlerts(): () => void {
  stop ??= onNativeEvent(handleAlarmEvent);
  return () => {
    stop?.();
    stop = undefined;
  };
}
