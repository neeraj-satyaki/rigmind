import type { jsPDF as JsPDF } from "jspdf";
import { eventHistory, severityTrend, weeklyReportMeta as meta } from "@/data/mock";
import type { StoredSummary } from "./event-review-store";

type RGB = [number, number, number];
const ink: RGB = [23, 33, 49];
const muted: RGB = [74, 85, 102];
const line: RGB = [220, 226, 233];
const panel: RGB = [244, 245, 247];
const accent: RGB = [245, 166, 35];
/* Satyaki's own brand colours, lifted from the logo artwork. */
const brand: RGB = [156, 2, 16];
const brandSub: RGB = [138, 106, 0];

const tier: Record<"critical" | "warning" | "advisory", RGB> = {
  critical: [180, 53, 30],
  warning: [217, 163, 90],
  advisory: [138, 173, 224],
};
const good: RGB = [29, 110, 69];
const bad: RGB = [180, 53, 30];

export function weeklyTotals() {
  const days = severityTrend.map((d) => ({ ...d, total: d.critical + d.warning + d.advisory }));
  const sum = (k: "critical" | "warning" | "advisory") => days.reduce((a, d) => a + d[k], 0);
  const critical = sum("critical"),
    warning = sum("warning"),
    advisory = sum("advisory");
  const total = critical + warning + advisory;
  const peak = days.reduce((a, d) => (d.total > a.total ? d : a), days[0]!);
  return { days, critical, warning, advisory, total, peak };
}

const pct = (a: number, b: number) => `${(((a - b) / b) * 100).toFixed(1)}%`;

export async function generateWeeklyReport(opts: {
  approved: StoredSummary[];
  preparedBy: string;
  preparedEmail: string;
  sections: string[];
}) {
  const { jsPDF } = await import("jspdf");
  const pdf: JsPDF = new jsPDF({ unit: "mm", format: "a4" });
  const W = 210,
    M = 14;
  const t = weeklyTotals();
  const color = (c: RGB) => pdf.setTextColor(c[0], c[1], c[2]);
  const fill = (c: RGB) => pdf.setFillColor(c[0], c[1], c[2]);
  const stroke = (c: RGB) => pdf.setDrawColor(c[0], c[1], c[2]);
  const font = (size: number, style: "normal" | "bold" = "normal", c: RGB = ink) => {
    pdf.setFont("helvetica", style);
    pdf.setFontSize(size);
    color(c);
  };
  const wrap = (text: string, x: number, y: number, w: number, lh = 4.6) => {
    const lines = pdf.splitTextToSize(text, w) as string[];
    pdf.text(lines, x, y);
    return y + lines.length * lh;
  };

  const header = () => {
    font(13, "bold");
    pdf.text("RigMind", M, 16);
    font(8.5, "normal", muted);
    pdf.text("Rig safety operations", M, 21);
    font(9.5, "bold", brand);
    pdf.text("SATYAKI SOLUTIONS", W - M, 16, { align: "right" });
    font(8.5, "normal", brandSub);
    pdf.text("Creating Innovative Impulse", W - M, 21, { align: "right" });

    fill(accent);
    pdf.rect(0, 26, W, 0.9, "F");
  };
  const eyebrow = (text: string, y: number) => {
    font(8, "bold", accent);
    pdf.setCharSpace(0.6);
    pdf.text(text.toUpperCase(), M, y);
    pdf.setCharSpace(0);
  };

  // ---- Page 1: overview
  header();
  eyebrow(`Weekly safety events report · Week ${meta.week}`, 37);
  const critDelta = t.critical - meta.previous.critical;
  font(20, "bold");
  let y = wrap(
    `Critical events ${critDelta <= 0 ? "down" : "up"} to ${t.critical} this week, ${t.total} verified in total`,
    M,
    47,
    150,
    8,
  );
  y += 2;
  fill(panel);
  pdf.roundedRect(M, y, W - 2 * M, 17, 2, 2, "F");
  const metaCols: [string, string][] = [
    ["Reporting period", meta.period],
    ["Asset", meta.asset],
    ["Cameras", meta.cameras],
    ["Monitoring", meta.monitored],
  ];
  metaCols.forEach(([k, v], i) => {
    const x = M + 4 + i * 45.5;
    font(6.5, "normal", muted);
    pdf.text(k.toUpperCase(), x, y + 6);
    font(8.5, "normal");
    pdf.text(pdf.splitTextToSize(v, 43) as string[], x, y + 11);
  });
  y += 25;
  font(9.5);
  y = wrap(
    `RigMind recorded ${t.total} verified events this week, ${pct(t.total, meta.previous.total).replace("-", "")} ${t.total < meta.previous.total ? "fewer" : "more"} than Week ${meta.week - 1} (${meta.previous.total}). Critical events moved from ${meta.previous.critical} to ${t.critical}; ${t.peak.day} peaked with ${t.peak.total} events. ${opts.approved.length} event summar${opts.approved.length === 1 ? "y has" : "ies have"} been approved by HSE for this report.`,
    M,
    y,
    W - 2 * M - 30,
    5,
  );
  y += 5;
  const cards: [string, string, string, boolean][] = [
    ["Total events", String(t.total), `${pct(t.total, meta.previous.total)} vs W${meta.week - 1} (${meta.previous.total})`, t.total <= meta.previous.total],
    ["Critical events", String(t.critical), `${critDelta >= 0 ? "+" : ""}${critDelta} vs W${meta.week - 1} (${meta.previous.critical})`, critDelta <= 0],
    ["Median acknowledgement", `${meta.current.medianAckS} s`, `${(meta.current.medianAckS - meta.previous.medianAckS).toFixed(1)} s vs W${meta.week - 1}`, meta.current.medianAckS <= meta.previous.medianAckS],
    ["Verified false-positive rate", `${meta.current.falsePositivePct}%`, `${(meta.current.falsePositivePct - meta.previous.falsePositivePct).toFixed(1)} pts vs W${meta.week - 1}`, meta.current.falsePositivePct <= meta.previous.falsePositivePct],
  ];
  const cw = (W - 2 * M - 9) / 4;
  cards.forEach(([k, v, d, ok], i) => {
    const x = M + i * (cw + 3);
    stroke(line);
    pdf.roundedRect(x, y, cw, 27, 2, 2, "S");
    font(7.5, "normal", muted);
    pdf.text(k, x + 4, y + 7);
    font(17, "bold");
    pdf.text(v, x + 4, y + 17);
    font(7, "normal", ok ? good : bad);
    pdf.text(`${ok ? "▼" : "▲"} ${d}`.replace(/[▼▲] /, ok ? "Down " : "Up "), x + 4, y + 23);
  });
  y += 34;

  // Chart
  const ch = 88;
  stroke(line);
  pdf.roundedRect(M, y, W - 2 * M, ch, 2, 2, "S");
  font(11, "bold");
  pdf.text("Events per day by severity", M + 5, y + 9);
  font(8, "normal", muted);
  pdf.text(`${t.peak.day} peaked at ${t.peak.total} events`, M + 5, y + 14.5);
  (["advisory", "warning", "critical"] as const).forEach((k, i) => {
    const lx = W - M - 75 + i * 25;
    fill(tier[k]);
    pdf.rect(lx, y + 6.5, 3, 3, "F");
    font(7.5, "normal", muted);
    pdf.text(k[0]!.toUpperCase() + k.slice(1), lx + 4.5, y + 9);
  });
  const base = y + ch - 16,
    top = y + 24,
    maxV = 25;
  const px = M + 14,
    pw = W - 2 * M - 22;
  for (let v = 0; v <= maxV; v += 5) {
    const gy = base - ((base - top) * v) / maxV;
    stroke(line);
    pdf.line(px, gy, px + pw, gy);
    font(7, "normal", muted);
    pdf.text(String(v), px - 3, gy + 1, { align: "right" });
  }
  const slot = pw / t.days.length,
    bw = 5;
  t.days.forEach((d, i) => {
    const cx = px + slot * i + slot / 2;
    (["advisory", "warning", "critical"] as const).forEach((k, j) => {
      const h = ((base - top) * d[k]) / maxV;
      const x = cx - bw * 1.5 - 0.6 + j * (bw + 0.6);
      fill(tier[k]);
      pdf.rect(x, base - h, bw, h, "F");
      font(6.5, "bold");
      pdf.text(String(d[k]), x + bw / 2, base - h - 1.2, { align: "center" });
    });
    font(7.5, "normal", muted);
    pdf.text(d.day, cx, base + 5, { align: "center" });
    font(7.5, "bold");
    pdf.text(`${d.total} total`, cx, base + 9.5, { align: "center" });
  });

  // ---- Page 2: tiers & log
  pdf.addPage();
  header();
  eyebrow("Overview", 37);
  font(18, "bold");
  pdf.text("All events by severity", M, 46);
  const tierRows = [
    ["Advisory", "Observation requiring review (PPE, posture)", t.advisory],
    ["Warning", "Equipment proximity or restricted-zone entry", t.warning],
    ["Critical", "Person in active movement zone", t.critical],
  ] as const;
  const table = (heads: string[], widths: number[], rows: string[][], startY: number, tones?: (RGB | null)[]) => {
    let ty = startY;
    font(6.8, "bold", muted);
    let x = M;
    heads.forEach((h, i) => {
      pdf.text(h.toUpperCase(), x, ty);
      x += widths[i]!;
    });
    ty += 2.5;
    stroke(line);
    pdf.line(M, ty, W - M, ty);
    rows.forEach((r, ri) => {
      const cells = r.map((c, i) => pdf.splitTextToSize(c, widths[i]! - 3) as string[]);
      const rh = Math.max(...cells.map((c) => c.length)) * 4 + 3.5;
      x = M;
      cells.forEach((c, i) => {
        const tone = i === 0 ? tones?.[ri] : null;
        font(8.2, i === 0 || i === 2 ? "bold" : "normal", tone ?? ink);
        pdf.text(c, x, ty + 5);
        x += widths[i]!;
      });
      ty += rh;
      pdf.line(M, ty, W - M, ty);
    });
    return ty;
  };
  y = table(
    ["Severity", "Trigger condition", `W${meta.week}`, "Share"],
    [30, 98, 25, 29],
    tierRows.map(([n, c, v]) => [n, c, String(v), `${((v / t.total) * 100).toFixed(1)}%`]),
    56,
    [tier.advisory, tier.warning, tier.critical],
  );
  font(13, "bold");
  pdf.text("Critical and escalated events log", M, y + 12);
  const logRows = eventHistory.filter((e) => e.severity !== "Advisory");
  const approvedIds = new Set(opts.approved.map((s) => s.eventId));
  y = table(
    ["Event ID", "Time", "Severity", "Trigger", "Review", "HSE summary"],
    [24, 22, 22, 50, 30, 34],
    logRows.map((e) => [e.id, `06 Oct ${e.occurred.slice(0, 5)}`, e.severity, e.type, e.review, approvedIds.has(e.id) ? "Approved" : "Not approved"]),
    y + 20,
    logRows.map((e) => (e.severity === "Critical" ? tier.critical : tier.warning)),
  );
  const insights: [string, string][] = [
    ["Peak day", `${t.peak.day} recorded ${t.peak.total} events, the highest this week.`],
    ["Response time", `Median acknowledgement ${meta.current.medianAckS} s, compared with ${meta.previous.medianAckS} s last week.`],
    ["Data quality", "Detection quality and alert latency are not yet measured. The alarm interface is simulated."],
  ];
  y += 8;
  const iw = (W - 2 * M - 8) / 3;
  insights.forEach(([h, b], i) => {
    const x = M + i * (iw + 4);
    fill(panel);
    pdf.roundedRect(x, y, iw, 30, 2, 2, "F");
    fill(accent);
    pdf.rect(x, y, 1, 30, "F");
    font(9, "bold");
    pdf.text(h, x + 4, y + 7);
    font(8, "normal", muted);
    wrap(b, x + 4, y + 12.5, iw - 7, 4);
  });
  y += 38;
  if (opts.sections.length) {
    font(9, "bold");
    pdf.text("Included sections", M, y);
    font(8, "normal", muted);
    wrap(opts.sections.join(" · "), M, y + 5, W - 2 * M);
  }

  // ---- Event detail pages: HSE-approved summaries only
  const approved = opts.approved;
  if (!approved.length) {
    pdf.addPage();
    header();
    eyebrow("Event details", 37);
    font(16, "bold");
    pdf.text("No HSE-approved event summaries this week", M, 47);
    font(9.5, "normal", muted);
    wrap("AI draft summaries appear in this report only after an HSE reviewer approves them on the event investigation page.", M, 56, W - 2 * M, 5);
  }
  approved.forEach((s, i) => {
    const ev = eventHistory.find((e) => e.id === s.eventId);
    pdf.addPage();
    header();
    eyebrow(`Event detail ${i + 1} of ${approved.length}`, 37);
    font(11, "bold");
    pdf.text(s.eventId, M, 45);
    if (ev) {
      const sev = ev.severity.toLowerCase() as keyof typeof tier;
      font(8, "bold", tier[sev] ?? ink);
      pdf.text(ev.severity, M + 22, 45);
    }
    font(17, "bold");
    let dy = wrap(ev?.type ?? "Event summary", M, 54, W - 2 * M, 7) + 2;
    fill(panel);
    pdf.roundedRect(M, dy, W - 2 * M, 14, 2, 2, "F");
    (
      [
        ["Time", ev ? `06 Oct 2026 · ${ev.occurred}` : "Not recorded"],
        ["Review status", ev?.review ?? "Not recorded"],
        ["Draft generated", s.generatedAt],
        ["Camera", s.sources.find((x) => x.startsWith("Evidence"))?.split(", ")[1] ?? "Not recorded"],
      ] as [string, string][]
    ).forEach(([k, v], j) => {
      const x = M + 4 + j * 45.5;
      font(6.5, "normal", muted);
      pdf.text(k.toUpperCase(), x, dy + 5.5);
      font(8.2);
      pdf.text(pdf.splitTextToSize(v, 43)[0] as string, x, dy + 10.5);
    });
    dy += 22;
    const section = (title: string, items: string[], tone: RGB = ink) => {
      if (dy > 260) {
        pdf.addPage();
        header();
        dy = 38;
      }
      font(11, "bold");
      pdf.text(title, M, dy);
      dy += 6;
      items.forEach((it) => {
        font(9, "normal", tone);
        pdf.text("•", M, dy);
        dy = wrap(it, M + 4, dy, W - 2 * M - 4, 4.6) + 1.2;
      });
      dy += 4;
    };
    section("What happened", [s.draft.summary]);
    section("Recorded facts", s.draft.recordedFacts);
    section("Missing information", s.draft.missingInformation, [138, 83, 0]);
    section("Source references", s.sources, muted);
    if (s.approval) {
      fill(panel);
      pdf.roundedRect(M, dy, W - 2 * M, 16, 2, 2, "F");
      fill(good);
      pdf.rect(M, dy, 1, 16, "F");
      font(9, "bold");
      pdf.text("HSE approval", M + 4, dy + 6);
      font(8.5, "normal", muted);
      pdf.text(`Approved by ${s.approval.by} (${s.approval.role}) on ${s.approval.at}. AI-drafted; reviewed before inclusion.`, M + 4, dy + 11.5);
    }
  });

  // Footers
  const n = pdf.getNumberOfPages();
  for (let p = 1; p <= n; p++) {
    pdf.setPage(p);
    stroke(line);
    pdf.line(M, 284, W - M, 284);
    font(7.5, "normal", muted);
    pdf.text(meta.website, M, 289);
    pdf.text(`${meta.confidentiality} · Prepared by ${opts.preparedBy} (${opts.preparedEmail})`, W / 2, 289, { align: "center" });
    pdf.text(`${String(p).padStart(2, "0")} / ${String(n).padStart(2, "0")}`, W - M, 289, { align: "right" });
  }
  pdf.save(`RigMind_W${meta.week}_Weekly_Events_Report.pdf`);
}
