import { isNative, nativeCall } from "@/lib/native-bridge";

export type AlertPhase = "Monitoring" | "Escalated" | "Open" | "Cleared";
export type RaisedAlert = { id: string; severity: string; condition: string; place: string; phase?: AlertPhase };
/** Presents an alert (toast, bell, tone). Raising an id the bell already holds updates that item in place. */
export const ALERT_RAISED = "rigmind:alert-raised";
/** Changes an item the bell already holds without presenting it again (for example Cleared). */
export const ALERT_UPDATED = "rigmind:alert-updated";
export const ALERT_READY = "rigmind:alert-ready";
/** Logo animation runs first; notification appears after it. */
export const LOGO_ALERT_MS = 1800;

/**
 * persist=false: the host already owns the row (alarm episodes are recorded by the host itself),
 * so the page only presents it.
 */
export function raiseAlert(a: RaisedAlert, opts: { persist?: boolean } = {}) {
  // Toast, bell and push presentation stay in the UI; the C++ alerts module stores the event.
  if (opts.persist !== false && isNative()) nativeCall("alert.raise", { ...a }).catch((e) => console.warn("alert.raise failed", e));
  window.dispatchEvent(new CustomEvent<RaisedAlert>(ALERT_RAISED, { detail: a }));
}

export function updateAlert(a: RaisedAlert) {
  window.dispatchEvent(new CustomEvent<RaisedAlert>(ALERT_UPDATED, { detail: a }));
}
