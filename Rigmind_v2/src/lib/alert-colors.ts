/** One colour per alarm level, shared by toasts, the bell and the Live chip: Yellow, Orange, Red. */
export type AlertColors = { background: string; color: string; borderColor: string };

export function alertColors(severity: string): AlertColors {
  if (severity === "Critical" || severity === "Red") return { background: "#dc2626", color: "#ffffff", borderColor: "#7f1d1d" };
  if (severity === "Warning" || severity === "Orange") return { background: "#f97316", color: "#1a0e00", borderColor: "#9a3412" };
  return { background: "#facc15", color: "#1c1500", borderColor: "#a16207" };
}

/** Title and description for the toast of one raised alert. */
export function alertToastText(a: { id: string; severity: string; condition: string; place: string }) {
  const level = a.severity === "Critical" ? "Red" : a.severity === "Warning" ? "Orange" : "Yellow";
  return { title: `${level} · ${a.severity} safety event`, description: `${a.id} · ${a.condition} · ${a.place}` };
}
