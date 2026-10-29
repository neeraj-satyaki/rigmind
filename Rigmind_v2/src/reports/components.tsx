import type { ReactNode } from "react";
import satyakiLogo from "@/assets/satyaki-logo.svg";

/** Shared printable-report components. All styling lives in reports.css, scoped under .rm-report. */

export function ReportHeader({ subtitle }: { subtitle: string }) {
  return (
    <header className="run">
      <div className="brand">
        <div>
          <b>RigMind<sup className="tm">™</sup></b>
          <span>{subtitle}</span>
        </div>
      </div>
      <div className="maker">
        <div>
          <b>SATYAKI SOLUTIONS</b>Creating Innovative Impulse
        </div>
        <img src={satyakiLogo} alt="Satyaki Solutions logo" />
      </div>
    </header>
  );
}

export function ReportFooter({ page }: { page: string }) {
  return (
    <footer className="run">
      <span>satyaki.co.in</span>
      <span>Confidential · prepared for rig operations and HSE</span>
      <span className="pg">{page}</span>
    </footer>
  );
}

/** "NN / TT" page labels for a report with `total` sheets starting at `start`. */
export function pages(start: number, total: number, count: number): string[] {
  return Array.from({ length: count }, (_, i) => {
    const n = String(start + i).padStart(2, "0");
    const t = String(total).padStart(2, "0");
    return `${n} / ${t}`;
  });
}

export function MetaStrip({ items }: { items: [string, ReactNode][] }) {
  return (
    <dl className="meta">
      {items.map(([label, value]) => (
        <div key={label}>
          <dt>{label}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

export function KpiGrid({ cols = 4, children }: { cols?: 3 | 4; children: ReactNode }) {
  return <div className={`kpis${cols === 3 ? " three" : ""}`}>{children}</div>;
}

export function Kpi({ label, value, delta, tone }: { label: string; value: ReactNode; delta?: ReactNode; tone?: "up" | "down" | undefined }) {
  return (
    <div className="kpi">
      <div className="label">{label}</div>
      <div className="val">{value}</div>
      {delta != null && <div className={`delta${tone ? ` ${tone}` : ""}`}>{delta}</div>}
    </div>
  );
}

export function SeverityPill({ sev }: { sev: "Critical" | "Warning" | "Advisory" }) {
  const tone = sev === "Critical" ? "r" : sev === "Warning" ? "o" : "y";
  return <span className={`pill ${tone}`}>{sev}</span>;
}

export function StatusPill({ tone, children }: { tone: "g" | "b" | "n" | "o" | "r" | "y"; children: ReactNode }) {
  return <span className={`pill ${tone}`}>{children}</span>;
}

export function DataTable({ head, children }: { head: ReactNode[]; children: ReactNode }) {
  return (
    <div className="tbl-wrap">
      <table>
        <thead>
          <tr>{head}</tr>
        </thead>
        <tbody>{children}</tbody>
      </table>
    </div>
  );
}

const SERIES_COLORS = ["#FCA719", "#EE6A1F", "#E74C3C"];

export function SeriesLegend() {
  const labels = ["Advisory", "Warning", "Critical"];
  return (
    <div className="legend">
      {labels.map((l, i) => (
        <span key={l}>
          <i style={{ background: SERIES_COLORS[i] }} />
          {l}
        </span>
      ))}
    </div>
  );
}

/** Grouped bar chart, three bars per group. series = [advisory, warning, critical]. */
export function GroupedBarChart({ groups, series, ticks }: { groups: string[]; series: [number[], number[], number[]]; ticks: number[] }) {
  const L = 44, R = 744, T = 20, B = 260;
  const max = ticks[ticks.length - 1] ?? 1;
  const gw = (R - L) / groups.length;
  const h = (v: number) => (v / max) * (B - T);
  return (
    <div className="chart-wrap">
      <svg viewBox="0 0 760 300" style={{ minWidth: 520, width: "100%" }} role="img" aria-label="Grouped bar chart">
        {ticks.map((t) => {
          const y = B - h(t);
          return (
            <g key={t}>
              <line x1={L} x2={R} y1={y} y2={y} stroke={t === 0 ? "#101010" : "#E8E8ED"} strokeWidth={t === 0 ? 1.5 : 1} />
              <text x={L - 8} y={y + 4} textAnchor="end" fontSize={11} fill="#4A4A5A">
                {t}
              </text>
            </g>
          );
        })}
        {groups.map((label, i) => {
          const center = L + gw * (i + 0.5);
          const x0 = center - 31; // 3 bars × 18px + 2 × 4px gaps = 62
          const values = series.map((s) => s[i] ?? 0);
          const total = (values[0] ?? 0) + (values[1] ?? 0) + (values[2] ?? 0);
          return (
            <g key={label}>
              {values.map((v, si) => {
                const bh = h(v);
                const x = x0 + si * 22;
                return (
                  <g key={si}>
                    <rect x={x} y={B - bh} width={18} height={bh} rx={2} fill={SERIES_COLORS[si]} />
                    <text x={x + 9} y={B - bh - 4} textAnchor="middle" fontSize={11} fontWeight={600} fill="#101010">
                      {v}
                    </text>
                  </g>
                );
              })}
              <text x={center} y={B + 20} textAnchor="middle" fontSize={12} fill="#4A4A5A">
                {label}
              </text>
              <text x={center} y={B + 36} textAnchor="middle" fontSize={11} fontWeight={500} fill="#4A4A5A">
                {total} total
              </text>
            </g>
          );
        })}
      </svg>
    </div>
  );
}

export function Findings({ items }: { items: { title: string; text: string }[] }) {
  return (
    <div className="findings">
      {items.map((f) => (
        <div className="finding" key={f.title}>
          <b>{f.title}</b>
          <p>{f.text}</p>
        </div>
      ))}
    </div>
  );
}

export function ActionNote({ children }: { children: ReactNode }) {
  return <div className="action">{children}</div>;
}

export function InfoNote({ children }: { children: ReactNode }) {
  return <div className="note">{children}</div>;
}

export function Timeline({ items }: { items: { time: string; text: ReactNode }[] }) {
  return (
    <ul className="timeline">
      {items.map((t, i) => (
        <li key={i}>
          <time>{t.time}</time>
          <span>{t.text}</span>
        </li>
      ))}
    </ul>
  );
}

export function Checklist({ items }: { items: { done: boolean; text: string; note: string }[] }) {
  return (
    <ul className="checklist">
      {items.map((c) => (
        <li key={c.text}>
          <span className={`box${c.done ? " on" : ""}`}>{c.done ? "✓" : ""}</span>
          <span>{c.text}</span>
          <span className="muted">{c.note}</span>
        </li>
      ))}
    </ul>
  );
}

export function SignatureRow({ items }: { items: { name: ReactNode; caption: string }[] }) {
  return (
    <div className="sign">
      {items.map((s, i) => (
        <div key={i}>
          <b>{s.name}</b>
          {s.caption}
        </div>
      ))}
    </div>
  );
}

export function TypeBars({ items }: { items: { label: string; count: number }[] }) {
  const max = Math.max(...items.map((i) => i.count), 1);
  return (
    <div className="bars">
      {items.map((i) => (
        <div className="bar-row" key={i.label}>
          <span>{i.label}</span>
          <span className="bar-track">
            <span className="bar-fill" style={{ width: `${(i.count / max) * 100}%` }} />
          </span>
          <strong>{i.count}</strong>
        </div>
      ))}
    </div>
  );
}

export function ProgressBar({ pct }: { pct: number }) {
  return (
    <div className="prog">
      <i style={{ width: `${pct}%` }} />
    </div>
  );
}
