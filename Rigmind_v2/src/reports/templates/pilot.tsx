import { DataTable, Kpi, KpiGrid, MetaStrip, ReportFooter, ReportHeader, SignatureRow, StatusPill } from "../components";
import { pilotCriteria } from "@/data/mock";
import { demoDataEnabled } from "@/lib/demo-mode";
import { RealPilotReport } from "../real-templates";

export function PilotReport({ page }: { page: string }) {
  return demoDataEnabled() ? <PilotReportDemo page={page} /> : <RealPilotReport page={page} />;
}

function PilotReportDemo({ page }: { page: string }) {
  return (
    <section className="sheet" id="pilot">
      <ReportHeader subtitle="Pilot results report · Demo rig 01" />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">Pilot results report · Demo rig 01</div>
          <h1>Evidence before expansion</h1>
        </div>
        <MetaStrip
          items={[
            ["Pilot rig", "Demo rig 01 · Eastern Province"],
            ["Pilot window", <span key="w"><span className="ph">[START DATE]</span> – <span className="ph">[END DATE]</span></span>],
            ["Sponsor", <span key="s" className="ph">[CLIENT SPONSOR]</span>],
            ["Prepared by", "Satyaki Solutions"],
          ]}
        />
        <p>
          This report decides whether RigMind is ready to move beyond the pilot rig. Each criterion
          below needs measured evidence from site acceptance testing. Items not yet measured are
          shown as open and are never counted as passed.
        </p>
        <div>
          <h3>Readiness criteria</h3>
        </div>
        <DataTable
          head={[
            <th key="c">Criterion</th>,
            <th key="t">Target</th>,
            <th key="m">Measured</th>,
            <th key="s">Status</th>,
          ]}
        >
          {pilotCriteria.map((c) => (
            <tr key={c.criterion}>
              <td>{c.criterion}</td>
              <td>{c.target}</td>
              <td>{c.measured}</td>
              <td><StatusPill tone={c.tone}>{c.status}</StatusPill></td>
            </tr>
          ))}
        </DataTable>
        <KpiGrid cols={3}>
          <Kpi label="Criteria met" value={<span>0<small> /6</small></span>} delta="2 need review or approval" />
          <Kpi label="Events in demo data" value="233" delta="Not pilot evidence" />
          <Kpi label="Decision" value="Hold" delta="Expand after UAT sign-off" />
        </KpiGrid>
        <SignatureRow
          items={[
            { name: <span className="ph">[CLIENT SPONSOR]</span>, caption: "Pilot sponsor · approve / hold" },
            { name: <span className="ph">[SATYAKI LEAD]</span>, caption: "Satyaki Solutions · delivery lead" },
          ]}
        />
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
