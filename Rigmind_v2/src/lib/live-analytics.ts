// Live layer for Analytics and System health. Demo numbers stay as the baseline; live data from the host
// (alerts.feed, camera status) is merged on top and labelled "Live" by the screens.
import { useEffect, useState } from "react";
import { isNative, nativeCall } from "@/lib/native-bridge";
import type { CameraStatus } from "@/lib/camera-status";

export type LiveAlertRow = {
  id: string;
  severity: "Critical" | "Warning" | "Advisory" | string;
  condition: string;
  place: string;
  status: string;
  /** Lifecycle of an alarm episode (Monitoring | Escalated | Cleared), apart from a person's response. */
  phase?: string;
  owner: string;
  raisedMs: number;
};

export type SeverityCounts = { critical: number; warning: number; advisory: number };

const POLL_MS = 3000;

/** Raised alerts from the host, polled while native. Empty array in a plain browser. */
export function useLiveAlerts(): LiveAlertRow[] {
  const [rows, setRows] = useState<LiveAlertRow[]>([]);
  useEffect(() => {
    if (!isNative()) return;
    let stopped = false;
    const poll = () =>
      nativeCall<LiveAlertRow[]>("alerts.feed", { limit: 500 })
        .then((r) => {
          if (!stopped && Array.isArray(r)) setRows(r);
        })
        .catch(() => {});
    poll();
    const timer = window.setInterval(poll, POLL_MS);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, []);
  return rows;
}

/** Local midnight (ms epoch) of the day containing `now`. */
export function startOfDay(now: number): number {
  const d = new Date(now);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

/** Alerts raised today (local time). */
export function todaysAlerts(rows: LiveAlertRow[], now: number): LiveAlertRow[] {
  const from = startOfDay(now);
  return rows.filter(
    (r) => Number.isFinite(r.raisedMs) && r.raisedMs >= from && r.raisedMs <= now + 60_000,
  );
}

export function countSeverity(rows: LiveAlertRow[]): SeverityCounts {
  const c: SeverityCounts = { critical: 0, warning: 0, advisory: 0 };
  for (const r of rows) {
    // A Monitoring item is the quiet "app is watching" state, not an event. An escalated item counts once, at its current severity.
    if ((r.phase ?? r.status) === "Monitoring") continue;
    if (r.severity === "Critical") c.critical++;
    else if (r.severity === "Warning") c.warning++;
    else if (r.severity === "Advisory") c.advisory++;
  }
  return c;
}

export function addCounts(a: SeverityCounts, b: SeverityCounts): SeverityCounts {
  return {
    critical: a.critical + b.critical,
    warning: a.warning + b.warning,
    advisory: a.advisory + b.advisory,
  };
}

export const totalOf = (c: SeverityCounts) => c.critical + c.warning + c.advisory;

/** Puts live counts on top of the demo row for the current (last) day. Other days are returned untouched. */
export function mergeTrend<T extends SeverityCounts & { day: string }>(
  demo: T[],
  live: SeverityCounts,
): T[] {
  if (demo.length === 0 || totalOf(live) === 0) return demo;
  const last = demo.length - 1;
  return demo.map((d, i) => (i === last ? { ...d, ...addCounts(d, live) } : d));
}

/** Seconds for acknowledged alerts are not reported by the host yet; returns null until they are. */
export function ackSeconds(rows: { ackSeconds?: number }[]): number[] {
  return rows.map((r) => r.ackSeconds).filter((v): v is number => typeof v === "number" && v >= 0);
}

export function percentile(values: number[], p: number): number | null {
  if (values.length === 0) return null;
  const s = [...values].sort((a, b) => a - b);
  const idx = Math.min(s.length - 1, Math.max(0, Math.ceil((p / 100) * s.length) - 1));
  return s[idx] ?? null;
}

export type CameraSummary = {
  total: number;
  online: number;
  connecting: number;
  offline: number;
  avgFps: number;
  totalKbps: number;
  reconnects: number;
  /** 0..1, mean availability across reporting cameras; null when none reported. */
  availability: number | null;
  /** Longest current continuous online time, seconds. */
  longestOnlineSec: number;
};

/** Summary over the host-reported cameras. `total` is at least the number of ids in `knownIds`. */
export function summarizeCameras(
  cams: Record<string, CameraStatus>,
  knownIds: string[] = [],
  now: number = Date.now(),
): CameraSummary {
  const list = Object.values(cams);
  const ids = new Set([...knownIds, ...Object.keys(cams)]);
  const online = list.filter((c) => c.state === "online");
  const connecting = list.filter((c) => c.state === "connecting").length;
  const avails = list.map((c) =>
    c.observedMs > 0 ? c.onlineMs / c.observedMs : c.state === "online" ? 1 : 0,
  );
  return {
    total: ids.size,
    online: online.length,
    connecting,
    offline: ids.size - online.length - connecting,
    avgFps: online.length ? online.reduce((s, c) => s + c.fps, 0) / online.length : 0,
    totalKbps: online.reduce((s, c) => s + c.kbps, 0),
    reconnects: list.reduce((s, c) => s + c.reconnects, 0),
    availability: avails.length ? avails.reduce((s, v) => s + v, 0) / avails.length : null,
    longestOnlineSec: online.reduce(
      (m, c) => Math.max(m, c.onlineSince ? Math.max(0, (now - c.onlineSince) / 1000) : 0),
      0,
    ),
  };
}

export function formatDuration(sec: number): string {
  const s = Math.max(0, Math.round(sec));
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} m ${String(s % 60).padStart(2, "0")} s`;
  return `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, "0")} m`;
}

export function formatAgo(ms: number, now: number = Date.now()): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  return s < 2 ? "just now" : `${formatDuration(s)} ago`;
}

export type HealthRow = {
  name: string;
  status: "Online" | "Offline" | "Connecting";
  tone: "success" | "critical" | "warning";
  seen: string;
  live: boolean;
};

/** Camera rows for System health. Native: real state for configured cameras, Offline for the rest. */
export function cameraHealthRows(
  demoIds: string[],
  cams: Record<string, CameraStatus>,
  now: number = Date.now(),
): HealthRow[] {
  const ids = [...demoIds, ...Object.keys(cams).filter((id) => !demoIds.includes(id))];
  return ids.map((id) => {
    const c = cams[id];
    if (!c)
      return { name: id, status: "Offline", tone: "critical", seen: "Not configured", live: true };
    if (c.state === "online")
      return {
        name: id,
        status: "Online",
        tone: "success",
        seen: `Online ${formatDuration(c.onlineSince ? (now - c.onlineSince) / 1000 : 0)} · changed ${formatAgo(c.changedAt, now)}`,
        live: true,
      };
    if (c.state === "connecting")
      return {
        name: id,
        status: "Connecting",
        tone: "warning",
        seen: `Since ${formatAgo(c.changedAt, now)}`,
        live: true,
      };
    return {
      name: id,
      status: "Offline",
      tone: "critical",
      seen: `Since ${formatAgo(c.changedAt, now)}`,
      live: true,
    };
  });
}

// ---- Real history built only from raised alerts (no sample baseline) ----------------------------------------------

const DAY_MS = 86_400_000;
const isEvent = (r: LiveAlertRow) => (r.phase ?? r.status) !== "Monitoring" && Number.isFinite(r.raisedMs);

/** "06 Oct" label of the local day containing `ms`. */
export function dayLabelOf(ms: number): string {
  return new Date(ms).toLocaleDateString("en-GB", { day: "2-digit", month: "short" });
}

/** Severity counts for each of the last `days` local days, oldest first, the last entry being today. */
export function realTrend(rows: LiveAlertRow[], now: number, days = 7): (SeverityCounts & { day: string })[] {
  const today = startOfDay(now);
  return Array.from({ length: days }, (_, i) => {
    const from = today - (days - 1 - i) * DAY_MS;
    const d = new Date(from);
    // Local-day bounds (a DST day is not exactly 24 h long).
    const next = new Date(d.getFullYear(), d.getMonth(), d.getDate() + 1).getTime();
    const counts = countSeverity(rows.filter((r) => isEvent(r) && r.raisedMs >= from && r.raisedMs < next));
    return { day: dayLabelOf(from), ...counts };
  });
}

/** Alert counts per weekday (last 7 days, oldest first) and two-hour block. */
export function realHeatmap(rows: LiveAlertRow[], now: number): { day: string; values: number[] }[] {
  const today = startOfDay(now);
  return Array.from({ length: 7 }, (_, i) => {
    const from = today - (6 - i) * DAY_MS;
    const d = new Date(from);
    const next = new Date(d.getFullYear(), d.getMonth(), d.getDate() + 1).getTime();
    const values = Array.from({ length: 12 }, () => 0);
    for (const r of rows) {
      if (!isEvent(r) || r.raisedMs < from || r.raisedMs >= next) continue;
      const b = Math.min(11, Math.floor(new Date(r.raisedMs).getHours() / 2));
      values[b] = (values[b] ?? 0) + 1;
    }
    return { day: d.toLocaleDateString("en-GB", { weekday: "short" }), values };
  });
}

/** Events grouped by condition, most frequent first. */
export function eventsByCondition(rows: LiveAlertRow[], from = 0): { type: string; count: number }[] {
  const m = new Map<string, number>();
  for (const r of rows) {
    if (!isEvent(r) || r.raisedMs < from) continue;
    const k = r.condition || "Unspecified";
    m.set(k, (m.get(k) ?? 0) + 1);
  }
  return [...m.entries()].map(([type, count]) => ({ type, count })).sort((a, b) => b.count - a.count);
}

/** Heat level 0..5 for a cell, scaled to the busiest cell. */
export function heatLevel(value: number, max: number): number {
  if (value <= 0 || max <= 0) return 0;
  return Math.min(5, Math.max(1, Math.ceil((value / max) * 5)));
}
