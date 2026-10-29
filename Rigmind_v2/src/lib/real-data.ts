// Real (non-sample) data helpers for the analytics, coordination, governance and printable-report pages.
// Used when demoDataEnabled() is false. Nothing here invents values: unknown things are empty or "not recorded".
import { useCallback, useEffect, useRef, useState } from "react";
import { listDevice, readDevice, writeDevice } from "@/lib/device-db";
import { isNative, nativeCall } from "@/lib/native-bridge";
import { audit } from "@/lib/audit";
import type { LiveAlertRow } from "@/lib/live-analytics";

/** Raised alerts from the host (alerts.feed), newest first. Empty outside the desktop host. */
export async function loadAlertFeed(limit = 500): Promise<LiveAlertRow[]> {
  if (!isNative()) return [];
  const rows = await nativeCall<LiveAlertRow[]>("alerts.feed", { limit });
  return Array.isArray(rows) ? rows : [];
}

/** A persisted value (records.get/set). `save` updates state at once and writes in the background. */
export function useRecord<T>(key: string, initial: T) {
  const [value, setValue] = useState<T>(initial);
  const [loaded, setLoaded] = useState(false);
  const latest = useRef<T>(initial);
  useEffect(() => {
    let off = false;
    readDevice<T>(key)
      .then((v) => {
        if (off || v === undefined || v === null) return;
        latest.current = v;
        setValue(v);
      })
      .catch(() => {})
      .finally(() => {
        if (!off) setLoaded(true);
      });
    return () => {
      off = true;
    };
  }, [key]);
  /** Accepts the new value or an updater over the latest value. */
  const save = useCallback(
    (next: T | ((prev: T) => T)) => {
      const v = typeof next === "function" ? (next as (p: T) => T)(latest.current) : next;
      latest.current = v;
      setValue(v);
      writeDevice(key, v).catch(() => {});
    },
    [key],
  );
  return { value, save, loaded };
}

/** Reads a persisted value once (for printable reports). */
export async function loadRecord<T>(key: string): Promise<T | undefined> {
  try {
    return (await readDevice<T>(key)) ?? undefined;
  } catch {
    return undefined;
  }
}

// ---- Persisted records ----------------------------------------------------------------------------------------

export type ActionUpdate = { at: number; who: string; text: string };
export type ActionStatus = "Open" | "In progress" | "Closed";
export type CorrectiveAction = {
  id: string;
  title: string;
  owner: string;
  approver: string;
  linkedEvent: string;
  due: string;
  status: ActionStatus;
  createdAt: number;
  updates: ActionUpdate[];
};
export const ACTIONS_KEY = "actions:v1";

export function nextActionId(list: CorrectiveAction[]): string {
  const max = list.reduce((m, a) => Math.max(m, Number(/^CA-(\d+)$/.exec(a.id)?.[1] ?? 0)), 0);
  return `CA-${String(max + 1).padStart(3, "0")}`;
}

export type HandoverSnapshot = {
  preparedAt: number;
  outgoing: string;
  receiving: string;
  accepted: boolean;
  acceptedAt: number | null;
  hazards: { id: string; severity: string; detail: string }[];
  gaps: { cam: string; state: string; since: string }[];
  pending: { id: string; title: string; owner: string; due: string }[];
  checklist: { text: string; done: boolean }[];
};
export const HANDOVER_KEY = "handover:v1";

export type RuleDraft = {
  id: string;
  severity: string;
  scope: string;
  trigger: string;
  confirmation: string;
  clearance: string;
  missing: string;
  savedAt: number;
};
export const RULES_KEY = "rules:v1";

export type StoredReviewRow = { eventId: string; classification: string; notes: string; savedAt: string; by: string };
/** Review decisions saved by reviewers (record keys "review:<eventId>"). */
export async function loadReviews(): Promise<StoredReviewRow[]> {
  try {
    const rows = await listDevice<{ classification: string; notes: string; savedAt: string; by: string }>("review:");
    return rows.map((r) => ({ eventId: r.key.slice("review:".length), ...r.value }));
  } catch {
    return [];
  }
}

// ---- Audit trail ---------------------------------------------------------------------------------------------

/** Audit actions shown on the audit trail: alarm and alert activity recorded by the host, plus changes made on these pages. */
export const AUDIT_ACTIONS = [
  "alarm.transition",
  "alert.respond",
  "alarm.simulate",
  "event.describe",
  "footage.request",
  "footage.save",
  "stream.open",
  "stream.close",
  "action.create",
  "action.update",
  "handover.prepare",
  "handover.accept",
  "rule.draft.save",
] as const;

export type HostAuditRow = { id: number; tsMs: number; source?: string; action?: string; target: string; detail: string };
export type AuditEntry = { id: number; tsMs: number; source: string; action: string; target: string; detail: string };

const ACTION_LABEL: Record<string, string> = {
  "alarm.transition": "Alarm transition",
  "alert.respond": "Alert response",
  "alarm.simulate": "Alarm simulation",
  "event.describe": "Plain-language summary",
  "footage.request": "Footage requested",
  "footage.save": "Footage saved",
  "stream.open": "Camera stream opened",
  "stream.close": "Camera stream closed",
  "action.create": "Action created",
  "action.update": "Action updated",
  "handover.prepare": "Handover prepared",
  "handover.accept": "Handover accepted",
  "rule.draft.save": "Rule draft saved",
};
export const auditActionLabel = (a: string) => ACTION_LABEL[a] ?? a;
export const auditActor = (source: string) =>
  source === "ui" ? "Operator (this device)" : source === "backend" ? "Host" : source ? `Host (${source})` : "Host";

/** Real audit rows for the given actions, newest first. Empty outside the desktop host. */
export async function loadAuditEntries(actions: readonly string[] = AUDIT_ACTIONS, limit = 200): Promise<AuditEntry[]> {
  if (!isNative()) return [];
  const parts = await Promise.all(
    actions.map((action) =>
      nativeCall<HostAuditRow[]>("audit.recent", { action, limit })
        .then((rows) => (Array.isArray(rows) ? rows.map((r) => ({ source: "", ...r, action: r.action ?? action }) as AuditEntry) : []))
        .catch(() => [] as AuditEntry[]),
    ),
  );
  return parts.flat().sort((a, b) => b.tsMs - a.tsMs || b.id - a.id).slice(0, limit);
}

/** Records a change made on these pages in the host's audit log (no-op outside the host). */
export const recordChange = audit;

// ---- Formatting ----------------------------------------------------------------------------------------------

export const stamp = (ms: number) =>
  new Date(ms).toLocaleString("en-GB", { day: "2-digit", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });
export const dayText = (ms: number) => new Date(ms).toLocaleDateString("en-GB", { day: "2-digit", month: "short", year: "numeric" });
export const timeText = (ms: number) => new Date(ms).toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });

/** Alerts that are real events (the quiet "Monitoring" state is not an event). */
export const isEventRow = (r: LiveAlertRow) => (r.phase ?? r.status) !== "Monitoring" && Number.isFinite(r.raisedMs);

/** Local ISO without zone: 2026-10-06T09:42:18 */
export function localIso(ms: number): string {
  const d = new Date(ms);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
