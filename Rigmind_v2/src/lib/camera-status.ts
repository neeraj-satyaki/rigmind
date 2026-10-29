// Live camera state, fed by the host (rigmind-pipeline) over the native bridge.
// One store for the Live page, the Health page and Analytics. In a plain browser there is no host: the store stays empty.
import { useSyncExternalStore } from "react";
import { isNative, nativeCall, onNativeEvent } from "@/lib/native-bridge";

export type CameraState = "connecting" | "online" | "offline";

export type CameraStatus = {
  id: string;
  state: CameraState;
  reason: string;
  width: number;
  height: number;
  /** Frame rate of the camera stream. */
  streamFps: number;
  /** Measured at the pipeline, every 2 s. */
  fps: number;
  kbps: number;
  reconnects: number;
  /** Consecutive failed connection attempts (0 while online). */
  attempt: number;
  /** Seconds the camera has been online since its last connect; 0 while not online. */
  onlineSince: number;
  /** ms epoch of the last status change. */
  changedAt: number;
  /** Total seconds spent online since the app started (for availability). */
  onlineMs: number;
  /** Total seconds observed since the camera was first opened. */
  observedMs: number;
};

type Listener = () => void;

let cameras: Record<string, CameraStatus> = {};
const listeners = new Set<Listener>();
let started = false;
let tick: ReturnType<typeof setInterval> | undefined;

function emit() {
  cameras = { ...cameras };
  listeners.forEach((l) => l());
}

function blank(id: string): CameraStatus {
  const now = Date.now();
  return {
    id, state: "offline", reason: "", width: 0, height: 0, streamFps: 0, fps: 0, kbps: 0, reconnects: 0, attempt: 0,
    onlineSince: 0, changedAt: now, onlineMs: 0, observedMs: 0,
  };
}

// Folds elapsed time into the availability counters before the state changes.
function accrue(cam: CameraStatus, now: number) {
  const dt = now - cam.changedAt;
  if (dt > 0) {
    cam.observedMs += dt;
    if (cam.state === "online") cam.onlineMs += dt;
  }
  cam.changedAt = now;
}

function start() {
  if (started || !isNative()) return;
  started = true;
  onNativeEvent((msg) => {
    const id = typeof msg.cam === "string" ? msg.cam : "";
    if (!id) return;
    const now = Date.now();
    if (msg.ev === "status") {
      const cam = { ...(cameras[id] ?? blank(id)) };
      accrue(cam, now);
      const state = msg["state"] as CameraState;
      cam.state = state;
      cam.reason = String(msg["reason"] ?? "");
      cam.reconnects = Number(msg["reconnects"] ?? 0);
      cam.attempt = Number(msg["attempt"] ?? 0);
      if (state === "online") {
        cam.width = Number(msg["width"] ?? 0);
        cam.height = Number(msg["height"] ?? 0);
        cam.streamFps = Number(msg["fps"] ?? 0);
        cam.onlineSince = now;
      } else {
        cam.onlineSince = 0;
        cam.fps = 0;
        cam.kbps = 0;
      }
      cameras[id] = cam;
      emit();
    } else if (msg.ev === "stats") {
      const cam = { ...(cameras[id] ?? blank(id)) };
      cam.fps = Number(msg["fps"] ?? 0);
      cam.kbps = Number(msg["kbps"] ?? 0);
      cam.reconnects = Number(msg["reconnects"] ?? cam.reconnects);
      cameras[id] = cam;
      emit();
    }
  });
  // Availability counters advance while a camera stays in one state.
  tick = setInterval(() => {
    const now = Date.now();
    let any = false;
    for (const [id, current] of Object.entries(cameras)) {
      const cam = { ...current };
      accrue(cam, now);
      cameras[id] = cam;
      any = true;
    }
    if (any) emit();
  }, 5000);
  // The page may have loaded after the pipeline reported: ask for the current state of every camera.
  nativeCall("stream.status").catch(() => {});
}

function subscribe(listener: Listener) {
  start();
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

const snapshot = () => cameras;
const serverSnapshot = () => ({}) as Record<string, CameraStatus>;

/** All cameras the host has reported. Re-renders on every status or stats event. */
export function useCameras(): Record<string, CameraStatus> {
  return useSyncExternalStore(subscribe, snapshot, serverSnapshot);
}

/** One camera, or undefined while the host has not reported it. */
export function useCamera(id: string | undefined): CameraStatus | undefined {
  const all = useCameras();
  return id ? all[id] : undefined;
}

/** Share of observed time a camera was online, 0..1 (1 when nothing has been observed yet). */
export function availability(cam: CameraStatus | undefined): number {
  if (!cam || cam.observedMs <= 0) return cam?.state === "online" ? 1 : 0;
  return cam.onlineMs / cam.observedMs;
}

export function stopCameraStatusTimer() {
  if (tick) clearInterval(tick);
  tick = undefined;
}
