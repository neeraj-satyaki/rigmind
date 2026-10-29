// Real (non-sample) state for the pages: cameras the user configured, events the host recorded, the signed-in operator.
// Used when demoDataEnabled() is false. Nothing here invents values: an unknown thing is reported as empty/unknown.
import { useEffect, useState } from "react";
import { readDevice, writeDevice } from "@/lib/device-db";
import { DEFAULT_STREAM_SOURCES } from "@/lib/default-sources";
import { isNative, nativeCall } from "@/lib/native-bridge";
import { useCameras } from "@/lib/camera-status";
import type { Severity } from "@/data/mock";

/** The person using this device. The host has no accounts yet, so this is a neutral label, never a sample name. */
export const localOperator = { name: "Operator", role: "This device", initials: "OP" };

/** camera id -> rtsp URI saved in device storage. With seed, a first run saves and returns the default source. */
export function useStreamSources(seed = false, enabled = true): Record<string, string> {
  const [sources, setSources] = useState<Record<string, string>>({});
  useEffect(() => {
    if (!enabled || !isNative()) return;
    readDevice<Record<string, string>>("stream.sources")
      .then((saved) => {
        if (saved && Object.keys(saved).length) setSources(saved);
        else if (seed) {
          // First run: open the default camera and remember it, so the host reconnects it at every start.
          setSources(DEFAULT_STREAM_SOURCES);
          writeDevice("stream.sources", DEFAULT_STREAM_SOURCES).catch(() => {});
        }
      })
      .catch(() => {});
  }, [seed, enabled]);
  return sources;
}

export type RealCamera = { id: string; area: string };

/** Cameras that exist for real: saved stream sources plus any camera the host has reported. Sorted by id. */
export function useRealCameras(sources: Record<string, string>): RealCamera[] {
  const status = useCameras();
  const ids = [...new Set([...Object.keys(sources), ...Object.keys(status)])].sort();
  return ids.map((id) => ({ id, area: "Camera stream" }));
}

// One row of the host's events.recent (alarm_events): a state change of one camera.
export type Transition = {
  id: number;
  timestampMs: number;
  from: string;
  to: string;
  camId: string;
  alertId: string;
  condition: string;
  place: string;
  severity: string;
};

export type RealEvent = {
  id: string;
  camId: string;
  place: string;
  condition: string;
  severity: Severity;
  startMs: number;
  endMs: number | null;
  cleared: boolean;
  steps: { ms: number; from: string; to: string }[];
};

const SEV: Record<string, Severity> = {
  critical: "Critical", red: "Critical", warning: "Warning", orange: "Warning", advisory: "Advisory", yellow: "Advisory",
};
const RANK: Record<Severity, number> = { Advisory: 0, Warning: 1, Critical: 2 };
const sevOf = (t: Transition): Severity => SEV[t.severity.toLowerCase()] ?? SEV[t.to.toLowerCase()] ?? "Advisory";

/** Groups host alarm transitions (newest first) into one event per alarm episode (alertId), newest episode first. */
export function eventsFromTransitions(rows: Transition[]): RealEvent[] {
  const byId = new Map<string, RealEvent>();
  for (const t of [...rows].sort((a, b) => a.timestampMs - b.timestampMs || a.id - b.id)) {
    const id = t.alertId || `T-${t.camId}-${t.id}`;
    let e = byId.get(id);
    if (!e) {
      e = { id, camId: t.camId, place: t.place, condition: t.condition, severity: "Advisory", startMs: t.timestampMs, endMs: null, cleared: false, steps: [] };
      byId.set(id, e);
    }
    e.steps.push({ ms: t.timestampMs, from: t.from, to: t.to });
    if (t.to === "None") {
      e.cleared = true;
      e.endMs = t.timestampMs;
    } else {
      if (t.condition) e.condition = t.condition;
      if (t.place) e.place = t.place;
      const s = sevOf(t);
      if (RANK[s] > RANK[e.severity]) e.severity = s;
    }
  }
  return [...byId.values()].sort((a, b) => b.startMs - a.startMs);
}

export type RealEvents = { events: RealEvent[]; loaded: boolean; error: string };

/** Alarm episodes recorded by the host (events.recent). Empty outside the desktop host. */
export function useRealEvents(limit = 500): RealEvents {
  const [state, setState] = useState<RealEvents>({ events: [], loaded: !isNative(), error: "" });
  useEffect(() => {
    if (!isNative()) return;
    let stopped = false;
    const load = () =>
      nativeCall<Transition[]>("events.recent", { limit })
        .then((rows) => !stopped && setState({ events: eventsFromTransitions(Array.isArray(rows) ? rows : []), loaded: true, error: "" }))
        .catch((e) => !stopped && setState((s) => ({ ...s, loaded: true, error: String(e) })));
    load();
    const timer = window.setInterval(load, 5000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [limit]);
  return state;
}

/** hh:mm:ss in the viewer's local time. */
export const clock = (ms: number) =>
  new Date(ms).toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });

/** "29 Oct 2026". */
export const dayLabel = (ms: number) =>
  new Date(ms).toLocaleDateString("en-GB", { day: "2-digit", month: "short", year: "numeric" });
