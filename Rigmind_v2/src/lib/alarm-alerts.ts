import { raiseAlert, updateAlert, type AlertPhase, type RaisedAlert } from "./alert-bus";
import { isNative, onNativeEvent, type NativeEvent } from "./native-bridge";
import { setAlertLoop } from "./alert-sound";

/** EWS alarm state -> app alert severity (same names the alert inbox and tones use). */
export const ALARM_SEVERITY = { Yellow: "Advisory", Orange: "Warning", Red: "Critical" } as const;
type Live = keyof typeof ALARM_SEVERITY;
const LEVEL: Record<Live, number> = { Yellow: 0, Orange: 1, Red: 2 };

/*
 * Episode model: one alert per episode, escalating in place.
 * Event cycle: Yellow>Orange>Yellow is a normal cycle; Yellow>Orange>Red>Orange>Yellow and Yellow>Orange>Red>Yellow are critical
 * cycles. Falling back to Yellow ends the cycle (the host sends closed:{alertId,peak,critical,cycle}); that Yellow opens the next
 * item. A closed cycle is marked Cleared. Its plain-language summary is requested and stored by the host (also with the page closed);
 * the page shows it from the alert row.
 *   Yellow = "monitoring" (quiet: no toast, no bell, no tone; the item sits in the inbox as Monitoring)
 *   Orange = Warning, Escalated: toast + bell entry
 *   Red    = Critical, Escalated: toast + bell entry + red tone + OS notification
 *   None   = the episode is Cleared (the item stays in history); the next non-None state opens a new item.
 * Alert text is facts only: `condition` is a neutral level label ("Orange alert"), `place` the camera id. The facts and the SLM
 * sentence come from the host row (see alert-text.ts). The host (alarm_recorder.cpp) records every transition and sends alertId / phase / escalation. When those
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
    const show = () => new Notification(`${a.condition} · ${a.place}`, { body: a.id, tag: a.id });
    if (Notification.permission === "granted") show();
    else if (Notification.permission === "default")
      void Notification.requestPermission().then((p) => p === "granted" && show()).catch(() => {});
  } catch (e) {
    console.warn("OS notification unavailable", e);
  }
}

const str = (v: unknown) => (typeof v === "string" ? v : "");

/** "Y>O>R>O>Y" -> "Yellow, Orange, Red, Orange, Yellow" */
const CYCLE_NAME: Record<string, string> = { Y: "Yellow", O: "Orange", R: "Red", N: "clear" };
export const cycleWords = (path: string) => path.split(">").map((p) => CYCLE_NAME[p] ?? p).join(", ");

/** Neutral label of a level: no sentence about what happened. */
export const levelLabel = (level: number) => (level >= 2 ? "Red alert" : level === 1 ? "Orange alert" : "Yellow monitoring");

/** Marks a finished event cycle Cleared; the row keeps its peak level label. The host writes the summary. Runs once per alert id. */
function closeCycle(closed: Record<string, unknown>, place: string) {
  const id = str(closed["alertId"]);
  if (!id || cleared.has(id)) return;
  cleared.add(id);
  const peak = typeof closed["peak"] === "number" ? (closed["peak"] as number) : 0;
  updateAlert({ id, severity: ALARM_SEVERITY[levelName(peak)], condition: levelLabel(peak), place, phase: "Cleared" });
}

/**
 * Applies one host alarm event to the episode of its camera. Returns the alert when this event presented it
 * (Orange or Red reached for the first time in the episode); Yellow, repeats and the return to None return nothing.
 */
export function handleAlarmEvent(msg: NativeEvent): RaisedAlert | undefined {
  if (msg.ev !== "alarm" || typeof msg.cam !== "string" || !msg.cam) return;
  const state = msg["state"];
  if (state !== "None" && state !== "Yellow" && state !== "Orange" && state !== "Red") return;
  // The tone loops while the alarm is Orange or Red and stops when it falls back to Yellow (or clears).
  setAlertLoop(msg.cam, state === "Red" ? "Critical" : state === "Orange" ? "Warning" : null);
  const ep = episodes.get(msg.cam) ?? { n: 1, last: "None" };
  episodes.set(msg.cam, ep);
  const hostId = str(msg["alertId"]);
  const localId = `ALM-${msg.cam}-${boot}${ep.n}`.slice(0, 32);
  const id = hostId || localId;
  const place = (str(msg["place"]) || msg.cam).slice(0, 128);
  const condition = (str(msg["condition"]) || (state === "None" ? "Cleared" : levelLabel(LEVEL[state]))).slice(0, 128);
  // Rows the host stored carry metadata / summary (null until written); a plain browser has none.
  const hostFields = hostId ? { metadata: null, summary: null, summarySource: null } : {};

  const closed = msg["closed"];
  if (closed && typeof closed === "object") {
    ep.n++; // the next non-None state starts a new item (local ids)
    closeCycle(closed as Record<string, unknown>, place);
  }

  if (state === "None") {
    const wasOpen = ep.last !== "None" || hostId !== ""; // a host id means the host knows an episode
    ep.last = "None";
    if (!wasOpen || cleared.has(id)) return;
    cleared.add(id);
    ep.n++; // the next non-None state starts a new episode (local ids)
    const shownLevel = presented.get(id) ?? 0;
    updateAlert({ id, severity: ALARM_SEVERITY[levelName(shownLevel)], condition: levelLabel(shownLevel), place, phase: "Cleared" });
    return;
  }

  ep.last = state;
  const level = LEVEL[state];
  const shown = presented.get(id);
  if (shown === undefined) presented.set(id, 0); // episode opened
  if (level === 0 || (shown !== undefined && shown >= level)) return; // Yellow is quiet; repeats change nothing
  presented.set(id, level);
  const phase: AlertPhase = "Escalated";
  const alert: RaisedAlert = { id, severity: ALARM_SEVERITY[state], condition: levelLabel(level), place, phase, ...hostFields };
  raiseAlert(alert, { persist: false }); // the host already stored the row; this is toast + bell + tone
  if (state === "Red") {
    if (!isNative()) osNotify(alert); // inside the desktop app the host raises the Windows notification from the tray icon
  }
  return alert;
}

function levelName(level: number): Live {
  return level >= 2 ? "Red" : level === 1 ? "Orange" : "Yellow";
}

let stop: (() => void) | undefined;
/** Subscribes once to host alarm events. Idempotent; returns the unsubscribe function. */
export function startAlarmAlerts(): () => void {
  // A camera that goes offline cannot report its alarm clearing: its looping tone stops too.
  stop ??= onNativeEvent((msg) => {
    if (msg.ev === "status" && msg["state"] === "offline" && typeof msg.cam === "string") setAlertLoop(msg.cam, null);
    handleAlarmEvent(msg);
  });
  return () => {
    stop?.();
    stop = undefined;
  };
}
