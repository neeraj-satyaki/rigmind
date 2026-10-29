import {
  Checklist,
  MetaStrip,
  ProgressBar,
  ReportFooter,
  ReportHeader,
  SignatureRow,
  Timeline,
} from "../components";
import { actionReport } from "@/data/mock";

export function ActionReport({ page, actionId = "CA-028" }: { page: string; actionId?: string }) {
  return (
    <section className="sheet" id="action">
      <ReportHeader subtitle={`Corrective action report · ${actionId}`} />
      <div className="sheet-body">
        <div className="evt-title">
          <div className="eyebrow">Corrective action report</div>
          <div className="evt-id">
            <span className="mono">{actionId}</span>
            <span className="pill b">In progress</span>
            <span className="tag">Linked EV-1042</span>
          </div>
          <h2>Review the crew positioning procedure for derrick-zone work</h2>
        </div>
        <MetaStrip
          items={[
            ["Created", "06 Oct 2026 · 09:47 by Maha S."],
            ["Owner", "Faisal A. · Shift supervisor"],
            ["Closure approver", "Maha S. · HSE"],
            ["Due", "07 Oct 2026 · 12:00 AST"],
          ]}
        />
        <div className="two">
          <div className="stack">
            <h3>Why this action exists</h3>
            <p>
              EV-1042 confirmed a crew member inside the derrick movement zone for 44 s during
              tubular handling. The current procedure does not say where the derrick crew should
              stand while the block is moving.
            </p>
            <h3 style={{ marginTop: 8 }}>Closure criteria</h3>
            <Checklist items={actionReport.checklist} />
          </div>
          <div className="kpi-box">
            <h3>Progress</h3>
            <ProgressBar pct={25} />
            <p className="muted" style={{ fontSize: 12, marginTop: 6 }}>
              1 of 4 closure criteria met
            </p>
            <dl className="kv" style={{ marginTop: 16 }}>
              <div>
                <dt>Time open</dt>
                <dd>3 <small>h</small></dd>
              </div>
              <div>
                <dt>Time to due</dt>
                <dd>23 <small>h</small></dd>
              </div>
              <div>
                <dt>Updates</dt>
                <dd>3</dd>
              </div>
              <div>
                <dt>Evidence files</dt>
                <dd>1</dd>
              </div>
            </dl>
          </div>
        </div>
        <div>
          <h3>Progress log</h3>
          <Timeline items={actionReport.progressLog} />
        </div>
        <SignatureRow
          items={[
            { name: "Faisal A.", caption: "Action owner · signature and date" },
            { name: "Maha S.", caption: "HSE reviewer · accepted / reopened" },
          ]}
        />
      </div>
      <ReportFooter page={page} />
    </section>
  );
}
