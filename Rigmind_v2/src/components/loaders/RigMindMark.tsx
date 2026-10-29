import React from "react";

/**
 * Animated RigMind mark, shared by BootSplash and PageLoader.
 * The derrick bracing lights up from base to top, then the beacon
 * broadcasts its signal arcs. Loops every 1.8 s.
 * Respects prefers-reduced-motion.
 */
export type MarkProps = {
  size?: number;
  tone?: "dark" | "light" | "auto"; // dark = navy mark for light backgrounds; auto follows theme
  animate?: boolean;       // false = static mark
  intro?: boolean;         // draw the shield in on mount (boot splash)
};

export const NAVY = "#12213A";
export const ORANGE = "#F29A1F";

const css = `
.rm-shield{stroke-dasharray:1;stroke-dashoffset:0}
.rm-intro .rm-shield{stroke-dashoffset:1;animation:rm-draw .9s cubic-bezier(.6,0,.2,1) forwards}
.rm-intro .rm-tower{opacity:0;animation:rm-fade .5s .35s ease-out forwards}
.rm-brace{animation:rm-rise 1.8s ease-in-out infinite}
.rm-b2{animation-delay:.1s}.rm-b3{animation-delay:.2s}.rm-b4{animation-delay:.3s}.rm-b5{animation-delay:.4s}
.rm-dot{transform-origin:60px 22px;animation:rm-beacon 1.8s .5s ease-out infinite}
.rm-arc{opacity:0;transform-origin:60px 22px;animation:rm-arc 1.8s ease-out infinite}
.rm-arc1{animation-delay:.6s}.rm-arc2{animation-delay:.78s}
@keyframes rm-draw{to{stroke-dashoffset:0}}
@keyframes rm-fade{to{opacity:1}}
@keyframes rm-rise{0%,100%{stroke:var(--rm-ink)}12%{stroke:var(--rm-accent)}35%{stroke:var(--rm-ink)}}
@keyframes rm-beacon{0%,100%{transform:scale(1);fill:var(--rm-ink)}15%{transform:scale(1.35);fill:var(--rm-accent)}45%{transform:scale(1);fill:var(--rm-ink)}}
@keyframes rm-arc{0%{opacity:0;transform:scale(.85)}18%{opacity:1;transform:scale(1)}55%{opacity:1}80%,100%{opacity:0;transform:scale(1.08)}}
.rm-still .rm-brace,.rm-still .rm-dot{animation:none}
.rm-still .rm-arc{animation:none;opacity:1}
@media (prefers-reduced-motion:reduce){
  .rm-intro .rm-shield{animation:none;stroke-dashoffset:0}
  .rm-intro .rm-tower{animation:none;opacity:1}
  .rm-brace,.rm-dot{animation:none}
  .rm-arc{animation:rm-fade 2.4s ease-in-out infinite alternate;opacity:.4}
}`;

export default function RigMindMark({ size = 96, tone = "auto", intro = false, animate = true }: MarkProps) {
  const ink = tone === "light" ? "#FFFFFF" : tone === "dark" ? NAVY : "var(--foreground)";
  const style = { color: ink, ["--rm-ink" as string]: ink, ["--rm-accent" as string]: ORANGE } as React.CSSProperties;

  return (
    <svg className={[intro && "rm-intro", !animate && "rm-still"].filter(Boolean).join(" ") || undefined} style={style} width={size} height={size * 112 / 120}
      viewBox="0 0 120 112" fill="none" aria-hidden="true">
      <style>{css}</style>
      <g stroke="currentColor" strokeWidth={8} strokeLinejoin="round">
        <path className="rm-shield" pathLength={1} d="M47 25 L24 33 C24 63 34 85 60 103" />
        <path className="rm-shield" pathLength={1} d="M73 25 L96 33 C96 63 86 85 60 103" />
      </g>
      <g className="rm-tower">
        <path d="M52 88 L57 32 M68 88 L63 32" stroke="currentColor" strokeWidth={3.6} strokeLinecap="round" />
        <g stroke="currentColor" strokeWidth={2.2} strokeLinecap="round">
          <path className="rm-brace rm-b1" d="M52 88L66.9 76M68 88L53.1 76" />
          <path className="rm-brace rm-b2" d="M53.1 76L65.9 64M66.9 76L54.1 64" />
          <path className="rm-brace rm-b3" d="M54.1 64L64.8 52M65.9 64L55.2 52" />
          <path className="rm-brace rm-b4" d="M55.2 52L63.7 40M64.8 52L56.3 40" />
          <path className="rm-brace rm-b5" d="M56.3 40L63 32M63.7 40L57 32" />
        </g>
        <rect x={53} y={28} width={14} height={4} rx={1} fill="currentColor" />
        <rect x={46} y={87} width={28} height={5} rx={1} fill="currentColor" />
        <path d="M60 28 V25" stroke="currentColor" strokeWidth={2.4} />
        <circle className="rm-dot" cx={60} cy={22} r={3.6} fill="currentColor" />
      </g>
      <g stroke={ORANGE} strokeWidth={3} strokeLinecap="round">
        <g className="rm-arc rm-arc1">
          <path d="M51.5 18.9 A9 9 0 0 1 56.9 13.5" /><path d="M63.1 13.5 A9 9 0 0 1 68.5 18.9" />
        </g>
        <g className="rm-arc rm-arc2">
          <path d="M45.9 16.9 A15 15 0 0 1 54.9 7.9" /><path d="M65.1 7.9 A15 15 0 0 1 74.1 16.9" />
        </g>
      </g>
    </svg>
  );
}