import {
  ActionNote,
  DataTable,
  Kpi,
  KpiGrid,
  MetaStrip,
  ReportFooter,
  ReportHeader,
  SeverityPill,
  StatusPill,
} from "../components";
import { hseCorrectiveActions, hseDecisions } from "@/data/mock";

export function HseReport({ page }: { page: string }) {
  return (
    <section className="sheet" id="hse">
      <ReportHeader subtitle="HSE review report · 06 Oct 2026" />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">HSE review report · 06 Oct 2026</div>
          <h1>Four of six reviewed events were confirmed, one needs a better view</h1>
        </div>
        <MetaStrip
          items={[
            ["Reviewer", "Maha S. · HSE"],
            ["Scope", "Demo fleet"],
            ["Period", "06 Oct 2026 · day shift"],
            ["Reviewed", "6 of 14 eligible"],
          ]}
        />
        <KpiGrid>
          <Kpi label="Confirmed exposures" value="3" delta="of 6 reviewed" />
          <Kpi label="False positives" value="1" delta="EV-1038 · glare on pipe rack" tone="up" />
          <Kpi label="Open corrective actions" value="2" delta="CA-028 due 07 Oct" />
          <Kpi label="Awaiting review" value="8" delta="4 critical or warning" tone="up" />
        </KpiGrid>
        <div>
          <h3>Review decisions</h3>
        </div>
        <DataTable
          head={[
            <th key="e">Event</th>,
            <th key="s">Severity</th>,
            <th key="c">Condition</th>,
            <th key="cl">Classification</th>,
            <th key="ev">Evidence</th>,
            <th key="f">Follow-up</th>,
          ]}
        >
          {hseDecisions.map((d) => (
            <tr key={d.event}>
              <td>{d.event}</td>
              <td><SeverityPill sev={d.severity} /></td>
              <td>{d.condition}</td>
              <td>{d.classification}</td>
              <td><StatusPill tone={d.evidenceTone}>{d.evidence}</StatusPill></td>
              <td className="mono">{d.followUp}</td>
            </tr>
          ))}
        </DataTable>
        <div>
          <h3>Corrective actions</h3>
        </div>
        <DataTable
          head={[
            <th key="a">Action</th>,
            <th key="l">Linked</th>,
            <th key="o">Owner</th>,
            <th key="d">Due</th>,
            <th key="s">Status</th>,
          ]}
        >
          {hseCorrectiveActions.map((a) => (
            <tr key={a.id}>
              <td>
                <span className="mono">{a.id}</span> {a.title}
              </td>
              <td>{a.linked}</td>
              <td>{a.owner}</td>
              <td>{a.due}</td>
              <td><StatusPill tone={a.tone}>{a.status}</StatusPill></td>
            </tr>
          ))}
        </DataTable>
        <ActionNote>
          <b>HSE note. </b>EV-1039 was rated inconclusive because the camera angle hides the hard hat.
          Request a second camera view of the derrick board before the next pilot review.
        </ActionNote>
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
