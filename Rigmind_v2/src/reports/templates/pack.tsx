import { useCallback, useState } from "react";
import { Link } from "@tanstack/react-router";
import { InfoNote, MetaStrip, ReportFooter, ReportHeader } from "../components";
import { ActionReport } from "./action";
import { AuditReport } from "./audit";
import { EventReport } from "./event";
import { HandoverReport } from "./handover";
import { HealthReport } from "./health";
import { HseReport } from "./hse";
import { PilotReport } from "./pilot";
import { RegisterReport } from "./register";
import { ShiftReport } from "./shift";
import { WeeklyReport } from "./weekly";

const toc: { n: string; id: string; title: string; desc: string; cta: string; src: string }[] = [
  { n: "01", id: "shift", title: "Shift safety report", desc: "Events, response times and coverage for one rig shift.", cta: "Generate report", src: "Reports › Shift safety · p. 02–03" },
  { n: "02", id: "event", title: "Event investigation report", desc: "One event end to end: frame, sequence, review and follow-up.", cta: "Export evidence", src: "Event investigation · p. 04" },
  { n: "03", id: "hse", title: "HSE review report", desc: "Review decisions, evidence status and corrective actions.", cta: "Generate report", src: "Reports › HSE review · p. 05" },
  { n: "04", id: "action", title: "Corrective action report", desc: "One action from creation to closure, with evidence.", cta: "Export action", src: "Actions & handover · p. 06" },
  { n: "05", id: "handover", title: "Shift handover report", desc: "What the outgoing supervisor passes to the next shift.", cta: "Prepare handover", src: "Actions & handover · p. 07" },
  { n: "06", id: "weekly", title: "Weekly fleet safety report", desc: "Seven-day trends across all rigs, by severity and type.", cta: "Export overview / Export report", src: "Fleet overview · Analytics · p. 08–09" },
  { n: "07", id: "health", title: "Monitoring coverage report", desc: "Camera, pipeline and storage health, with gaps.", cta: "Diagnostics", src: "System health · p. 10" },
  { n: "08", id: "pilot", title: "Pilot results report", desc: "Readiness and measured outcomes for the pilot rig.", cta: "Generate report", src: "Reports › Pilot results · p. 11" },
  { n: "09", id: "register", title: "Event register", desc: "Auditable list of every verified event in a period.", cta: "Export register", src: "Event history · p. 12" },
  { n: "10", id: "audit", title: "Audit trail extract", desc: "Append-only record of actions on an entity.", cta: "Export audit", src: "Audit trail · p. 13" },
];

/** Weekly report occupies p. 08–09 plus any HSE summary sheets; later sheets shift by the same amount. */
export function shiftPages(src: string, extra: number) {
  const f = (n: number) => String(n).padStart(2, "0");
  return src.replace(/p\. (\d\d)(?:–(\d\d))?/, (_m, a: string, b?: string) => {
    const start = Number(a);
    const end = Number(b ?? a);
    const s2 = start > 9 ? start + extra : start;
    const e2 = end >= 9 ? end + extra : end;
    return s2 === e2 ? `p. ${f(s2)}` : `p. ${f(s2)}–${f(e2)}`;
  });
}

export function PackIndex() {
  const [weeklySheets, setWeeklySheets] = useState(2);
  const extra = weeklySheets - 2;
  const total = 13 + extra;
  const pg = (n: number) => `${String(n > 9 ? n + extra : n).padStart(2, "0")} / ${String(total).padStart(2, "0")}`;
  const weeklyPage = useCallback((i: number) => `${String(8 + i).padStart(2, "0")} / ${String(13 + extra).padStart(2, "0")}`, [extra]);
  return (
    <>
      <section className="sheet" id="index">
        <ReportHeader subtitle="Report pack · index" />
        <div className="sheet-body">
          <div>
            <div className="eyebrow">RigMind report pack · demonstration data</div>
            <h1>Every report the app generates, in one format</h1>
          </div>
          <MetaStrip
            items={[
              ["Workspace", "Safety workspace · Enterprise demo"],
              ["Rigs", "Demo rigs 01–04"],
              ["Reference day", "Tue 06 Oct 2026"],
              ["Time zone", "AST (UTC+3)"],
            ]}
          />
          <p>
            Each report below is produced from a screen in RigMind. The right-hand column names the
            button that creates it. All reports share the same sheet, header, severity colours and
            footer, so they read as one set when filed together.
          </p>
          <nav className="toc">
            {toc.map((t) => (
              <Link key={t.id} to="/report/$reportId" params={{ reportId: t.id }}>
                <span className="n">{t.n}</span>
                <span>
                  <b>{t.title}</b>
                  <span className="d">{t.desc}</span>
                </span>
                <span className="src">
                  <b>{t.cta}</b>
                  <br />
                  {shiftPages(t.src, extra)}
                </span>
              </Link>
            ))}
          </nav>
          <InfoNote>
            <b>Severity colours.</b> Critical events are red, warning events are orange and advisory
            events are yellow, in every table, chart and label.
          </InfoNote>
        </div>
        <ReportFooter page={pg(1)} />
      </section>

      <ShiftReport pages={[pg(2), pg(3)]} />
      <EventReport page={pg(4)} />
      <HseReport page={pg(5)} />
      <ActionReport page={pg(6)} />
      <HandoverReport page={pg(7)} />
      <WeeklyReport pageOf={weeklyPage} onSheets={setWeeklySheets} />
      <HealthReport page={pg(10)} />
      <PilotReport page={pg(11)} />
      <RegisterReport page={pg(12)} />
      <AuditReport page={pg(13)} />
    </>
  );
}
