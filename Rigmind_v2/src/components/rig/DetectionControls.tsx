import { useEffect, useState } from "react";
import { isNative, nativeCall } from "@/lib/native-bridge";
import {
  MODELS,
  setOverlay,
  useDetections,
  useOverlayPrefs,
  type ModelKey,
} from "@/lib/detections";
import { classColor } from "@/lib/detection-geometry";
import { alertColors } from "@/lib/alert-colors";
import { ppeCondition } from "@/lib/ppe-alerts";

/** "Red · Derrickman out of safe zone · Derrick board"; Yellow reads "Monitoring active". */
export function alarmText(a: { state: string; condition: string; place: string }): string {
  // Yellow is only "the app is monitoring": a quiet chip, no condition text. Orange and Red are escalations.
  if (a.state === "Yellow") return "Monitoring active";
  return [a.state, a.condition, a.place].filter(Boolean).join(" · ");
}

/** Explicit colours so Yellow, Orange and Red stay distinct in both themes. */
export function alarmStyle(state: string): { background: string; color: string; borderColor: string } {
  if (state === "Red") return { background: "#dc2626", color: "#ffffff", borderColor: "#7f1d1d" };
  if (state === "Orange") return { background: "#f97316", color: "#1a0e00", borderColor: "#9a3412" };
  return { background: "#facc15", color: "#1c1500", borderColor: "#a16207" };
}

const NAMES: Record<ModelKey, string> = { ppe: "PPE", idss: "IDSS" };

/** PPE / IDSS overlay toggles plus live inference numbers. Renders nothing outside the native host. */
export function DetectionControls({ camId }: { camId: string }) {
  const native = isNative();
  const det = useDetections(camId);
  const prefs = useOverlayPrefs();
  const [, setTick] = useState(0);
  // Counts expire with the detections; tick so the chip follows.
  useEffect(() => {
    if (!native) return;
    const t = setInterval(() => setTick((n) => n + 1), 500);
    return () => clearInterval(t);
  }, [native]);
  if (!native) return null;

  return (
    <div className="flex flex-wrap items-center gap-2" data-testid="detection-controls">
      {MODELS.map((m) => (
        <button
          key={m}
          type="button"
          aria-pressed={prefs[m]}
          onClick={() => {
            setOverlay(m, !prefs[m]);
            // When the pipeline draws on the frames, the toggle has to reach it; in canvas mode this is harmless.
            nativeCall("stream.overlay", { cam: camId, ...prefs, [m]: !prefs[m] }).catch(() => {});
          }}
          className={
            "inline-flex h-7 items-center gap-1.5 rounded-md border px-2 text-[11px] font-semibold " +
            (prefs[m]
              ? "border-border bg-field text-foreground"
              : "border-border text-muted-foreground opacity-70")
          }
        >
          <span
            className="size-2 rounded-full"
            style={{
              background: prefs[m] ? classColor(m, 0) : "transparent",
              boxShadow: `inset 0 0 0 1.5px ${classColor(m, 0)}`,
            }}
          />
          {NAMES[m]}
        </button>
      ))}
      {det.ppe?.state === "missing" && det.ppe.missing.length > 0 && (
        <span
          role="status"
          data-testid="ppe-chip"
          className="inline-flex h-7 items-center rounded-md border px-2 text-[11px] font-semibold"
          style={alertColors("Advisory")}
        >
          {ppeCondition(det.ppe.missing)}
        </span>
      )}
      {det.alarm && (
        <span
          role="status"
          data-testid="alarm-chip"
          className="inline-flex h-7 items-center rounded-md border px-2 text-[11px] font-semibold"
          style={alarmStyle(det.alarm.state)}
        >
          {alarmText(det.alarm)}
        </span>
      )}
    </div>
  );
}
