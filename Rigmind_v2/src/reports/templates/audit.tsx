import { DataTable, Findings, InfoNote, MetaStrip, ReportFooter, ReportHeader, StatusPill } from "../components";
import { auditReportFindings, auditReportRows } from "@/data/mock";
import { exportCsv } from "@/lib/demo-export";
import { demoDataEnabled } from "@/lib/demo-mode";
import { AUDIT_ACTIONS, auditActionLabel, auditActor, loadAuditEntries, stamp } from "@/lib/real-data";
import { RealAuditReport } from "../real-templates";

export async function downloadAuditCsv() {
  if (!demoDataEnabled()) {
    const rows = await loadAuditEntries(AUDIT_ACTIONS, 200);
    exportCsv("rigmind-audit", rows.map((r) => ({ Time: stamp(r.tsMs), Actor: auditActor(r.source), Action: auditActionLabel(r.action), Entity: r.target, Detail: r.detail })));
    return;
  }
  exportCsv(
    "rigmind-audit",
    auditReportRows.map((r) => ({
      "Time (AST)": r.time,
      Actor: r.actor,
      Action: r.action,
      Entity: r.entity,
      Result: r.result,
    })),
  );
}

export function AuditReport({ page }: { page: string }) {
  return demoDataEnabled() ? <AuditReportDemo page={page} /> : <RealAuditReport page={page} />;
}

function AuditReportDemo({ page }: { page: string }) {
  return (
    <section className="sheet" id="audit">
      <ReportHeader subtitle="Audit trail extract · EV-1042" />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">Audit trail extract</div>
          <h1>Every action on EV-1042 and what followed</h1>
        </div>
        <MetaStrip
          items={[
            ["Entity", "EV-1042 and linked items"],
            ["Date", "06 Oct 2026"],
            ["Time zone", "Asia/Riyadh"],
            ["Extracted by", "Audit viewer · read only"],
          ]}
        />
        <InfoNote>
          <b>Append-only record. </b>Original detection data and later review decisions are stored
          separately and can each be traced. Nothing in this log can be edited or deleted.
        </InfoNote>
        <DataTable
          head={[
            <th key="t">Time (AST)</th>,
            <th key="a">Actor</th>,
            <th key="ac">Action</th>,
            <th key="e">Entity</th>,
            <th key="r">Result</th>,
          ]}
        >
          {auditReportRows.map((r) => (
            <tr key={`${r.time}-${r.action}`}>
              <td>{r.time}</td>
              <td>{r.actor}</td>
              <td>{r.action}</td>
              <td>
                {r.isTag ? <span className="tag">{r.entity}</span> : r.entity}
              </td>
              <td><StatusPill tone={r.tone}>{r.result}</StatusPill></td>
            </tr>
          ))}
        </DataTable>
        <Findings items={auditReportFindings} />
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
