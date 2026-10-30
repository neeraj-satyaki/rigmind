// Model detections (PPE, IDSS) per camera, fed by the host (rigmind-pipeline) over the native bridge.
// In a plain browser there is no host: the store stays empty and nothing is drawn.
import { useSyncExternalStore } from "react";
import { isNative, onNativeEvent, type NativeEvent } from "@/lib/native-bridge";

export type ModelKey = "ppe" | "idss";
export const MODELS: readonly ModelKey[] = ["ppe", "idss"];

/** Detections older than this (ms since received) are not drawn. */
export const STALE_MS: Record<ModelKey, number> = { ppe: 1500, idss: 4000 };

export type DetItem = {
  cls: number;
  label: string;
  conf: number;
  box: [number, number, number, number];
  /** False when the box is outside the ROI (true when there is no ROI). */
  inRoi: boolean;
  /** Optional mask outline in frame pixels (closed path). */
  polygon?: [number, number][] | undefined;
  /** PPE tracker id (ByteTrack) when the pipeline tracks the boxes. */
  trackId?: number | undefined;
  /** True when the box is the tracker's prediction (no detection on this frame). */
  held?: boolean | undefined;
};

export type Platform = {
  box: [number, number, number, number];
  /** Closed contours in frame pixels; empty when the mask was empty. */
  outline: [number, number][][];
};

export type DetFrame = {
  model: ModelKey;
  /** Frame size the boxes are expressed in. */
  w: number;
  h: number;
  /** Inference time of this frame, ms. */
  ms: number;
  /** Host timestamp (unix ms). */
  ts: number;
  /** Local receive time (ms epoch); staleness is measured from this. */
  receivedAt: number;
  /** Boundary used for this frame, in frame pixels, or null. */
  roi: [number, number, number, number] | null;
  /** IDSS platform box and mask contours, or null. */
  platform: Platform | null;
  items: DetItem[];
  /** RTP timestamp (90 kHz, uint32) of the video frame these detections belong to, when sent. */
  rtp?: number | undefined;
  /** Presentation time of that frame in ms, when sent. */
  ptsMs?: number | undefined;
};

/** How long detection frames are kept for frame-exact sync. */
export const HISTORY_MS = 2000;

export type AlarmState = "None" | "Yellow" | "Orange" | "Red";
export type AlarmInfo = {
  state: AlarmState;
  severity: string;
  condition: string;
  place: string;
  receivedAt: number;
};

/** The derrickman class: any label containing "derrick". */
export function isDerrickman(label: string): boolean {
  return label.toLowerCase().includes("derrick");
}

/** PPE compliance of the people in view: items NOT DETECTED (never "not worn": the camera may not see them). */
export type PpeInfo = { state: "missing" | "ok"; missing: string[]; persons: number; receivedAt: number };

export type InferStats = {
  model: ModelKey;
  /** The pipeline draws the detections into the video frames itself: the browser canvas must stay empty. */
  burnedIn: boolean;
  fps: number;
  avgMs: number;
  conf: number;
  iou: number;
  receivedAt: number;
};

export type CamDetections = {
  frames: Partial<Record<ModelKey, DetFrame>>;
  stats: Partial<Record<ModelKey, InferStats>>;
  /** Latest alarm state; undefined until an alarm event arrives, cleared to undefined on state None. */
  alarm?: AlarmInfo | undefined;
  /** Latest PPE compliance state; undefined until a ppe event arrives. */
  ppe?: PpeInfo | undefined;
  /** Recent frames per model, oldest first, for frame-exact sync. */
  history: Partial<Record<ModelKey, DetFrame[]>>;
};

type Listener = () => void;

let byCam: Record<string, CamDetections> = {};
const listeners = new Set<Listener>();
let started = false;
let unsubscribe: (() => void) | undefined;

const EMPTY: CamDetections = { frames: {}, stats: {}, history: {} };

function isModel(v: unknown): v is ModelKey {
  return v === "ppe" || v === "idss";
}

function num(v: unknown, fallback = 0): number {
  const n = Number(v);
  return Number.isFinite(n) ? n : fallback;
}

function emit() {
  byCam = { ...byCam };
  listeners.forEach((l) => l());
}

/** Parses a mask polygon; undefined when it has fewer than 3 usable points. */
export function parsePolygon(raw: unknown): [number, number][] | undefined {
  if (!Array.isArray(raw)) return undefined;
  const pts: [number, number][] = [];
  for (const pt of raw) {
    if (Array.isArray(pt) && pt.length >= 2) pts.push([num(pt[0]), num(pt[1])]);
  }
  return pts.length >= 3 ? pts : undefined;
}

/** Parses the platform field; null for anything without a usable box. */
export function parsePlatform(raw: unknown): Platform | null {
  if (!raw || typeof raw !== "object") return null;
  const p = raw as { box?: unknown; outline?: unknown };
  if (!Array.isArray(p.box) || p.box.length < 4) return null;
  const b = p.box.map((v) => num(v));
  const outline: [number, number][][] = [];
  if (Array.isArray(p.outline)) {
    for (const c of p.outline) {
      if (!Array.isArray(c)) continue;
      const pts: [number, number][] = [];
      for (const pt of c) {
        if (Array.isArray(pt) && pt.length >= 2) pts.push([num(pt[0]), num(pt[1])]);
      }
      if (pts.length >= 2) outline.push(pts);
    }
  }
  return { box: [b[0]!, b[1]!, b[2]!, b[3]!], outline };
}

/** True when the frame is too old to draw. */
export function isStale(frame: Pick<DetFrame, "model" | "receivedAt">, now: number): boolean {
  return now - frame.receivedAt > STALE_MS[frame.model];
}

/** Applies one host event to the store. Exported for tests; start() wires it to the bridge. */
export function ingest(msg: NativeEvent, now: number = Date.now()): void {
  const cam = typeof msg.cam === "string" ? msg.cam : "";
  const model = msg["model"];
  if (!cam) return;
  const current = byCam[cam] ?? EMPTY;
  if (msg.ev === "alarm") {
    const st = msg["state"];
    if (st !== "None" && st !== "Yellow" && st !== "Orange" && st !== "Red") return;
    const alarm: AlarmInfo | undefined =
      st === "None"
        ? undefined
        : {
            state: st,
            severity: String(msg["severity"] ?? ""),
            condition: String(msg["condition"] ?? ""),
            place: String(msg["place"] ?? ""),
            receivedAt: now,
          };
    byCam[cam] = { ...current, alarm };
    emit();
    return;
  }
  if (msg.ev === "ppe") {
    const st = msg["state"];
    if (st !== "missing" && st !== "ok") return;
    const missing = Array.isArray(msg["missing"]) ? (msg["missing"] as unknown[]).filter((m): m is string => typeof m === "string") : [];
    byCam[cam] = { ...current, ppe: { state: st, missing, persons: num(msg["persons"]), receivedAt: now } };
    emit();
    return;
  }
  if (!isModel(model)) return;
  if (msg.ev === "detections") {
    const raw = Array.isArray(msg["items"]) ? (msg["items"] as unknown[]) : [];
    const items: DetItem[] = [];
    for (const r of raw) {
      const it = (r ?? {}) as {
        cls?: unknown;
        label?: unknown;
        conf?: unknown;
        box?: unknown;
        in_roi?: unknown;
        polygon?: unknown;
        track_id?: unknown;
        held?: unknown;
      };
      if (!Array.isArray(it.box) || it.box.length < 4) continue;
      const b = it.box.map((v) => num(v));
      items.push({
        cls: num(it.cls),
        label: typeof it.label === "string" ? it.label : String(it.cls ?? ""),
        conf: num(it.conf),
        box: [b[0]!, b[1]!, b[2]!, b[3]!],
        inRoi: it.in_roi !== false,
        polygon: parsePolygon(it.polygon),
        trackId: it.track_id === undefined ? undefined : num(it.track_id),
        held: it.held === true ? true : undefined,
      });
    }
    const rawRoi = msg["roi"];
    const roi =
      Array.isArray(rawRoi) && rawRoi.length >= 4
        ? ([num(rawRoi[0]), num(rawRoi[1]), num(rawRoi[2]), num(rawRoi[3])] as [
            number,
            number,
            number,
            number,
          ])
        : null;
    const frame: DetFrame = {
      platform: parsePlatform(msg["platform"]),
      model,
      w: num(msg["w"], 1280) || 1280,
      h: num(msg["h"], 720) || 720,
      ms: num(msg["ms"]),
      ts: num(msg["ts"], now),
      receivedAt: now,
      rtp: msg["rtp"] === undefined || msg["rtp"] === null ? undefined : num(msg["rtp"]) >>> 0,
      ptsMs: msg["pts_ms"] === undefined ? undefined : num(msg["pts_ms"]),
      roi,
      items,
    };
    const history = [...(current.history[model] ?? []), frame].filter(
      (f) => now - f.receivedAt <= HISTORY_MS,
    );
    byCam[cam] = {
      ...current,
      frames: { ...current.frames, [model]: frame },
      history: { ...current.history, [model]: history },
    };
    emit();
  } else if (msg.ev === "inference") {
    const stats: InferStats = {
      model,
      burnedIn: msg["burned_in"] === true,
      fps: num(msg["fps"]),
      avgMs: num(msg["avg_ms"]),
      conf: num(msg["conf"]),
      iou: num(msg["iou"]),
      receivedAt: now,
    };
    byCam[cam] = { ...current, stats: { ...current.stats, [model]: stats } };
    emit();
  }
}

function start() {
  if (started || !isNative()) return;
  started = true;
  unsubscribe = onNativeEvent((msg) => ingest(msg));
}

/** Test helper: the stored state of one camera. */
export const __peek = (cam: string): CamDetections | undefined => byCam[cam];

/** Test helper: forget everything and detach from the bridge. */
export function resetDetections() {
  unsubscribe?.();
  unsubscribe = undefined;
  started = false;
  byCam = {};
  listeners.forEach((l) => l());
}

function subscribe(listener: Listener) {
  start();
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

const snapshot = () => byCam;
const serverSnapshot = () => ({}) as Record<string, CamDetections>;

/** Latest detections and inference numbers for one camera. Re-renders on every event for any camera. */
export function useDetections(camId: string | undefined): CamDetections {
  const all = useSyncExternalStore(subscribe, snapshot, serverSnapshot);
  return (camId && all[camId]) || EMPTY;
}

// ---- overlay toggles (per device, remembered) ----

export type OverlayPrefs = Record<ModelKey, boolean>;
const PREFS_KEY = "rigmind.overlay.prefs";
let prefs: OverlayPrefs | undefined;
const prefListeners = new Set<Listener>();

function loadPrefs(): OverlayPrefs {
  try {
    const raw = globalThis.localStorage?.getItem(PREFS_KEY);
    if (raw) {
      const p = JSON.parse(raw) as Partial<OverlayPrefs>;
      return { ppe: p.ppe !== false, idss: p.idss !== false };
    }
  } catch {
    /* storage blocked: defaults */
  }
  return { ppe: true, idss: true };
}

export function setOverlay(model: ModelKey, on: boolean) {
  prefs = { ...(prefs ?? loadPrefs()), [model]: on };
  try {
    globalThis.localStorage?.setItem(PREFS_KEY, JSON.stringify(prefs));
  } catch {
    /* ignore */
  }
  prefListeners.forEach((l) => l());
}

/** The saved PPE/IDSS overlay choice, read outside React (to restore it after a raw preview). */
export const currentOverlayPrefs = (): OverlayPrefs => (prefs ??= loadPrefs());

const prefSnapshot = () => (prefs ??= loadPrefs());
const SERVER_PREFS: OverlayPrefs = { ppe: true, idss: true };
const prefServer = () => SERVER_PREFS;
const prefSubscribe = (l: Listener) => {
  prefListeners.add(l);
  return () => {
    prefListeners.delete(l);
  };
};

export function useOverlayPrefs(): OverlayPrefs {
  return useSyncExternalStore(prefSubscribe, prefSnapshot, prefServer);
}
