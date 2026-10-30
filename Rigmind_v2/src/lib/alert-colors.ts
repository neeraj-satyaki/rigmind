/** One colour per alarm level, shared by toasts, the bell and the Live chip: Yellow, Orange, Red. */
export type AlertColors = { background: string; color: string; borderColor: string };

export function alertColors(severity: string): AlertColors {
  if (severity === "Critical" || severity === "Red") return { background: "#dc2626", color: "#ffffff", borderColor: "#7f1d1d" };
  if (severity === "Warning" || severity === "Orange") return { background: "#f97316", color: "#1a0e00", borderColor: "#9a3412" };
  return { background: "#facc15", color: "#1c1500", borderColor: "#a16207" };
}

/**
 * Toast for one level: the theme background (the loading-screen navy in dark theme), transparent and blurred (no gradient), a rim and glow in the
 * level colour, and thick bold text in that colour (yellow, orange, red; deeper shades on the light theme, see --toast-* in
 * styles.css). Spread into sonner's toast options: toast(title, { description, ...alertToastOptions(severity) }).
 */
export function alertToastOptions(severity: string) {
  const level = severity === "Critical" || severity === "Red" ? "red" : severity === "Warning" || severity === "Orange" ? "orange" : "yellow";
  const text = `var(--toast-${level})`;
  // The rim is the pure level colour (same in light and dark theme), full strength.
  const rim = { yellow: "#FFD60A", orange: "#FF9500", red: "#FF3B30" }[level];
  return {
    style: {
      background: "var(--toast-bg)", // light: the theme colour at a third; dark: the loading-screen navy; both see-through, the blur keeps the text readable
      color: text,
      border: `2px solid ${rim}`,
      backdropFilter: "blur(18px) saturate(170%)",
      WebkitBackdropFilter: "blur(18px) saturate(170%)",
      boxShadow: `0 10px 28px color-mix(in srgb, ${rim} 35%, transparent), inset 0 1px 0 rgba(255,255,255,0.22)`, // soft glow in the rim colour + glass top highlight
      borderRadius: "14px",
    } as React.CSSProperties,
    classNames: {
      title: "!font-black !text-[16px] !tracking-tight !text-inherit",
      description: "!text-inherit !font-bold !opacity-100",
    },
    descriptionClassName: "!text-inherit font-bold",
  };
}

/** Title and description for the toast of one raised alert. */
export function alertToastText(a: { id: string; severity: string; condition: string; place: string }) {
  const level = a.severity === "Critical" ? "Red" : a.severity === "Warning" ? "Orange" : "Yellow";
  return { title: `${level} · ${a.severity} safety event`, description: `${a.id} · ${a.condition} · ${a.place}` };
}
