import { isNative, nativeCall } from "@/lib/native-bridge";

export type RaisedAlert = { id: string; severity: string; condition: string; place: string };
export const ALERT_RAISED = "rigmind:alert-raised";
export const ALERT_READY = "rigmind:alert-ready";
/** Logo animation runs first; notification appears after it. */
export const LOGO_ALERT_MS = 1800;

export function raiseAlert(a: RaisedAlert) {
  // Toast, bell and push presentation stay in the UI; the C++ alerts module stores the event.
  if (isNative()) nativeCall("alert.raise", { ...a }).catch((e) => console.warn("alert.raise failed", e));
  window.dispatchEvent(new CustomEvent<RaisedAlert>(ALERT_RAISED, { detail: a }));
}
