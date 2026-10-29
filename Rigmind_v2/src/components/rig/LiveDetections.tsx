import { useCallback, useEffect, useRef } from "react";
import {
  isDerrickman,
  isStale,
  MODELS,
  STALE_MS,
  useDetections,
  useOverlayPrefs,
  type DetFrame,
} from "@/lib/detections";
import { classColor, containRect, mapBox, mapPoint, type Rect } from "@/lib/detection-geometry";

const FONT = "600 11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";

const ALERT_RED = "#ff1f2d";
const ROI_COLOR = "#38bdf8";

function drawRoi(ctx: CanvasRenderingContext2D, frame: DetFrame, video: Rect) {
  if (!frame.roi) return;
  const r = mapBox(frame.roi, frame.w, frame.h, video);
  if (r.w < 1 || r.h < 1) return;
  ctx.lineWidth = 4;
  ctx.strokeStyle = "rgba(0,0,0,0.45)";
  ctx.setLineDash([10, 6]);
  ctx.strokeRect(r.x, r.y, r.w, r.h);
  ctx.lineWidth = 2;
  ctx.strokeStyle = ROI_COLOR;
  ctx.strokeRect(r.x, r.y, r.w, r.h);
  ctx.setLineDash([]);

}

export const PLATFORM_COLOR = "#00ff00";

/** Platform like IDSS: green 2px, the box rectangle first, then each contour as a closed polyline. */
export function drawPlatform(ctx: CanvasRenderingContext2D, frame: DetFrame, video: Rect) {
  const pl = frame.platform;
  if (!pl) return;
  ctx.save();
  ctx.setLineDash([]);
  ctx.lineWidth = 2;
  ctx.strokeStyle = PLATFORM_COLOR;
  const r = mapBox(pl.box, frame.w, frame.h, video);
  ctx.strokeRect(r.x, r.y, r.w, r.h);
  for (const contour of pl.outline) {
    if (contour.length < 2) continue;
    ctx.beginPath();
    contour.forEach((pt, i) => {
      const [x, y] = mapPoint(pt, frame.w, frame.h, video);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.closePath();
    ctx.stroke();
  }
  ctx.restore();
}

export function drawFrame(ctx: CanvasRenderingContext2D, frame: DetFrame, video: Rect) {
  ctx.font = FONT;
  ctx.textBaseline = "middle";
  if (frame.model === "idss") drawPlatform(ctx, frame, video);
  if (frame.model === "idss" && frame.roi) drawRoi(ctx, frame, video);
  for (const it of frame.items) {
    const r = mapBox(it.box, frame.w, frame.h, video);
    if (r.w < 1 || r.h < 1) continue;
    const breach = !it.inRoi && isDerrickman(it.label);
    const isPlatform = it.label.toLowerCase().includes("platform");
    const color = breach ? ALERT_RED : isPlatform ? PLATFORM_COLOR : classColor(frame.model, it.cls);
    // Mask under the box and label: translucent fill (not for the platform) plus a 2px outline.
    if (it.polygon && it.polygon.length >= 3) {
      ctx.beginPath();
      it.polygon.forEach((pt, i) => {
        const [x, y] = mapPoint(pt, frame.w, frame.h, video);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.closePath();
      if (!isPlatform) {
        ctx.globalAlpha = 0.25;
        ctx.fillStyle = color;
        ctx.fill();
        ctx.globalAlpha = 1;
      }
      ctx.setLineDash(it.inRoi ? [] : [7, 4]);
      ctx.lineWidth = 2;
      ctx.strokeStyle = color;
      ctx.stroke();
      ctx.setLineDash([]);
    }
    // Dark under-stroke keeps the box readable on bright video.
    ctx.lineWidth = breach ? 6 : 4;
    ctx.strokeStyle = "rgba(0,0,0,0.45)";
    ctx.setLineDash(it.inRoi ? [] : [7, 4]);
    ctx.strokeRect(r.x, r.y, r.w, r.h);
    ctx.lineWidth = breach ? 4 : 2;
    ctx.strokeStyle = color;
    ctx.strokeRect(r.x, r.y, r.w, r.h);
    ctx.setLineDash([]);

    // No text labels on the frame (user decision): class and boundary status are carried by colour and line style.
  }
}

/**
 * Canvas over a playing <video> (object-contain) that draws the model detections.
 * Place it inside the same relative parent as the video; it ignores pointer events.
 * Stale detections vanish with a hard cut (no fades, so reduced-motion needs no special case).
 */
export function LiveDetections({
  camId,
  video,
}: {
  camId: string;
  video: React.RefObject<HTMLVideoElement | null>;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const det = useDetections(camId);
  const prefs = useOverlayPrefs();
  const latest = useRef({ det, prefs });
  latest.current = { det, prefs };
  const raf = useRef(0);
  const expiry = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const drawRef = useRef<() => void>(() => {});

  const schedule = useCallback(() => {
    if (!raf.current) raf.current = requestAnimationFrame(() => drawRef.current());
  }, []);

  drawRef.current = () => {
    raf.current = 0;
    const canvas = canvasRef.current;
    const v = video.current;
    if (!canvas || !v) return;
    const cssW = canvas.clientWidth;
    const cssH = canvas.clientHeight;
    const dpr = window.devicePixelRatio || 1;
    const pw = Math.round(cssW * dpr);
    const ph = Math.round(cssH * dpr);
    if (canvas.width !== pw || canvas.height !== ph) {
      canvas.width = pw;
      canvas.height = ph;
    }
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);
    // The pipeline already drew the detections into the video frames (frame-exact by construction): draw nothing here.
    if (MODELS.some((m) => latest.current.det.stats[m]?.burnedIn)) return;
    const rect = containRect(cssW, cssH, v.videoWidth, v.videoHeight);
    if (rect.w === 0) return;
    const now = Date.now();
    let nextExpiry = Infinity;
    for (const model of MODELS) {
      const frame = latest.current.det.frames[model];
      if (!frame || !latest.current.prefs[model] || isStale(frame, now)) continue;
      drawFrame(ctx, frame, rect);
      nextExpiry = Math.min(nextExpiry, frame.receivedAt + STALE_MS[model] - now);
    }
    clearTimeout(expiry.current);
    if (Number.isFinite(nextExpiry)) expiry.current = setTimeout(schedule, nextExpiry + 20);
  };

  // New events or toggles.
  useEffect(schedule, [det, prefs, schedule]);

  // Resize, and the video's real size becoming known.
  useEffect(() => {
    const canvas = canvasRef.current;
    const v = video.current;
    if (!canvas) return;
    const ro = typeof ResizeObserver !== "undefined" ? new ResizeObserver(schedule) : undefined;
    ro?.observe(canvas);
    v?.addEventListener("loadedmetadata", schedule);
    v?.addEventListener("resize", schedule);
    return () => {
      ro?.disconnect();
      v?.removeEventListener("loadedmetadata", schedule);
      v?.removeEventListener("resize", schedule);
      if (raf.current) cancelAnimationFrame(raf.current);
      raf.current = 0;
      clearTimeout(expiry.current);
    };
  }, [video, schedule]);

  return (
    <canvas
      ref={canvasRef}
      aria-hidden="true"
      className="pointer-events-none absolute inset-0 z-[5] h-full w-full"
    />
  );
}
