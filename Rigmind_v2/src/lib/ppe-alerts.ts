import { raiseAlert, updateAlert, type RaisedAlert } from "./alert-bus";
import { onNativeEvent, type NativeEvent } from "./native-bridge";

/*
 * PPE compliance episodes. The pipeline checks every person for the required PPE and sends {"ev":"ppe"} when the set of
 * items NOT DETECTED changes (state "missing" | "ok"). The host (PpeRecorder) stores one Advisory alert row per episode and
 * adds alertId / phase / condition. The wording is always "not detected" (the camera may simply not see the item).
 *
 *   first "missing" of an episode  -> yellow Advisory toast (once), bell row, inbox row (Open). No tone.
 *   the missing set changes        -> the bell row is updated in place; no new toast
 *   "ok"                           -> the row becomes Cleared
 */
const presented = new Set<string>();
const cleared = new Set<string>();
const episodes = new Map<string, number>();
const boot = Date.now().toString(36);

/** Forgets de-duplication state. Tests only. */
export function resetPpeAlerts() {
  presented.clear();
  cleared.clear();
  episodes.clear();
}

const str = (v: unknown) => (typeof v === "string" ? v : "");

/** "PPE not detected: vest, gloves" */
export function ppeCondition(missing: string[]): string {
  return `PPE not detected: ${missing.join(", ")}`;
}

/** Applies one host ppe event. Returns the alert when this event presented it (first "missing" of an episode). */
export function handlePpeEvent(msg: NativeEvent): RaisedAlert | undefined {
  if (msg.ev !== "ppe" || typeof msg.cam !== "string" || !msg.cam) return;
  const state = msg["state"];
  if (state !== "missing" && state !== "ok") return;
  const missing = Array.isArray(msg["missing"]) ? (msg["missing"] as unknown[]).filter((m): m is string => typeof m === "string") : [];
  const n = episodes.get(msg.cam) ?? 1;
  const id = str(msg["alertId"]) || `PPE-${msg.cam}-${boot}${n}`.slice(0, 32);
  const condition = (str(msg["condition"]) || ppeCondition(missing)).slice(0, 128);
  const place = msg.cam;

  if (state === "ok") {
    if (!presented.has(id) || cleared.has(id)) return;
    cleared.add(id);
    episodes.set(msg.cam, n + 1); // the next missing item starts a new episode (local ids)
    updateAlert({ id, severity: "Advisory", condition, place, phase: "Cleared" });
    return;
  }
  if (missing.length === 0) return;
  if (presented.has(id)) {
    updateAlert({ id, severity: "Advisory", condition, place, phase: "Open" }); // missing set changed: same row
    return;
  }
  presented.add(id);
  const alert: RaisedAlert = { id, severity: "Advisory", condition, place, phase: "Open" };
  raiseAlert(alert, { persist: false }); // the host stored the row; this is the yellow toast and the bell row (Advisory has no tone)
  return alert;
}

let stop: (() => void) | undefined;
/** Subscribes once to host ppe events. Idempotent; returns the unsubscribe function. */
export function startPpeAlerts(): () => void {
  stop ??= onNativeEvent(handlePpeEvent);
  return () => {
    stop?.();
    stop = undefined;
  };
}
