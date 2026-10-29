import { useEffect, useRef } from "react";

// Normalized drill-floor boundary, shared by drawing and pointer hit testing.
export const ZONE = new Float32Array([.48, .70, .70, .64, .88, .76, .79, .87, .43, .85]);
export function insideZone(x: number, y: number) {
  let inside = false;
  for (let i = 0, j = ZONE.length - 2; i < ZONE.length; j = i, i += 2) {
    const ax = ZONE[i] ?? 0, ay = ZONE[i + 1] ?? 0;
    const bx = ZONE[j] ?? 0, by = ZONE[j + 1] ?? 0;
    if ((ay > y) !== (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax) inside = !inside;
  }
  return inside;
}

export default function MonitoringField() {
  const hostRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const host = hostRef.current;
    const canvas = canvasRef.current;
    if (!host || !canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const motion = window.matchMedia("(prefers-reduced-motion: reduce)");
    let reduced = motion.matches;
    let visible = false, frame = 0, last = 0, clock = 0;
    let width = 0, height = 0, dpr = 1;
    let pointer = false, px = 0, py = 0, label = 0, angle = .7;
    let card = "", foreground = "", primary = "", amber = "", mono = "";
    let gradient: CanvasGradient | null = null;
    let dots = new Float32Array(0);
    const dash = [6, 5], solid: number[] = [];
    let chipFont = "";
    const readColors = () => {
      const css = getComputedStyle(host);
      card = css.getPropertyValue("--card").trim();
      foreground = css.getPropertyValue("--foreground").trim();
      primary = css.getPropertyValue("--primary").trim();
      amber = css.getPropertyValue("--monitor-zone").trim();
      mono = css.getPropertyValue("--font-mono").trim();
      chipFont = `10px ${mono}`;
      // The gradient is rebuilt only on resize/theme changes, not every frame.
      gradient = ctx.createLinearGradient(0, 0, Math.hypot(width, height), 0);
      gradient.addColorStop(0, primary);
      gradient.addColorStop(1, "transparent");
    };
    const draw = (dt: number) => {
      const cx = width * .17, cy = height * .20;
      const gx = pointer ? px : width * (.61 + .13 * Math.sin(clock * .23));
      const gy = pointer ? py : height * (.73 + .07 * Math.sin(clock * .31));
      const active = pointer && insideZone(px / width, py / height);
      const targetAngle = active ? Math.atan2(py - cy, px - cx) : .69 + (reduced ? 0 : .20 * Math.sin(clock * Math.PI / 4));
      const ease = reduced ? 1 : 1 - Math.exp(-dt * 7);
      angle += (targetAngle - angle) * ease;
      label += ((active ? 1 : 0) - label) * ease;
      ctx.globalAlpha = 1;
      ctx.fillStyle = card;
      ctx.fillRect(0, 0, width, height);
      ctx.fillStyle = foreground;
      for (let i = 0; i < dots.length; i += 3) {
        const x = dots[i] ?? 0, y = dots[i + 1] ?? 0, previous = dots[i + 2] ?? 0;
        const dx = x - gx, dy = y - gy;
        const target = pointer || !reduced ? Math.max(0, 1 - Math.sqrt(dx * dx + dy * dy) / 90) : 0;
        const intensity = previous + (target - previous) * ease;
        dots[i + 2] = intensity;
        ctx.globalAlpha = .08 + .32 * intensity;
        ctx.beginPath(); ctx.arc(x, y, 1 + intensity, 0, Math.PI * 2); ctx.fill();
      }
      // Static engineering schematic.
      const top = height * .20, floor = height * .77, center = width * .66;
      const narrow = width * .024, wide = width * .105;
      ctx.globalAlpha = .35; ctx.strokeStyle = foreground; ctx.lineWidth = 1.25;
      ctx.beginPath();
      ctx.moveTo(center - narrow, top); ctx.lineTo(center - wide, floor);
      ctx.moveTo(center + narrow, top); ctx.lineTo(center + wide, floor);
      for (let i = 0; i < 5; i++) {
        const y1 = top + (floor - top) * i / 5, y2 = top + (floor - top) * (i + 1) / 5;
        const a = narrow + (wide - narrow) * i / 5, b = narrow + (wide - narrow) * (i + 1) / 5;
        ctx.moveTo(center - a, y1); ctx.lineTo(center + b, y2);
        ctx.moveTo(center + a, y1); ctx.lineTo(center - b, y2);
      }
      ctx.rect(center - narrow - 4, top - 9, narrow * 2 + 8, 9);
      ctx.moveTo(width * .42, floor); ctx.lineTo(width * .88, floor); ctx.stroke();
      // Drill-floor monitoring perimeter.
      ctx.beginPath(); ctx.moveTo((ZONE[0] ?? 0) * width, (ZONE[1] ?? 0) * height);
      for (let i = 2; i < ZONE.length; i += 2) ctx.lineTo((ZONE[i] ?? 0) * width, (ZONE[i + 1] ?? 0) * height);
      ctx.closePath(); ctx.fillStyle = amber; ctx.globalAlpha = .06; ctx.fill();
      ctx.strokeStyle = amber; ctx.globalAlpha = .6 + .4 * label;
      ctx.setLineDash(dash); ctx.lineDashOffset = reduced ? 0 : -clock * 12; ctx.stroke(); ctx.setLineDash(solid);
      // Camera field of view, rotating its cached local gradient.
      ctx.save(); ctx.translate(cx, cy); ctx.rotate(angle);
      ctx.globalAlpha = .14; ctx.fillStyle = gradient ?? primary;
      ctx.beginPath(); ctx.moveTo(0, 0); ctx.arc(0, 0, Math.hypot(width, height), -19 * Math.PI / 180, 19 * Math.PI / 180); ctx.closePath(); ctx.fill(); ctx.restore();
      ctx.globalAlpha = 1; ctx.fillStyle = primary;
      ctx.beginPath(); ctx.arc(cx, cy, 6, 0, Math.PI * 2); ctx.fill();
      if (label > .005) {
        const lx = Math.max(5, Math.min(width - 112, px + 12));
        const ly = Math.max(6, Math.min(height - 42, py - 28));
        ctx.globalAlpha = label; ctx.fillStyle = card; ctx.fillRect(lx, ly, 107, 22);
        ctx.strokeStyle = amber; ctx.lineWidth = 1; ctx.strokeRect(lx, ly, 107, 22);
        ctx.fillStyle = foreground; ctx.font = chipFont; ctx.textBaseline = "middle";
        ctx.fillText("Person in zone", lx + 7, ly + 11);
      }
      ctx.globalAlpha = 1;
    };
    const tick = (now: number) => {
      frame = 0;
      if (!visible || document.hidden || !width || !height) return;
      const delta = last ? now - last : 1000 / 60;
      if (delta >= 1000 / 60 - .5) {
        const dt = Math.min(delta / 1000, .05);
        last = now; if (!reduced) clock += dt;
        draw(dt);
      }
      if (!reduced) frame = requestAnimationFrame(tick);
    };
    const wake = () => {
      if (visible && !document.hidden && !frame) frame = requestAnimationFrame(tick);
    };
    const resize = () => {
      const bounds = host.getBoundingClientRect();
      width = bounds.width; height = bounds.height; dpr = window.devicePixelRatio || 1;
      canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      const cols = Math.max(0, Math.ceil((width - 11) / 22)), rows = Math.max(0, Math.ceil((height - 11) / 22));
      dots = new Float32Array(cols * rows * 3);
      let index = 0;
      for (let y = 11; y < height; y += 22) for (let x = 11; x < width; x += 22) {
        dots[index++] = x; dots[index++] = y; dots[index++] = 0;
      }
      readColors(); last = 0; wake();
    };
    const move = (event: PointerEvent) => {
      const bounds = canvas.getBoundingClientRect();
      px = event.clientX - bounds.left; py = event.clientY - bounds.top;
      pointer = px >= 0 && py >= 0 && px <= width && py <= height; wake();
    };
    const down = (event: PointerEvent) => {
      canvas.setPointerCapture(event.pointerId); move(event);
    };
    const leave = () => { pointer = false; wake(); };
    const visibility = () => {
      if (document.hidden || !visible) { cancelAnimationFrame(frame); frame = 0; last = 0; }
      else wake();
    };
    const preference = () => { reduced = motion.matches; last = 0; wake(); };
    const theme = new MutationObserver(() => { readColors(); wake(); });
    theme.observe(document.documentElement, { attributes: true, attributeFilter: ["class", "style"] });
    const observer = new ResizeObserver(resize); observer.observe(host);
    const intersection = new IntersectionObserver(entries => { visible = entries[0]?.isIntersecting ?? false; visibility(); });
    intersection.observe(host);
    canvas.addEventListener("pointermove", move); canvas.addEventListener("pointerdown", down);
    canvas.addEventListener("pointerleave", leave); canvas.addEventListener("pointerup", leave); canvas.addEventListener("pointercancel", leave);
    document.addEventListener("visibilitychange", visibility); motion.addEventListener("change", preference);
    window.addEventListener("resize", resize);
    resize();
    return () => {
      cancelAnimationFrame(frame); observer.disconnect(); intersection.disconnect(); theme.disconnect();
      canvas.removeEventListener("pointermove", move); canvas.removeEventListener("pointerdown", down);
      canvas.removeEventListener("pointerleave", leave); canvas.removeEventListener("pointerup", leave); canvas.removeEventListener("pointercancel", leave);
      document.removeEventListener("visibilitychange", visibility); motion.removeEventListener("change", preference);
      window.removeEventListener("resize", resize);
    };
  }, []);
  return (
    <div ref={hostRef} className="relative h-56 w-full overflow-hidden rounded-lg border border-border bg-card lg:h-[420px]">
      <canvas ref={canvasRef} aria-hidden="true" className="block size-full touch-none" />
      <p className="sr-only">Simulated camera monitoring a derrick and its drill-floor zone. Moving or touching the field highlights nearby sensors; entering the zone marks a person and directs the camera toward them.</p>
      <div className="pointer-events-none absolute bottom-3 left-3 right-3 flex items-center gap-1.5 font-mono text-[11px] text-muted-foreground">
        <span className="size-1.5 shrink-0 rounded-full bg-success" />
        <span>CAM-01 · Derrick board · live view (demo)</span>
      </div>
    </div>
  );
}