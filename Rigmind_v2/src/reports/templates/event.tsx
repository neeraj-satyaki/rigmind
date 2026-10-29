import feed from "@/assets/rig-feed.jpg";
import { demoDataEnabled } from "@/lib/demo-mode";
import { RealEventReport } from "../real-templates";
import { ActionNote, MetaStrip, ReportFooter, ReportHeader, StatusPill, Timeline } from "../components";

export function EventReport({ page, eventId }: { page: string; eventId?: string | undefined }) {
  return demoDataEnabled() ? <EventReportDemo page={page} eventId={eventId ?? "EV-1042"} /> : <RealEventReport page={page} eventId={eventId} />;
}

function EventReportDemo({ page, eventId }: { page: string; eventId: string }) {
  return (
    <section className="sheet" id="event">
      <ReportHeader subtitle={`Event investigation report · ${eventId}`} />
      <div className="sheet-body">
        <div className="evt-title">
          <div className="eyebrow">Event investigation report</div>
          <div className="evt-id">
            <span className="mono">{eventId}</span>
            <span className="pill r">Critical</span>
            <span className="pill g">Hazard cleared</span>
            <span className="pill b">Confirmed unsafe exposure</span>
          </div>
          <h2>Crew member stayed in the derrick movement zone while tubulars were moving</h2>
        </div>
        <MetaStrip
          items={[
            ["Time", "Tue 06 Oct 2026 · 09:42:18"],
            ["Rig · area", "Demo rig 01 · Derrick board"],
            ["Camera", "CAM-01 · 1080p"],
            ["Rule", "DZ-01 · version 3"],
          ]}
        />
        <div className="shot">
          <img src={feed} alt={`Annotated frame for ${eventId}: a person inside the dashed derrick movement zone`} />
          <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
            <polygon
              points="8,100 10,58 30,52 58,62 76,72 86,90 84,100"
              fill="#FCA719"
              fillOpacity="0.12"
              stroke="#FCA719"
              strokeWidth="0.5"
              strokeDasharray="1.6 1.1"
              vectorEffect="non-scaling-stroke"
            />
            <rect
              x="18"
              y="13"
              width="19"
              height="65"
              fill="none"
              stroke="#3FC1D0"
              strokeWidth="1.6"
              vectorEffect="non-scaling-stroke"
            />
          </svg>
          <span className="ov tl">CAM-01 · RIG 01</span>
          <span className="ov tr">CRITICAL</span>
          <span className="ov bl">09:42:18.240</span>
          <span className="ov br">Person 01 | zone overlap</span>
        </div>
        <div className="evt-grid">
          <div>
            <h3>What happened</h3>
            <p>
              During tubular handling, the rig signal reported equipment movement while a crew member
              was inside the configured derrick exclusion zone. The person box overlapped the zone
              polygon for 44 s in total.
            </p>
            <p>
              RigMind raised a critical alert and a simulated local beacon. Faisal A. acknowledged in
              8 s from the live view. The person left the zone at 09:43:02 and the hazard cleared.
              Maha S. reviewed the clip and confirmed an unsafe exposure.
            </p>
            <h3 style={{ marginTop: 20 }}>Sequence</h3>
            <Timeline
              items={[
                { time: "09:42:18", text: "Critical alert: person in zone, movement active" },
                { time: "09:42:18", text: "Local beacon (simulated) activated" },
                { time: "09:42:26", text: "Acknowledged by Faisal A. in live view" },
                { time: "09:43:02", text: "Person left zone, hazard cleared" },
                { time: "09:45:10", text: "Review started by Maha S." },
                { time: "09:47:00", text: "Corrective action CA-028 created" },
              ]}
            />
          </div>
          <div className="kpi-box">
            <h3>Event KPIs</h3>
            <dl className="kv">
              <div>
                <dt>Exposure</dt>
                <dd>44 <small>s</small></dd>
              </div>
              <div>
                <dt>Acknowledged in</dt>
                <dd>8 <small>s</small></dd>
              </div>
              <div>
                <dt>Signal age at alert</dt>
                <dd>1 <small>s</small></dd>
              </div>
              <div>
                <dt>Activity</dt>
                <dd style={{ fontSize: 14 }}>Tubular handling</dd>
              </div>
              <div>
                <dt>Model</dt>
                <dd style={{ fontSize: 14 }}>demo-v1</dd>
              </div>
              <div>
                <dt>Evidence clip</dt>
                <dd style={{ fontSize: 14 }}>00:30 · ready</dd>
              </div>
            </dl>
          </div>
        </div>
        <ActionNote>
          <b>Review outcome. </b>Confirmed unsafe exposure, evidence sufficient for review. CA-028
          assigned to Faisal A. (shift supervisor) to review the crew positioning procedure, due 07
          Oct 12:00 AST. Closure approver: Maha S., HSE.
        </ActionNote>
      </div>
      <ReportFooter page={page} />
    </section>
  );
}

export { StatusPill };
