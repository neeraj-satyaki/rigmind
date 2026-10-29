import React, { useEffect, useState } from "react";
import RigMindMark from "./RigMindMark";

/**
 * Page-to-page loading, in two layers:
 * 1. <TopProgress active={isNavigating} />  thin orange bar under the top bar.
 * 2. <PageLoader />  centered mark inside the content area; waits `delay` ms
 *    before appearing so quick pages never flash it.
 */

const ORANGE = "#F29A1F";

const css = `
.rm-top{position:absolute;left:0;right:0;top:auto;bottom:-2px;height:2px;overflow:hidden;pointer-events:none;z-index:50}
.rm-top__bar{height:100%;width:0;background:${ORANGE};box-shadow:0 0 6px rgba(242,154,31,.6)}
.rm-top--on .rm-top__bar{animation:rm-trickle 8s cubic-bezier(.1,.6,.2,1) forwards}
.rm-top--off .rm-top__bar{width:100%;opacity:0;transition:opacity .3s .15s ease,width .15s ease}
@keyframes rm-trickle{0%{width:0}15%{width:45%}50%{width:75%}100%{width:92%}}

.rm-page{flex-grow:1;min-height:320px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px;
  font-family:'IBM Plex Sans',system-ui,sans-serif;color:#4A5566;animation:rm-in .25s ease-out}
.rm-page__label{font-size:11px;letter-spacing:.14em;text-transform:uppercase}
@keyframes rm-in{from{opacity:0}to{opacity:1}}
@media (prefers-reduced-motion:reduce){.rm-top--on .rm-top__bar{animation:none;width:60%}}
`;

export function TopProgress({ active }: { active: boolean }) {
  const [state, setState] = useState<"idle" | "on" | "off">("idle");
  useEffect(() => {
    if (active) setState("on");
    else if (state === "on") {
      setState("off");
      const t = setTimeout(() => setState("idle"), 500);
      return () => clearTimeout(t);
    }
    return undefined;
  }, [active]); // eslint-disable-line react-hooks/exhaustive-deps

  if (state === "idle") return null;
  return (
    <div className={`rm-top rm-top--${state}`} aria-hidden="true">
      <style>{css}</style>
      <div className="rm-top__bar" />
    </div>
  );
}

export default function PageLoader({ label = "Loading", delay = 300 }: { label?: string; delay?: number }) {
  const [show, setShow] = useState(delay === 0);
  useEffect(() => {
    const t = setTimeout(() => setShow(true), delay);
    return () => clearTimeout(t);
  }, [delay]);

  if (!show) return null;
  return (
    <div className="rm-page" role="status" aria-live="polite" aria-label={label}>
      <style>{css}</style>
      <RigMindMark size={56} />
      <span className="rm-page__label">{label}</span>
    </div>
  );
}