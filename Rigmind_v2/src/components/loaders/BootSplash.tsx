import React from "react";
import RigMindMark, { ORANGE } from "./RigMindMark";

/**
 * Application boot splash — fills the desktop window while the app starts.
 *   <BootSplash progress={0.4} status="Loading safety rules" />
 * Omit `progress` for an indeterminate bar. Set `done` to fade it out
 * (remove it from the tree after ~400 ms).
 */
type Props = {
  progress?: number;      // 0–1; omit for indeterminate
  status?: string;        // current boot step
  version?: string;
  done?: boolean;
};

export const BOOT_STEPS = [
  "Starting services",
  "Connecting to rig-local edge",
  "Loading safety rules",
  "Syncing events",
  "Preparing workspace",
];

const css = `
.rm-boot{position:fixed;inset:0;z-index:10000;display:flex;flex-direction:column;align-items:center;justify-content:center;
  background:radial-gradient(120% 90% at 50% 40%,#16294A 0%,#0D1A2E 70%);color:#fff;
  font-family:'IBM Plex Sans',system-ui,sans-serif;transition:opacity .4s ease;-webkit-app-region:drag;user-select:none}
.rm-boot--done{opacity:0;pointer-events:none}
.rm-boot__brand{display:flex;flex-direction:column;align-items:center;gap:22px}
.rm-boot__word{font-size:30px;letter-spacing:.06em;line-height:1}
.rm-boot__word b{font-weight:700}.rm-boot__word span{font-weight:400}
.rm-boot__word .tm{font-size:.55em;font-weight:500;vertical-align:super;line-height:0;margin-left:1px}
.rm-boot__tag{font-size:10px;letter-spacing:.32em;color:#9FB0C8;margin-top:-10px}
.rm-boot__bar{width:220px;height:3px;border-radius:2px;background:rgba(255,255,255,.12);overflow:hidden;margin-top:34px}
.rm-boot__fill{height:100%;border-radius:2px;background:${ORANGE};transition:width .35s ease}
.rm-boot__fill--ind{width:35%;animation:rm-ind 1.4s ease-in-out infinite}
.rm-boot__status{margin-top:12px;height:16px;font-size:11px;color:#9FB0C8;letter-spacing:.02em}
.rm-boot__foot{position:absolute;bottom:22px;left:0;right:0;display:flex;justify-content:space-between;padding:0 28px;font-size:10px;color:#6F82A0}
@keyframes rm-ind{0%{transform:translateX(-110%)}100%{transform:translateX(320%)}}
@media (prefers-reduced-motion:reduce){.rm-boot__fill--ind{animation:none;width:100%;opacity:.5}}
`;

export default function BootSplash({ progress, status = BOOT_STEPS[0], version = "v0.1.0", done = false }: Props) {
  const determinate = typeof progress === "number";
  const pct = determinate ? Math.round(Math.min(1, Math.max(0, progress ?? 0)) * 100) : undefined;

  return (
    <div className={`rm-boot${done ? " rm-boot--done" : ""}`} role="status" aria-live="polite" aria-label={`RigMind is starting. ${status}`}>
      <style>{css}</style>
      <div className="rm-boot__brand">
        <RigMindMark size={112} tone="light" intro />
        <div className="rm-boot__word"><b>RIG</b><span>MIND</span><sup className="tm">™</sup></div>
        <div className="rm-boot__tag">RIG SAFETY. CONNECTED.</div>
      </div>
      <div className="rm-boot__bar" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct}>
        <div className={`rm-boot__fill${determinate ? "" : " rm-boot__fill--ind"}`} style={determinate ? { width: `${pct}%` } : undefined} />
      </div>
      <div className="rm-boot__status">{status}…</div>
      <div className="rm-boot__foot"><span>Powered by Satyaki</span><span>{version}</span></div>
    </div>
  );
}