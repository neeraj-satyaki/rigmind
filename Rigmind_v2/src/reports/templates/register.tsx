import { DataTable, InfoNote, MetaStrip, ReportFooter, ReportHeader, SeverityPill, StatusPill } from "../components";
import { registerRows } from "@/data/mock";
import { exportCsv } from "@/lib/demo-export";

export function downloadRegisterCsv() {
  exportCsv(
    "rigmind-event-register",
    registerRows.map((r) => ({
      "Event ID": r.id,
      Occurred: r.occurred,
      Severity: r.severity,
      "Event type": r.type,
      "Rig · area": r.location,
      "Review status": r.review,
      Owner: r.owner,
    })),
  );
}

export function RegisterReport({ page }: { page: string }) {
  return (
    <section className="sheet" id="register">
      <ReportHeader subtitle="Event register · 06 Oct 2026" />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">Event register · export</div>
          <h1>Verified events, 06 Oct 2026</h1>
        </div>
        <MetaStrip
          items={[
            ["Period", "06 Oct 2026 · 00:00–23:59 AST"],
            ["Rigs", "All assigned rigs"],
            ["Filters", "All severities · all statuses"],
            ["Exported by", "Faisal A. · 10:02 AST"],
          ]}
        />
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
        <p className="muted" style={{ fontSize: 12 }}>
          11 of 42 records shown on this page. Test events are excluded. The full register is
          attached as CSV with one row per event and the same columns.
        </p>
        <InfoNote>
          <b>Integrity. </b>Detection data in this register is never edited. Review status and owner
          reflect the latest review decision; earlier values are in the audit trail.
        </InfoNote>
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
