import { ActionNote, DataTable, Kpi, KpiGrid, MetaStrip, ReportFooter, ReportHeader, StatusPill } from "../components";
import { services } from "@/data/mock";

const toneMap: Record<string, "g" | "b" | "r"> = { success: "g", advisory: "b", critical: "r" };

export function HealthReport({ page }: { page: string }) {
  return (
    <section className="sheet" id="health">
      <ReportHeader subtitle="Monitoring coverage report · 06 Oct 2026" />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">Monitoring coverage report</div>
          <h1>One camera down for 2 h 14 min, all other services healthy</h1>
        </div>
        <MetaStrip
          items={[
            ["Snapshot", "06 Oct 2026 · 09:46 AST"],
            ["Deployment", "Rig-local / hybrid"],
            ["Licence", "24 of 32 streams"],
            ["Enterprise sync", "Connected · 0 pending"],
          ]}
        />
        <KpiGrid>
          <Kpi label="Camera streams" value={<span>23<small> /24</small></span>} delta="One camera offline" tone="up" />
          <Kpi label="Inference pipelines" value={<span>23<small> /24</small></span>} delta="1 blocked by source outage" />
          <Kpi label="Evidence storage" value={<span>68<small>%</small></span>} delta="No write failures" tone="down" />
          <Kpi label="Licensed capacity" value={<span>75<small>%</small></span>} delta="8 streams available" />
        </KpiGrid>
        <div>
          <h3>Service status</h3>
        </div>
        <DataTable
          head={[
            <th key="c">Component</th>,
            <th key="s">State</th>,
            <th key="u">Last update</th>,
          ]}
        >
          {services.map((s) => (
            <tr key={s.name}>
              <td>{s.name}</td>
              <td><StatusPill tone={toneMap[s.tone] ?? "n"}>{s.status}</StatusPill></td>
              <td>{s.seen}</td>
            </tr>
          ))}
        </DataTable>
        <div className="two">
          <div className="kpi-box">
            <h3>Monitoring gap · CAM-06</h3>
            <dl className="kv">
              <div>
                <dt>Rig · area</dt>
                <dd style={{ fontSize: 14 }}>Rig 04 · Access corridor</dd>
              </div>
              <div>
                <dt>Last frame</dt>
                <dd style={{ fontSize: 14 }}>09:42 AST</dd>
              </div>
              <div>
                <dt>Assigned to</dt>
                <dd style={{ fontSize: 14 }}>Omar S. · Maintenance</dd>
              </div>
              <div>
                <dt>Rules affected</dt>
                <dd style={{ fontSize: 14 }}>Access-zone monitoring</dd>
              </div>
            </dl>
          </div>
          <div className="stack">
            <h3>What this means</h3>
            <p>
              While CAM-06 is down, no events can be raised for the rig 04 access corridor. Fleet
              summaries show the camera as unavailable instead of counting the area as clear.
            </p>
          </div>
        </div>
        <ActionNote>
          <b>Required. </b>A lost feed is not an all-clear condition. Restore CAM-06 or post a manual
          watch at the access corridor until the feed returns.
        </ActionNote>
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
