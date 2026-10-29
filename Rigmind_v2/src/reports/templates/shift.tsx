import {
  ActionNote,
  DataTable,
  Findings,
  GroupedBarChart,
  Kpi,
  KpiGrid,
  MetaStrip,
  ReportFooter,
  ReportHeader,
  SeriesLegend,
  SeverityPill,
  StatusPill,
} from "../components";
import { demoDataEnabled } from "@/lib/demo-mode";
import { RealShiftReport } from "../real-templates";
import { shiftCoverageGaps, shiftCriticalRows, shiftFindings, shiftReport } from "@/data/mock";

export function ShiftReport({ pages }: { pages: [string, string] }) {
  return demoDataEnabled() ? <ShiftReportDemo pages={pages} /> : <RealShiftReport pages={pages} />;
}

function ShiftReportDemo({ pages: [p1, p2] }: { pages: [string, string] }) {
  return (
    <>
      <section className="sheet" id="shift">
        <ReportHeader subtitle="Shift safety report · 06 Oct 2026 · Day shift" />
        <div className="sheet-body">
          <div>
            <div className="eyebrow">Shift safety report · Day shift</div>
            <h1>Two critical zone events before 10:00, both acknowledged within 24 s</h1>
          </div>
          <MetaStrip
            items={[
              ["Shift", "Tue 06 Oct 2026 · 06:00–18:00 AST"],
              ["Scope", "Demo rigs 01–04"],
              ["Supervisor", "Faisal A."],
              ["Monitored", "23 of 24 cameras"],
            ]}
          />
          <p>
            RigMind verified <b>26 events</b> across the four demonstration rigs this shift. Three
            were critical. Two of them, EV-1041 and EV-1042, came within 11 seconds of each other at
            09:42 and both involved crew inside an active zone. Every critical event was acknowledged
            within 24 s. CAM-06 on Demo rig 04 was offline for 2 h 14 min, so the access corridor had
            no coverage for part of the morning.
          </p>
          <KpiGrid>
            <Kpi label="Verified events" value="26" delta="14 advisory · 9 warning · 3 critical" />
            <Kpi label="Critical events" value="3" delta="▲ 1 vs previous day shift" tone="up" />
            <Kpi label="Median acknowledgement" value={<span>8<small> s</small></span>} delta="P95 24 s" />
            <Kpi label="Camera coverage" value={<span>95.8<small>%</small></span>} delta="CAM-06 offline 2 h 14 m" tone="up" />
          </KpiGrid>
          <div className="chart-card">
            <div className="chart-head">
              <div>
                <h3 style={{ margin: 0 }}>Verified events per two-hour block</h3>
                <div className="muted" style={{ fontSize: 12 }}>
                  The 08:00–10:00 block holds both zone events and coincides with tubular handling on
                  rig 01
                </div>
              </div>
              <SeriesLegend />
            </div>
            <GroupedBarChart
              groups={shiftReport.buckets}
              series={[shiftReport.series.advisory, shiftReport.series.warning, shiftReport.series.critical]}
              ticks={shiftReport.ticks}
            />
          </div>
        </div>
        <ReportFooter page={p1} />
      </section>

      <section className="sheet">
        <ReportHeader subtitle="Shift safety report · 06 Oct 2026 · Day shift" />
        <div className="sheet-body">
          <div>
            <div className="eyebrow">Shift safety report · Detail</div>
            <h2>Critical and warning events</h2>
          </div>
          <DataTable
            head={[
              <th key="e">Event</th>,
              <th key="t">Time</th>,
              <th key="s">Severity</th>,
              <th key="c">Condition · location</th>,
              <th key="a" className="r">Ack</th>,
              <th key="st">Status</th>,
            ]}
          >
            {shiftCriticalRows.map((r) => (
              <tr key={r.id}>
                <td className="mono">{r.id}</td>
                <td>{r.time}</td>
                <td><SeverityPill sev={r.severity} /></td>
                <td>
                  {r.condition}
                  <br />
                  <span className="muted">{r.location}</span>
                </td>
                <td className="r">{r.ack}</td>
                <td><StatusPill tone={r.tone}>{r.status}</StatusPill></td>
              </tr>
            ))}
          </DataTable>
          <p className="muted" style={{ fontSize: 12 }}>
            Showing all 3 critical events and the 5 warning events with the longest exposure. The 14
            advisory events are in the event register.
          </p>
          <div>
            <h3>Coverage and unavailable periods</h3>
          </div>
          <DataTable
            head={[
              <th key="c">Camera</th>,
              <th key="r">Rig · area</th>,
              <th key="u">Unavailable</th>,
              <th key="d" className="r">Duration</th>,
              <th key="rf">Rules affected</th>,
            ]}
          >
            {shiftCoverageGaps.map((g) => (
              <tr key={g.cam}>
                <td className="mono">{g.cam}</td>
                <td>{g.area}</td>
                <td>{g.window}</td>
                <td className="r">{g.duration}</td>
                <td>{g.rules}</td>
              </tr>
            ))}
          </DataTable>
          <Findings items={shiftFindings} />
          <ActionNote>
            <b>Carry to night shift. </b>Review EV-1042 with the derrick crew, assign an owner to
            unowned critical alerts within 10 s, and confirm CAM-06 stays online.
          </ActionNote>
        </div>
        <ReportFooter page={p2} />
      </section>
    </>
  );
}
