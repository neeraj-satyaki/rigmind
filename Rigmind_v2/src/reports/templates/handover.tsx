import { Checklist, DataTable, MetaStrip, ReportFooter, ReportHeader, SeverityPill, SignatureRow, StatusPill } from "../components";
import { handoverReport } from "@/data/mock";

export function HandoverReport({ page }: { page: string }) {
  return (
    <section className="sheet" id="handover">
      <ReportHeader subtitle="Shift handover report · Day → Night · 06 Oct" />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">Shift handover report</div>
          <h1>Day to night handover, Demo rig 01</h1>
        </div>
        <MetaStrip
          items={[
            ["Handover", "06 Oct 2026 · 18:00 AST"],
            ["Outgoing", "Faisal A. · Day supervisor"],
            ["Receiving", "Omar S. · Night supervisor"],
            ["Status", <span key="s" className="pill b">Awaiting acceptance</span>],
          ]}
        />
        <div>
          <h3>Open hazards and alerts</h3>
        </div>
        <DataTable
          head={[
            <th key="i">Item</th>,
            <th key="s">Severity</th>,
            <th key="d">Detail</th>,
            <th key="n">Next step</th>,
          ]}
        >
          {handoverReport.hazards.map((h) => (
            <tr key={h.id}>
              <td className="mono">{h.id}</td>
              <td><SeverityPill sev={h.severity} /></td>
              <td>{h.detail}</td>
              <td>{h.next}</td>
            </tr>
          ))}
        </DataTable>
        <div>
          <h3>Monitoring gaps</h3>
        </div>
        <DataTable
          head={[
            <th key="c">Camera</th>,
            <th key="a">Area</th>,
            <th key="s">State at handover</th>,
            <th key="w">Watch for</th>,
          ]}
        >
          {handoverReport.gaps.map((g) => (
            <tr key={g.cam}>
              <td className="mono">{g.cam}</td>
              <td>{g.area}</td>
              <td><StatusPill tone={g.tone}>{g.state}</StatusPill></td>
              <td>{g.watch}</td>
            </tr>
          ))}
        </DataTable>
        <div>
          <h3>Pending actions</h3>
        </div>
        <DataTable
          head={[
            <th key="a">Action</th>,
            <th key="o">Owner</th>,
            <th key="d">Due</th>,
            <th key="h">Handover note</th>,
          ]}
        >
          {handoverReport.pendingActions.map((a) => (
            <tr key={a.id}>
              <td>
                <span className="mono">{a.id}</span> {a.title}
              </td>
              <td>{a.owner}</td>
              <td>{a.due}</td>
              <td>{a.note}</td>
            </tr>
          ))}
        </DataTable>
        <div>
          <h3>Handover checklist</h3>
          <Checklist items={handoverReport.checklist} />
        </div>
        <SignatureRow
          items={[
            { name: "Faisal A.", caption: "Outgoing supervisor" },
            { name: "Omar S.", caption: "Receiving supervisor · explicit acceptance" },
          ]}
        />
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
