import type { ModelKey } from "@/lib/detections";

export type Rect = { x: number; y: number; w: number; h: number };

/** The rectangle an object-contain video occupies inside its element (letterbox / pillarbox). */
export function containRect(boxW: number, boxH: number, srcW: number, srcH: number): Rect {
  if (boxW <= 0 || boxH <= 0 || srcW <= 0 || srcH <= 0) return { x: 0, y: 0, w: 0, h: 0 };
  const scale = Math.min(boxW / srcW, boxH / srcH);
  const w = srcW * scale;
  const h = srcH * scale;
  return { x: (boxW - w) / 2, y: (boxH - h) / 2, w, h };
}

/** Maps a box in frame pixels (frameW x frameH) into the displayed video rectangle, clamped to the frame. */
export function mapBox(
  box: readonly [number, number, number, number],
  frameW: number,
  frameH: number,
  video: Rect,
): Rect {
  const sx = video.w / frameW;
  const sy = video.h / frameH;
  const clampX = (v: number) => Math.max(0, Math.min(frameW, v));
  const clampY = (v: number) => Math.max(0, Math.min(frameH, v));
  const x1 = clampX(Math.min(box[0], box[2]));
  const x2 = clampX(Math.max(box[0], box[2]));
  const y1 = clampY(Math.min(box[1], box[3]));
  const y2 = clampY(Math.max(box[1], box[3]));
  return { x: video.x + x1 * sx, y: video.y + y1 * sy, w: (x2 - x1) * sx, h: (y2 - y1) * sy };
}

/** PPE uses green-teal hues, IDSS orange-magenta; the class picks a shade within the model's range. */
export function classColor(model: ModelKey, cls: number): string {
  const base = model === "ppe" ? 150 : 15;
  const hue = (base + (((cls % 6) + 6) % 6) * 22) % 360;
  return `hsl(${hue} 90% 52%)`;
}

/** Maps one frame-pixel point into the displayed video rectangle. */
export function mapPoint(
  pt: readonly [number, number],
  frameW: number,
  frameH: number,
  video: Rect,
): [number, number] {
  return [video.x + (pt[0] * video.w) / frameW, video.y + (pt[1] * video.h) / frameH];
}
