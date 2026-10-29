import type { ReactNode, ButtonHTMLAttributes } from "react";
import { cn } from "@/lib/utils";
import type { Tone } from "@/data/mock";
import feed from "@/assets/rig-feed.jpg";
import { demoDataEnabled } from "@/lib/demo-mode";

export const pageHead = (title: string, description: string) => () => ({
  meta: [
    { title: `${title} · RigMind` },
    { name: "description", content: description },
    { property: "og:title", content: `${title} · RigMind` },
    { property: "og:description", content: description },
    { property: "og:type", content: "website" },
    { name: "twitter:card", content: "summary" },
  ],
});

const toneCls: Record<Tone, string> = {
  critical: "text-critical bg-critical-bg border-critical/30",
  warning: "text-warning bg-warning-bg border-warning/30",
  advisory: "text-advisory bg-advisory-bg border-advisory/30",
  success: "text-success bg-success-bg border-success/30",
  neutral: "text-neutral bg-neutral-bg border-border",
};

export function Chip({ tone = "neutral", children, dot, className }: { tone?: Tone; children: ReactNode; dot?: boolean; className?: string }) {
  return (
    <span className={cn("inline-flex items-center gap-1 whitespace-nowrap rounded-sm border px-1.5 py-0.5 text-[10px] font-semibold leading-none", toneCls[tone], className)}>
      {dot && <span className="size-1.5 rounded-full bg-current" />}
      {children}
    </span>
  );
}

export function Card({ children, className }: { children: ReactNode; className?: string }) {
  return <div className={cn("rounded-lg border border-border bg-card", className)}>{children}</div>;
}

export function CardHead({ title, sub, right }: { title: ReactNode; sub?: ReactNode; right?: ReactNode }) {
  return (
    <div className="flex items-start justify-between gap-3 border-b border-border px-4 py-3">
      <div>
        <div className="font-semibold">{title}</div>
        {sub && <div className="text-xs text-muted-foreground">{sub}</div>}
      </div>
      {right}
    </div>
  );
}

type BtnVariant = "primary" | "outline" | "danger" | "ghost" | "muted";
const btnCls: Record<BtnVariant, string> = {
  primary: "bg-primary text-primary-foreground hover:bg-primary-hover border-primary",
  outline: "bg-card text-foreground border-border hover:bg-field",
  danger: "bg-critical text-destructive-foreground border-critical hover:opacity-90",
  ghost: "border-transparent text-muted-foreground hover:bg-field",
  muted: "bg-neutral-bg text-muted-foreground border-border cursor-default",
};
export function Btn({ variant = "outline", className, ...p }: ButtonHTMLAttributes<HTMLButtonElement> & { variant?: BtnVariant }) {
  return <button {...p} className={cn("inline-flex h-8 items-center justify-center gap-1.5 whitespace-nowrap rounded-md border px-3 text-[13px] font-medium transition-colors disabled:opacity-60", btnCls[variant], className)} />;
}

export function PageHeader({ eyebrow, title, subtitle, actions }: { eyebrow: string; title: string; subtitle?: ReactNode; actions?: ReactNode }) {
  return (
    <div className="mb-5 flex flex-col gap-3 md:flex-row md:items-end md:justify-between">
      <div>
        <div className="eyebrow">{eyebrow}</div>
        <h1 className="mt-1 text-[28px] font-semibold leading-tight tracking-tight">{title}</h1>
        {subtitle && <p className="mt-1 text-muted-foreground">{subtitle}</p>}
      </div>
      {actions && <div className="flex flex-wrap gap-2">{actions}</div>}
    </div>
  );
}

export function Callout({ title, children, tone = "advisory" }: { title: string; children: ReactNode; tone?: "advisory" | "warning" }) {
  return (
    <div className={cn("rounded-md border-l-[3px] bg-field px-4 py-3", tone === "warning" ? "border-l-warning bg-warning-bg" : "border-l-primary")}>
      <div className="font-semibold">{title}</div>
      <div className="text-[13px] text-muted-foreground">{children}</div>
    </div>
  );
}

export function Avatar({ initials, size = 28 }: { initials: string; size?: number }) {
  return (
    <span className="inline-flex shrink-0 items-center justify-center rounded-full bg-slate text-[10px] font-semibold text-slate-foreground" style={{ width: size, height: size }}>
      {initials}
    </span>
  );
}

export function Kpi({ label, value, note, noteTone }: { label: string; value: ReactNode; note?: ReactNode; noteTone?: "critical" | "success" }) {
  return (
    <Card className="p-4">
      <div className="text-xs font-medium text-muted-foreground">{label}</div>
      <div className="mt-1 font-mono text-[28px] font-semibold leading-none">{value}</div>
      {note && <div className={cn("mt-2 text-xs", noteTone === "critical" ? "text-critical" : noteTone === "success" ? "text-success" : "text-muted-foreground")}>{note}</div>}
    </Card>
  );
}

export function Tabs({ tabs, value, onChange }: { tabs: { id: string; label: ReactNode }[]; value: string; onChange: (id: string) => void }) {
  return (
    <div className="mb-4 flex gap-1 overflow-x-auto border-b border-border">
      {tabs.map((t) => (
        <button key={t.id} onClick={() => onChange(t.id)} className={cn("-mb-px whitespace-nowrap border-b-2 px-3 py-2 text-[13px] font-medium", value === t.id ? "border-primary text-primary" : "border-transparent text-muted-foreground hover:text-foreground")}>
          {t.label}
        </button>
      ))}
    </div>
  );
}

export function Field({ label, children, hint }: { label: string; children: ReactNode; hint?: string }) {
  return (
    <label className="block">
      <span className="mb-1 block text-xs font-medium text-muted-foreground">{label}</span>
      {children}
      {hint && <span className="mt-1 block text-[11px] text-muted-foreground">{hint}</span>}
    </label>
  );
}
export const inputCls = "h-9 w-full rounded-md border border-border bg-field px-3 text-[13px] outline-none focus:border-primary focus:ring-2 focus:ring-primary/15";

export function Select({ options, value, onChange, className }: { options: string[]; value?: string; onChange?: (v: string) => void; className?: string }) {
  return (
    <select value={value} onChange={(e) => onChange?.(e.target.value)} className={cn(inputCls, "pr-8", className)}>
      {options.map((o) => <option key={o}>{o}</option>)}
    </select>
  );
}

export function Row({ k, v }: { k: ReactNode; v: ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3 border-b border-border py-2 text-[13px] last:border-0">
      <span className="text-muted-foreground">{k}</span>
      <span className="text-right font-medium">{v}</span>
    </div>
  );
}

export function TableWrap({ children }: { children: ReactNode }) {
  return <div className="overflow-x-auto"><table className="w-full min-w-[720px] text-[13px]">{children}</table></div>;
}
export const th = "border-b border-border bg-field px-4 py-2 text-left text-[10px] font-semibold uppercase tracking-wider text-muted-foreground";
export const td = "border-b border-border px-4 py-3 align-middle";

export function Mono({ children, className }: { children: ReactNode; className?: string }) {
  return <span className={cn("font-mono text-[12px]", className)}>{children}</span>;
}

export function DetectionOverlay({ label = "Person 01 | Zone overlap" }: { label?: string | undefined }) {
  return (
    <svg viewBox="0 0 100 56.25" preserveAspectRatio="none" className="pointer-events-none absolute inset-0 h-full w-full">
      <polygon points="38,22 62,20 70,42 32,45" className="fill-zone/15 stroke-zone" strokeWidth="0.4" strokeDasharray="1.5 1" />
      <rect x="51" y="27" width="6" height="12" className="fill-none stroke-detect" strokeWidth="0.4" />
      <rect x="51" y="24.2" width="21" height="2.8" className="fill-detect" />
      <text x="51.8" y="26.2" fontSize="1.8" className="fill-foreground font-mono">{label}</text>
    </svg>
  );
}

export function CameraTile({ id, area, overlay, overlayLabel, live, offline, className, children }: { id: string; area: string; overlay?: boolean; overlayLabel?: string | undefined; live?: boolean; offline?: boolean; className?: string; children?: ReactNode }) {
  return (
    <Card className={cn("overflow-hidden", className)}>
      <div className="flex items-center justify-between gap-2 px-3 py-2 text-xs">
        <div className="flex min-w-0 items-center gap-2"><Mono className="font-semibold">{id}</Mono><span className="truncate text-muted-foreground">{area}</span></div>
        <span className="whitespace-nowrap text-[11px] text-muted-foreground">{offline ? <><span className="text-critical">●</span> Offline</> : <><span className="text-success">●</span> {live ? "Live" : demoDataEnabled() ? "Demo image" : "No stream"}</>}</span>
      </div>
      <div className="relative aspect-video bg-slate">
        {offline ? (
          <div className="flex h-full w-full flex-col items-center justify-center gap-1 bg-[#3B4350] text-white/80">
            <span className="text-[11px] font-medium uppercase tracking-[0.14em]">Camera offline</span>
            <span className="text-[11px] text-white/60">No stream connected</span>
          </div>
        ) : (
          !live &&
          (demoDataEnabled() ? (
            <img src={feed} alt={`${area} demo camera image`} loading="lazy" width={1280} height={720} className="h-full w-full object-cover" />
          ) : (
            <div className="flex h-full w-full flex-col items-center justify-center gap-1 bg-[#3B4350] text-white/80">
              <span className="text-[11px] font-medium uppercase tracking-[0.14em]">No stream</span>
              <span className="text-[11px] text-white/60">Add this camera under Administration / Cameras</span>
            </div>
          ))
        )}
        {overlay && !offline && <DetectionOverlay label={overlayLabel} />}
        {children}
      </div>
      <div className="px-3 py-1.5 font-mono text-[11px] text-muted-foreground">{offline ? "No signal" : live ? "Live" : demoDataEnabled() ? "Demo image" : "No stream"}</div>
    </Card>
  );
}
