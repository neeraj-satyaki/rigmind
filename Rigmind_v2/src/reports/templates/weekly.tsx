import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "@tanstack/react-router";
import {
  DataTable, Findings, GroupedBarChart, InfoNote, Kpi, KpiGrid, MetaStrip,
  ReportFooter, ReportHeader, SeriesLegend, StatusPill, TypeBars,
} from "../components";
import { availableWeeks, computeWeekly, loadEventLog, weekLabel, type Comparison, type LoggedEvent, type WeeklyStats } from "@/lib/weekly-events";
import { loadApprovedSummaries, type StoredSummary } from "@/lib/event-review-store";

const one = (n: number) => n.toFixed(1).replace(/\.0$/, "");
const vs = (c: Comparison, unit = "") =>
  !c.available ? "Prior-week comparison unavailable"
  : `${c.change > 0 ? "+" : ""}${c.change}${unit} vs ${c.previous}${unit} prior week${c.pct === null ? "" : ` (${c.pct > 0 ? "+" : ""}${one(c.pct)}%)`}`;
const when = (at: string) => { const d = new Date(at); return Number.isNaN(d.getTime()) ? at : d.toLocaleString("en-GB", { day: "2-digit", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" }); };

const pad2 = (n: number) => String(n).padStart(2, "0");
type PageOf = (index: number, sheets: number) => string;
const ownPages: PageOf = (i, n) => `${pad2(i + 1)} / ${pad2(n)}`;

export function WeeklyReport({ week, pageOf = ownPages, onSheets }: { week?: string | undefined; pageOf?: PageOf; onSheets?: (sheets: number) => void }) {
  const [events, setEvents] = useState<LoggedEvent[] | null>(null);
  const [summaries, setSummaries] = useState<StoredSummary[]>([]);
  const [error, setError] = useState<string | null>(null);
  const navigate = useNavigate();
  useEffect(() => {
    Promise.all([loadEventLog(), loadApprovedSummaries()])
      .then(([e, s]) => { setEvents(e); setSummaries(s); })
      .catch(() => setError("Stored events could not be read on this device."));
  }, []);
  const weeks = useMemo(() => (events ? availableWeeks(events) : []), [events]);
  const selected = week && weeks.includes(week) ? week : weeks[0];
  const stats = useMemo(() => (events && selected ? computeWeekly(events, selected) : null), [events, selected]);
  const summaryPages = useMemo(
    () => paginateSummaries(stats ? summaries.filter((s) => stats.ids.has(s.eventId)) : []),
    [stats, summaries],
  );
  const sheets = 2 + summaryPages.length;
  useEffect(() => { onSheets?.(sheets); }, [sheets, onSheets]);
  const p1 = pageOf(0, sheets);
  const p2 = pageOf(1, sheets);

  if (error || !stats) {
    return (
      <section className="sheet" id="weekly">
        <ReportHeader subtitle="Weekly fleet safety report" />
        <div className="sheet-body"><p className="muted">{error ?? (events ? "No stored events yet." : "Loading stored events…")}</p></div>
        <ReportFooter page={p1} />
      </section>
    );
  }

  const subtitle = `Weekly fleet safety report · ${stats.period}`;
  const maxDay = Math.max(...stats.series.flatMap((s) => s), 1);
  const step = Math.max(1, Math.ceil(maxDay / 5 / 5) * 5);
  const ticks = Array.from({ length: Math.ceil(maxDay / step) + 1 }, (_, i) => i * step);

  return (
    <>
      <div className="rm-weekpick">
        <label>
          Reporting week{" "}
          <select
            aria-label="Reporting week"
            value={selected}
            onChange={(e) => navigate({ to: ".", search: (p: Record<string, unknown>) => ({ ...p, week: e.target.value }) })}
          >
            {weeks.map((w) => <option key={w} value={w}>Week {weekLabel(w)} · {computeWeekly(events!, w).period}</option>)}
          </select>
        </label>
        <span>Calculated from {events!.length} events stored on this device.</span>
      </div>
      <section className="sheet" id="weekly">
        <ReportHeader subtitle={subtitle} />
        <div className="sheet-body">
          <div>
            <div className="eyebrow">Weekly fleet safety report · Week {stats.week}</div>
            <h1>
              {stats.busiestDay.label} was the busiest day of the week, with {stats.busiestDay.total} events and {stats.busiestDay.critical} critical
            </h1>
          </div>
          <MetaStrip items={[["Period", stats.period], ["Fleet", "Demo rigs 01–04"], ["Source", "Device event log"], ["Prepared for", "Operations and HSE leads"]]} />
          <p>
            RigMind verified <b>{stats.total} events</b> across the fleet this week: {stats.advisory} advisory, {stats.warning} warning
            and {stats.critical} critical.{" "}
            {stats.hasPrev
              ? <>Compared with the prior week, total events changed by {vs(stats.vsPrev.total).split(" vs")[0]} and critical events by {vs(stats.vsPrev.critical).split(" vs")[0]}.</>
              : <>No events are stored for the prior week, so week-on-week comparisons are unavailable.</>}{" "}
            Read the counts alongside monitoring coverage.
          </p>
          <KpiGrid>
            <Kpi label="Verified events" value={String(stats.total)} delta={<>Avg {one(stats.avgPerDay)} per day · {vs(stats.vsPrev.total)}</>} />
            <Kpi label="Critical events" value={String(stats.critical)} delta={<>{one(stats.criticalShare)}% of all events · {vs(stats.vsPrev.critical)}</>} tone={stats.vsPrev.critical.available && stats.vsPrev.critical.change > 0 ? "up" : undefined} />
            <Kpi
              label="Median acknowledgement"
              value={stats.medianAck === null ? "—" : <span>{one(stats.medianAck)}<small> s</small></span>}
              delta={<>P95 {stats.p95Ack ?? "—"} s · {stats.acknowledged} acknowledged · {stats.vsPrev.medianAck.available ? `prior week ${one(stats.vsPrev.medianAck.previous)} s` : "prior week unavailable"}</>}
            />
            <Kpi label="Open events" value={String(stats.rigRows.reduce((a, r) => a + r.open, 0))} delta="Not yet closed in review" />
          </KpiGrid>
          <div className="chart-card">
            <div className="chart-head">
              <div>
                <h3 style={{ margin: 0 }}>Verified events per day by severity</h3>
                <div className="muted" style={{ fontSize: 12 }}>Critical events peaked at {stats.criticalPeak.n} on {stats.criticalPeak.label}</div>
              </div>
              <SeriesLegend />
            </div>
            <GroupedBarChart groups={stats.labels} series={stats.series} ticks={ticks} />
          </div>
        </div>
        <ReportFooter page={p1} />
      </section>

      <section className="sheet">
        <ReportHeader subtitle={subtitle} />
        <div className="sheet-body">
          <div>
            <div className="eyebrow">Weekly fleet safety report · Breakdown</div>
            <h2>Rigs, event types and busy hours</h2>
          </div>
          <DataTable head={[<th key="r">Rig</th>, <th key="g">Region</th>, <th key="rd">Readiness</th>, <th key="c" className="r">Critical</th>, <th key="w" className="r">Warning</th>, <th key="a" className="r">Advisory</th>, <th key="o" className="r">Open</th>]}>
            {stats.rigRows.map((r) => (
              <tr key={r.rig}>
                <td>{r.rig}</td><td>{r.region}</td>
                <td><StatusPill tone={r.readiness === "Ready" ? "g" : "o"}>{r.readiness}</StatusPill></td>
                <td className="r">{r.critical}</td><td className="r">{r.warning}</td><td className="r">{r.advisory}</td><td className="r">{r.open}</td>
              </tr>
            ))}
          </DataTable>
          <div className="two">
            <div className="kpi-box">
              <h3>Events by type · {stats.busiestDay.label}</h3>
              <TypeBars items={stats.typeBars} />
            </div>
            <div className="kpi-box">
              <h3>Busiest hours (AST)</h3>
              <dl className="kv">
                <div><dt>Peak block</dt><dd>{stats.peak.label}</dd></div>
                <div><dt>Quietest block</dt><dd>{stats.quiet.label}</dd></div>
                <div><dt>Events in peak</dt><dd>{stats.peak.n} <small>of {stats.total}</small></dd></div>
                <div><dt>Night share 18–06</dt><dd>{Math.round(stats.nightShare)} <small>%</small></dd></div>
              </dl>
            </div>
          </div>
          <Findings items={findings(stats)} />
          {summaryPages.length === 0 ? (
            <div className="kpi-box">
              <h3>HSE-approved investigation summaries</h3>
              <p className="muted" style={{ margin: 0 }}>No investigation summaries for this week have HSE approval. Draft summaries are excluded.</p>
            </div>
          ) : (
            <p className="muted" style={{ margin: 0 }}>
              {summaryPages.flat().filter((b) => b.kind === "head").length} HSE-approved investigation summaries follow on page{summaryPages.length > 1 ? "s" : ""} {pageOf(2, sheets).split(" /")[0]}
              {summaryPages.length > 1 ? `–${pageOf(sheets - 1, sheets).split(" /")[0]}` : ""}. Draft summaries are excluded.
            </p>
          )}
          <InfoNote>
            <b>Reading the counts. </b>Lower event counts alone do not establish a safer operation.
            Compare them with usable monitoring time for each rig.
          </InfoNote>
        </div>
        <ReportFooter page={p2} />
      </section>

      {summaryPages.map((blocks, i) => (
        <section className="sheet" key={i}>
          <ReportHeader subtitle={subtitle} />
          <div className="sheet-body">
            <div>
              <div className="eyebrow">Weekly fleet safety report · HSE-approved summaries{i > 0 ? " (continued)" : ""}</div>
              <h2>Investigation summaries approved by HSE</h2>
            </div>
            <SummaryBlocks blocks={blocks} />
          </div>
          <ReportFooter page={pageOf(2 + i, sheets)} />
        </section>
      ))}
    </>
  );
}

function findings(s: WeeklyStats) {
  return [
    { title: `${s.topRig.rig} leads volume`, text: `${Math.round(s.topRig.share)}% of the week's events (${s.topRig.total} of ${s.total}) came from ${s.topRig.rig.toLowerCase()}.` },
    { title: "Peak risk window", text: `${s.peak.n} events fell between ${s.peak.label}, the busiest six-hour block of the week.` },
    s.hasPrev
      ? { title: "Week-on-week", text: `Total events ${vs(s.vsPrev.total)}; critical events ${vs(s.vsPrev.critical)}.` }
      : { title: "Comparison unavailable", text: "No events are stored for the prior week, so no trend is reported." },
  ];
}

/* ---------- Approved-summary pagination ----------
   Summaries are split into blocks (heading + text, each fact, sources + approval)
   and packed onto continuation sheets using a conservative height estimate for the
   1000px-wide printed A4 sheet, so nothing overflows the fixed print page. */
type Block =
  | { kind: "head"; s: StoredSummary; cont: boolean; text: string }
  | { kind: "fact"; s: StoredSummary; text: string }
  | { kind: "meta"; s: StoredSummary };

/** Usable body height on a continuation sheet (px at print width) after header, title and footer. */
export const SUMMARY_PAGE_BUDGET = 980;
const CHARS_PER_LINE = 125;
const lines = (t: string, w = CHARS_PER_LINE) => Math.max(1, Math.ceil(t.length / w));
const metaText = (s: StoredSummary) => `Sources: ${s.sources.join(" · ")}`;
export function blockHeight(b: Block) {
  if (b.kind === "head") return 44 + lines(b.text) * 23;
  if (b.kind === "fact") return 6 + lines(b.text, CHARS_PER_LINE - 6) * 22;
  return 30 + lines(metaText(b.s)) * 19 + 19 + 24;
}

function chunkText(t: string, maxLines: number) {
  const max = maxLines * CHARS_PER_LINE;
  const out: string[] = [];
  let rest = t.trim();
  while (rest.length > max) {
    let cut = rest.lastIndexOf(". ", max) + 1;
    if (cut < max * 0.6) cut = rest.lastIndexOf(" ", max);
    if (cut < max * 0.6) cut = max;
    out.push(rest.slice(0, cut).trim());
    rest = rest.slice(cut).trim();
  }
  out.push(rest);
  return out;
}

export function paginateSummaries(items: StoredSummary[], budget = SUMMARY_PAGE_BUDGET): Block[][] {
  const blocks: Block[] = [];
  for (const s of items) {
    chunkText(s.draft.summary, 6).forEach((text, i) => blocks.push({ kind: "head", s, cont: i > 0, text }));
    for (const f of s.draft.recordedFacts) for (const text of chunkText(f, 6)) blocks.push({ kind: "fact", s, text });
    blocks.push({ kind: "meta", s });
  }
  const pages: Block[][] = [];
  let cur: Block[] = [];
  let used = 0;
  for (const b of blocks) {
    const h = blockHeight(b);
    if (cur.length && used + h > budget) {
      pages.push(cur);
      cur = [];
      used = 0;
    }
    // A summary continued on a new sheet repeats its event heading for context.
    if (!cur.length && b.kind !== "head") {
      const head: Block = { kind: "head", s: b.s, cont: true, text: "" };
      cur.push(head);
      used += 44;
    }
    cur.push(b);
    used += h;
  }
  if (cur.length) pages.push(cur);
  return pages;
}

function SummaryBlocks({ blocks }: { blocks: Block[] }) {
  return (
    <div style={{ display: "flex", flexDirection: "column" }}>
      {blocks.map((b, i) => {
        if (b.kind === "head" && b.cont && i > 0 && blocks[i - 1]!.s === b.s)
          return <p key={i} style={{ margin: "0 0 4px", maxWidth: "none", overflowWrap: "anywhere" }}>{b.text}</p>;
        if (b.kind === "head")
          return (
            <div key={i} style={{ marginTop: i ? 16 : 0 }}>
              <b className="mono">{b.s.eventId}</b>
              {b.cont && <span className="muted" style={{ fontSize: 12 }}> · continued</span>}
              {b.text && <p style={{ margin: "4px 0", maxWidth: "none", overflowWrap: "anywhere" }}>{b.text}</p>}
            </div>
          );
        if (b.kind === "fact")
          return <div key={i} style={{ paddingLeft: 18, overflowWrap: "anywhere" }}>• {b.text}</div>;
        return (
          <div key={i} className="muted" style={{ fontSize: 12, marginTop: 6, paddingBottom: 8, borderBottom: "1px solid #E8E8ED", overflowWrap: "anywhere" }}>
            {metaText(b.s) === "Sources: " ? "Sources: None recorded" : metaText(b.s)}
            <br />
            Approved by {b.s.approval!.by} ({b.s.approval!.role}) on {when(b.s.approval!.at)}
          </div>
        );
      })}
    </div>
  );
}
