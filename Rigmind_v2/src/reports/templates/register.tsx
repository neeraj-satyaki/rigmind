import { DataTable, InfoNote, MetaStrip, ReportFooter, ReportHeader, SeverityPill, StatusPill } from "../components";
import { registerRows } from "@/data/mock";
import { useEffect, useState } from "react";
import { exportCsv } from "@/lib/demo-export";
import { loadPlainSummaries, plainSummaryCsvRows, type PlainSummary } from "@/lib/plain-summary";
import { PlainSummaryTable } from "../PlainSummaryTable";
import { demoDataEnabled } from "@/lib/demo-mode";
import { loadAlertFeed } from "@/lib/real-data";
import { RealRegisterTable, registerRowsFrom } from "../real-templates";

export function registerCsvRows(plain: PlainSummary[]) {
  const summaryOf = new Map(plain.map((s) => [s.alertId, s.text]));
  return registerRows.map((r) => ({
    "Event ID": r.id,
    Occurred: r.occurred,
    Severity: r.severity,
    "Event type": r.type,
    "Rig · area": r.location,
    "Review status": r.review,
    Owner: r.owner,
    "Plain-language summary": summaryOf.get(r.id) ?? "",
  }));
}

export async function downloadRegisterCsv() {
  const plain = await loadPlainSummaries().catch(() => []);
  if (!demoDataEnabled()) {
    const summaryOf = new Map(plain.map((s) => [s.alertId, s.text]));
    const rows = registerRowsFrom(await loadAlertFeed(500).catch(() => []));
    exportCsv("rigmind-event-register", rows.map((r) => ({ "Event ID": r.id, Occurred: r.occurred, Severity: r.severity, "Event type": r.type, Area: r.location, Status: r.review, Owner: r.owner, "Plain-language summary": summaryOf.get(r.id) ?? "" })));
    if (plain.length) exportCsv("rigmind-plain-language-summaries", plainSummaryCsvRows(plain));
    return;
  }
  exportCsv("rigmind-event-register", registerCsvRows(plain));
  // Alerts raised on this device are not in the demonstration register: export them as their own file.
  if (plain.length) exportCsv("rigmind-plain-language-summaries", plainSummaryCsvRows(plain));
}

export function RegisterReport({ page }: { page: string }) {
  const demo = demoDataEnabled();
  const [plain, setPlain] = useState<PlainSummary[]>([]);
  const [feed, setFeed] = useState<Awaited<ReturnType<typeof loadAlertFeed>> | null>(null);
  useEffect(() => {
    loadPlainSummaries().then(setPlain).catch(() => {});
    if (!demo) loadAlertFeed(500).then(setFeed).catch(() => setFeed([]));
  }, [demo]);
  const real = registerRowsFrom(feed ?? []);
  return (
    <section className="sheet" id="register">
      <ReportHeader subtitle={demo ? "Event register · 06 Oct 2026" : "Event register"} />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">Event register · export</div>
          <h1>{demo ? "Verified events, 06 Oct 2026" : feed === null ? "Loading…" : `${real.length} alerts recorded on this device`}</h1>
        </div>
        <MetaStrip
          items={
            demo
              ? [
                  ["Period", "06 Oct 2026 · 00:00–23:59 AST"],
                  ["Rigs", "All assigned rigs"],
                  ["Filters", "All severities · all statuses"],
                  ["Exported by", "Faisal A. · 10:02 AST"],
                ]
              : [
                  ["Period", real.length ? `${real[real.length - 1]!.occurred} – ${real[0]!.occurred}` : "No alerts"],
                  ["Rigs", "This rig"],
                  ["Filters", "All severities · all statuses"],
                  ["Source", "Alerts raised on this device (latest 500)"],
                ]
          }
        />
        {!demo && feed !== null && real.length === 0 && <div className="kpi-box"><h3>No data</h3><p className="muted" style={{ margin: 0 }}>No data for this period. No alerts have been recorded yet.</p></div>}
        {!demo ? (
          real.length > 0 && <RealRegisterTable rows={real} />
        ) : (
        <DataTable
          head={[
            <th key="i">Event ID</th>,
            <th key="o">Occurred</th>,
            <th key="s">Severity</th>,
            <th key="t">Event type</th>,
            <th key="r">Rig · area</th>,
            <th key="rs">Review status</th>,
            <th key="ow">Owner</th>,
          ]}
        >
          {registerRows.map((r) => (
            <tr key={r.id}>
              <td>{r.id}</td>
              <td>{r.occurred}</td>
              <td><SeverityPill sev={r.severity} /></td>
              <td>{r.type}</td>
              <td>{r.location}</td>
              <td><StatusPill tone={r.tone}>{r.review}</StatusPill></td>
              <td>{r.owner}</td>
            </tr>
          ))}
        </DataTable>
        )}
        <p className="muted" style={{ fontSize: 12 }}>
          {demo
            ? "11 of 42 records shown on this page. Test events are excluded. The full register is attached as CSV with one row per event and the same columns."
            : "Alerts are listed as the detector raised them, newest first. Use Download CSV for a file with the same columns."}
        </p>
        {plain.length > 0 && (
          <>
            <h3 style={{ margin: 0 }}>Plain-language summaries of alerts raised on this device</h3>
            <PlainSummaryTable rows={plain} />
          </>
        )}
        <InfoNote>
          <b>Integrity. </b>Detection data in this register is never edited. Review status and owner
          reflect the latest review decision; earlier values are in the audit trail.
        </InfoNote>
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
