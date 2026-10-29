// Printable reports with sample data off. Every figure is computed from the host's alerts, camera status, audit log and
// the records the user saved. Where nothing was recorded the report says so; no sample incident is ever shown.
import { useEffect, useState, type ReactNode } from "react";
import {
  ActionNote, DataTable, Findings, GroupedBarChart, InfoNote, Kpi, KpiGrid, MetaStrip, ReportFooter, ReportHeader,
  SeriesLegend, SeverityPill, SignatureRow, StatusPill, Timeline,
} from "./components";
import { useCameras } from "@/lib/camera-status";
import { isNative, nativeCall } from "@/lib/native-bridge";
import { formatDuration, startOfDay, type LiveAlertRow } from "@/lib/live-analytics";
import { localOperator, useRealCameras, useStreamSources, type Transition } from "@/lib/real-state";
import { pilotCriteria } from "@/data/mock";
import {
  ACTIONS_KEY, AUDIT_ACTIONS, HANDOVER_KEY, auditActionLabel, auditActor, dayText, isEventRow, loadAlertFeed, loadAuditEntries,
  loadRecord, loadReviews, stamp, timeText, type AuditEntry, type CorrectiveAction, type HandoverSnapshot, type StoredReviewRow,
} from "@/lib/real-data";
import type { Severity } from "@/data/mock";

type Pill = "g" | "b" | "n" | "o" | "r" | "y";
const sev = (s: string): Severity => (s === "Critical" || s === "Warning" ? s : "Advisory");
const statusTone = (s: string): Pill => (s === "Cleared" || s === "Closed" ? "g" : s === "Escalated" ? "r" : s === "Open" ? "o" : "n");

/** Runs an async loader once; `null` while loading. */
function useLoaded<T>(load: () => Promise<T>, fallback: T): T | null {
  const [v, setV] = useState<T | null>(null);
  useEffect(() => {
    let off = false;
    load()
      .then((r) => !off && setV(r))
      .catch(() => !off && setV(fallback));
    return () => {
      off = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return v;
}
export const useAlertRows = () => useLoaded(() => loadAlertFeed(500), [] as LiveAlertRow[]);

export function NoData({ children = "No data for this period." }: { children?: ReactNode }) {
  return (
    <div className="kpi-box">
      <h3>No data</h3>
      <p className="muted" style={{ margin: 0 }}>{children}</p>
    </div>
  );
}

function Sheet({ id, subtitle, eyebrow, title, page, children }: { id: string; subtitle: string; eyebrow: string; title: string; page: string; children: ReactNode }) {
  return (
    <section className="sheet" id={id}>
      <ReportHeader subtitle={subtitle} />
      <div className="sheet-body">
        <div>
          <div className="eyebrow">{eyebrow}</div>
          <h1>{title}</h1>
        </div>
        {children}
      </div>
      <ReportFooter page={page} />
    </section>
  );
}

const camLine = (n: number, total: number) => (total ? `${n} of ${total} online` : "No camera configured");

// ---------------------------------------------------------------------------------------------------------------------
// Shift safety: today so far (00:00 to now), computed from the alerts the host raised.

export function RealShiftReport({ pages: [p1, p2] }: { pages: [string, string] }) {
  const rows = useAlertRows();
  const status = useCameras();
  const cams = useRealCameras(useStreamSources());
  const now = Date.now();
  const from = startOfDay(now);
  const today = (rows ?? []).filter((r) => isEventRow(r) && r.raisedMs >= from && r.raisedMs <= now + 60_000);
  const count = (s: string) => today.filter((r) => r.severity === s).length;
  const buckets = Array.from({ length: 12 }, (_, i) => `${String(i * 2).padStart(2, "0")}–${String(i * 2 + 2).padStart(2, "0")}`);
  const series = (["Advisory", "Warning", "Critical"] as const).map((s) =>
    buckets.map((_, b) => today.filter((r) => r.severity === s && Math.floor(new Date(r.raisedMs).getHours() / 2) === b).length),
  ) as [number[], number[], number[]];
  const max = Math.max(1, ...series.flat());
  const ticks = Array.from({ length: Math.ceil(max / Math.max(1, Math.ceil(max / 4))) + 1 }, (_, i) => i * Math.max(1, Math.ceil(max / 4)));
  const online = cams.filter((c) => status[c.id]?.state === "online").length;
  const down = cams.filter((c) => status[c.id]?.state !== "online");
  const detail = today.filter((r) => r.severity !== "Advisory");
  const sub = `Shift safety report · ${dayText(now)} · today so far`;
  const busiest = buckets.reduce((b, _, i) => ((series[0][i]! + series[1][i]! + series[2][i]!) > (series[0][b]! + series[1][b]! + series[2][b]!) ? i : b), 0);
  return (
    <>
      <Sheet id="shift" subtitle={sub} eyebrow="Shift safety report · Today so far" title={rows === null ? "Loading…" : today.length ? `${today.length} alerts raised today, ${count("Critical")} critical` : "No alerts raised today"} page={p1}>
        <MetaStrip items={[["Period", `${dayText(now)} · 00:00–${timeText(now).slice(0, 5)} local`], ["Scope", "This rig"], ["Source", "Alerts raised on this device"], ["Cameras", camLine(online, cams.length)]]} />
        {rows !== null && today.length === 0 ? (
          <NoData>No data for this period. No alerts were raised today{isNative() ? "." : "; alert data is read from the RigMind desktop app."}</NoData>
        ) : (
          <>
            <KpiGrid>
              <Kpi label="Alerts raised" value={String(today.length)} delta={`${count("Advisory")} advisory · ${count("Warning")} warning · ${count("Critical")} critical`} />
              <Kpi label="Critical alerts" value={String(count("Critical"))} />
              <Kpi label="Median acknowledgement" value="—" delta="Not recorded by the host yet" />
              <Kpi label="Cameras online" value={cams.length ? `${online}/${cams.length}` : "—"} delta={cams.length ? "At report time" : "No camera configured"} />
            </KpiGrid>
            <div className="chart-card">
              <div className="chart-head">
                <div>
                  <h3 style={{ margin: 0 }}>Alerts per two-hour block</h3>
                  <div className="muted" style={{ fontSize: 12 }}>Busiest block {buckets[busiest]} local time</div>
                </div>
                <SeriesLegend />
              </div>
              <GroupedBarChart groups={buckets} series={series} ticks={ticks} />
            </div>
          </>
        )}
      </Sheet>
      <section className="sheet">
        <ReportHeader subtitle={sub} />
        <div className="sheet-body">
          <div>
            <div className="eyebrow">Shift safety report · Detail</div>
            <h2>Critical and warning alerts</h2>
          </div>
          {detail.length === 0 ? (
            <NoData>No critical or warning alerts in this period.</NoData>
          ) : (
            <DataTable head={[<th key="e">Alert</th>, <th key="t">Time</th>, <th key="s">Severity</th>, <th key="c">Condition · location</th>, <th key="a" className="r">Ack</th>, <th key="st">Status</th>]}>
              {detail.map((r) => (
                <tr key={r.id}>
                  <td className="mono">{r.id}</td>
                  <td>{timeText(r.raisedMs)}</td>
                  <td><SeverityPill sev={sev(r.severity)} /></td>
                  <td>{r.condition}<br /><span className="muted">{r.place}</span></td>
                  <td className="r">—</td>
                  <td><StatusPill tone={statusTone(r.phase ?? r.status)}>{r.status}</StatusPill></td>
                </tr>
              ))}
            </DataTable>
          )}
          <div><h3>Coverage and unavailable periods</h3></div>
          {down.length === 0 ? (
            <p className="muted" style={{ margin: 0 }}>{cams.length ? "No camera is unavailable at report time. Outages before the app started are not recorded." : "No camera configured."}</p>
          ) : (
            <DataTable head={[<th key="c">Camera</th>, <th key="u">Unavailable since</th>, <th key="d" className="r">Duration</th>, <th key="r">Detail</th>]}>
              {down.map((c) => {
                const st = status[c.id];
                return (
                  <tr key={c.id}>
                    <td className="mono">{c.id}</td>
                    <td>{st ? stamp(st.changedAt) : "No report from the host"}</td>
                    <td className="r">{st ? formatDuration((now - st.changedAt) / 1000) : "—"}</td>
                    <td>{st?.reason || st?.state || "—"}</td>
                  </tr>
                );
              })}
            </DataTable>
          )}
          {today.length > 0 && <Findings items={[{ title: "Reading these counts", text: "Counts are alerts raised by the detector and are not reviewed. Compare them with camera availability before drawing conclusions." }]} />}
        </div>
        <ReportFooter page={p2} />
      </section>
    </>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function RealEventReport({ page, eventId }: { page: string; eventId?: string | undefined }) {
  const data = useLoaded(
    async () => {
      if (!eventId) return null;
      const [rows, steps, reviews] = await Promise.all([
        loadAlertFeed(500),
        isNative() ? nativeCall<Transition[]>("events.recent", { limit: 500 }).catch(() => [] as Transition[]) : Promise.resolve([] as Transition[]),
        loadReviews(),
      ]);
      return { alert: rows.find((r) => r.id === eventId), steps: (Array.isArray(steps) ? steps : []).filter((t) => t.alertId === eventId).sort((a, b) => a.timestampMs - b.timestampMs), review: reviews.find((r) => r.eventId === eventId) };
    },
    null,
  );
  const sub = `Event investigation report${eventId ? ` · ${eventId}` : ""}`;
  if (!eventId || (data && !data.alert && data.steps.length === 0))
    return (
      <Sheet id="event" subtitle={sub} eyebrow="Event investigation report" title={eventId ? `No data for ${eventId}` : "No event selected"} page={page}>
        <NoData>{eventId ? "This event was not found in the alerts recorded on this device." : "Open an event from the event history and choose Export evidence."}</NoData>
      </Sheet>
    );
  if (!data) return <Sheet id="event" subtitle={sub} eyebrow="Event investigation report" title="Loading…" page={page}>{null}</Sheet>;
  const a = data.alert;
  const first = data.steps[0];
  return (
    <Sheet id="event" subtitle={sub} eyebrow="Event investigation report" title={a?.condition || first?.condition || eventId} page={page}>
      <div className="evt-id">
        <span className="mono">{eventId}</span>
        {a && <SeverityPill sev={sev(a.severity)} />}
        {a && <StatusPill tone={statusTone(a.phase ?? a.status)}>{a.status}</StatusPill>}
        {data.review && <span className="pill b">{data.review.classification}</span>}
      </div>
      <MetaStrip items={[["Time", a ? stamp(a.raisedMs) : first ? stamp(first.timestampMs) : "—"], ["Location", a?.place || first?.place || "—"], ["Camera", first?.camId || "—"], ["Owner", a?.owner || "Unassigned"]]} />
      <div className="evt-grid">
        <div>
          <h3>Recorded sequence</h3>
          {data.steps.length === 0 ? (
            <p className="muted">No state changes were recorded for this alert.</p>
          ) : (
            <Timeline items={data.steps.map((t) => ({ time: timeText(t.timestampMs), text: `${t.from} → ${t.to}` }))} />
          )}
        </div>
        <div className="kpi-box">
          <h3>Review</h3>
          {data.review ? (
            <>
              <p style={{ margin: 0 }}><b>{data.review.classification}</b></p>
              <p className="muted" style={{ fontSize: 12 }}>{data.review.notes || "No notes."}</p>
              <p className="muted" style={{ fontSize: 12 }}>Saved {data.review.savedAt} by {data.review.by}</p>
            </>
          ) : (
            <p className="muted" style={{ margin: 0 }}>Not reviewed yet.</p>
          )}
        </div>
      </div>
    </Sheet>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function RealHseReport({ page }: { page: string }) {
  const data = useLoaded(
    async () => ({ reviews: await loadReviews(), rows: await loadAlertFeed(500), actions: (await loadRecord<CorrectiveAction[]>(ACTIONS_KEY)) ?? [] }),
    { reviews: [] as StoredReviewRow[], rows: [] as LiveAlertRow[], actions: [] as CorrectiveAction[] },
  );
  const sub = "HSE review report";
  if (!data) return <Sheet id="hse" subtitle={sub} eyebrow="HSE review report" title="Loading…" page={page}>{null}</Sheet>;
  const { reviews, rows, actions } = data;
  const events = rows.filter(isEventRow);
  const byId = new Map(events.map((r) => [r.id, r]));
  const open = actions.filter((a) => a.status !== "Closed");
  const confirmed = reviews.filter((r) => /confirmed/i.test(r.classification)).length;
  const falsePos = reviews.filter((r) => /false positive/i.test(r.classification)).length;
  const awaiting = events.filter((r) => !reviews.some((v) => v.eventId === r.id)).length;
  return (
    <Sheet id="hse" subtitle={sub} eyebrow="HSE review report" title={reviews.length ? `${reviews.length} reviewed ${reviews.length === 1 ? "event" : "events"}, ${confirmed} confirmed` : "No events reviewed yet"} page={page}>
      <MetaStrip items={[["Scope", "This rig"], ["Source", "Review decisions saved on this device"], ["Prepared", stamp(Date.now())], ["Reviewed", `${reviews.length} of ${events.length} alerts`]]} />
      {reviews.length === 0 && open.length === 0 ? (
        <NoData>No data for this period. No review decisions or corrective actions have been saved.</NoData>
      ) : (
        <>
          <KpiGrid>
            <Kpi label="Confirmed exposures" value={String(confirmed)} delta={`of ${reviews.length} reviewed`} />
            <Kpi label="False positives" value={String(falsePos)} />
            <Kpi label="Open corrective actions" value={String(open.length)} />
            <Kpi label="Awaiting review" value={String(awaiting)} delta="Alerts without a saved decision" />
          </KpiGrid>
          <div><h3>Review decisions</h3></div>
          {reviews.length === 0 ? (
            <p className="muted" style={{ margin: 0 }}>None saved.</p>
          ) : (
            <DataTable head={[<th key="e">Event</th>, <th key="s">Severity</th>, <th key="c">Condition</th>, <th key="cl">Classification</th>, <th key="b">Reviewer</th>, <th key="n">Notes</th>]}>
              {reviews.map((v) => {
                const a = byId.get(v.eventId);
                return (
                  <tr key={v.eventId}>
                    <td className="mono">{v.eventId}</td>
                    <td>{a ? <SeverityPill sev={sev(a.severity)} /> : "—"}</td>
                    <td>{a?.condition ?? "—"}</td>
                    <td>{v.classification}</td>
                    <td>{v.by}</td>
                    <td>{v.notes || "—"}</td>
                  </tr>
                );
              })}
            </DataTable>
          )}
          <div><h3>Corrective actions</h3></div>
          {actions.length === 0 ? (
            <p className="muted" style={{ margin: 0 }}>None created.</p>
          ) : (
            <DataTable head={[<th key="a">Action</th>, <th key="l">Linked</th>, <th key="o">Owner</th>, <th key="d">Due</th>, <th key="s">Status</th>]}>
              {actions.map((a) => (
                <tr key={a.id}>
                  <td><span className="mono">{a.id}</span> {a.title}</td>
                  <td>{a.linkedEvent || "—"}</td>
                  <td>{a.owner || "—"}</td>
                  <td>{a.due || "—"}</td>
                  <td><StatusPill tone={statusTone(a.status)}>{a.status}</StatusPill></td>
                </tr>
              ))}
            </DataTable>
          )}
        </>
      )}
    </Sheet>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function RealActionReport({ page, actionId }: { page: string; actionId?: string | undefined }) {
  const actions = useLoaded(async () => (await loadRecord<CorrectiveAction[]>(ACTIONS_KEY)) ?? [], [] as CorrectiveAction[]);
  const sub = `Corrective action report${actionId ? ` · ${actionId}` : ""}`;
  if (!actions) return <Sheet id="action" subtitle={sub} eyebrow="Corrective action report" title="Loading…" page={page}>{null}</Sheet>;
  const a = actions.find((x) => x.id === actionId) ?? (actionId ? undefined : actions[0]);
  if (!a)
    return (
      <Sheet id="action" subtitle={sub} eyebrow="Corrective action report" title={actionId ? `No data for ${actionId}` : "No corrective actions yet"} page={page}>
        <NoData>{actionId ? "This action was not found on this device." : "Create an action under Actions & handover first."}</NoData>
      </Sheet>
    );
  const openHours = Math.max(0, Math.round((Date.now() - a.createdAt) / 3_600_000));
  return (
    <Sheet id="action" subtitle={`Corrective action report · ${a.id}`} eyebrow="Corrective action report" title={a.title} page={page}>
      <div className="evt-id">
        <span className="mono">{a.id}</span>
        <StatusPill tone={statusTone(a.status)}>{a.status}</StatusPill>
        {a.linkedEvent && <span className="tag">Linked {a.linkedEvent}</span>}
      </div>
      <MetaStrip items={[["Created", stamp(a.createdAt)], ["Owner", a.owner || "Not assigned"], ["Closure approver", a.approver || "Not assigned"], ["Due", a.due || "Not set"]]} />
      <KpiGrid cols={3}>
        <Kpi label="Time open" value={<span>{openHours}<small> h</small></span>} />
        <Kpi label="Updates" value={String(a.updates.length)} />
        <Kpi label="Status" value={a.status} />
      </KpiGrid>
      <div>
        <h3>Progress log</h3>
        {a.updates.length === 0 ? <p className="muted">No updates posted.</p> : <Timeline items={a.updates.map((u) => ({ time: stamp(u.at), text: `${u.who}: ${u.text}` }))} />}
      </div>
      <SignatureRow items={[{ name: a.owner || " ", caption: "Action owner · signature and date" }, { name: a.approver || " ", caption: "HSE reviewer · accepted / reopened" }]} />
    </Sheet>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function RealHandoverReport({ page }: { page: string }) {
  const snap = useLoaded<HandoverSnapshot | undefined>(() => loadRecord<HandoverSnapshot>(HANDOVER_KEY), undefined);
  const sub = "Shift handover report";
  if (!snap)
    return (
      <Sheet id="handover" subtitle={sub} eyebrow="Shift handover report" title={snap === null ? "Loading…" : "No handover prepared"} page={page}>
        {snap === undefined && <NoData>No data for this period. Prepare a handover under Actions & handover first.</NoData>}
      </Sheet>
    );
  return (
    <Sheet id="handover" subtitle={`${sub} · ${dayText(snap.preparedAt)}`} eyebrow="Shift handover report" title={`Handover prepared ${stamp(snap.preparedAt)}`} page={page}>
      <MetaStrip items={[["Prepared", stamp(snap.preparedAt)], ["Outgoing", snap.outgoing || "—"], ["Receiving", snap.receiving || "—"], ["Status", snap.accepted ? <span key="s" className="pill g">Accepted {snap.acceptedAt ? stamp(snap.acceptedAt) : ""}</span> : <span key="s" className="pill b">Awaiting acceptance</span>]]} />
      <div><h3>Open hazards and alerts</h3></div>
      {snap.hazards.length === 0 ? <p className="muted" style={{ margin: 0 }}>None open at handover.</p> : (
        <DataTable head={[<th key="i">Item</th>, <th key="s">Severity</th>, <th key="d">Detail</th>]}>
          {snap.hazards.map((h) => <tr key={h.id}><td className="mono">{h.id}</td><td><SeverityPill sev={sev(h.severity)} /></td><td>{h.detail}</td></tr>)}
        </DataTable>
      )}
      <div><h3>Monitoring gaps</h3></div>
      {snap.gaps.length === 0 ? <p className="muted" style={{ margin: 0 }}>No camera was unavailable at handover.</p> : (
        <DataTable head={[<th key="c">Camera</th>, <th key="s">State at handover</th>, <th key="w">Since</th>]}>
          {snap.gaps.map((g) => <tr key={g.cam}><td className="mono">{g.cam}</td><td>{g.state}</td><td>{g.since}</td></tr>)}
        </DataTable>
      )}
      <div><h3>Pending actions</h3></div>
      {snap.pending.length === 0 ? <p className="muted" style={{ margin: 0 }}>None pending.</p> : (
        <DataTable head={[<th key="a">Action</th>, <th key="o">Owner</th>, <th key="d">Due</th>]}>
          {snap.pending.map((a) => <tr key={a.id}><td><span className="mono">{a.id}</span> {a.title}</td><td>{a.owner || "—"}</td><td>{a.due || "—"}</td></tr>)}
        </DataTable>
      )}
      <div><h3>Handover checklist</h3></div>
      <ul className="muted" style={{ margin: 0 }}>{snap.checklist.map((c) => <li key={c.text}>{c.done ? "Done" : "Open"} · {c.text}</li>)}</ul>
      <SignatureRow items={[{ name: snap.outgoing || " ", caption: "Outgoing supervisor" }, { name: snap.receiving || " ", caption: "Receiving supervisor · explicit acceptance" }]} />
    </Sheet>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function RealHealthReport({ page }: { page: string }) {
  const status = useCameras();
  const cams = useRealCameras(useStreamSources());
  const now = Date.now();
  const list = cams.map((c) => ({ id: c.id, st: status[c.id] }));
  const online = list.filter((c) => c.st?.state === "online");
  const down = list.filter((c) => c.st?.state !== "online");
  const avail = list.filter((c) => c.st && c.st.observedMs > 0).map((c) => c.st!.onlineMs / c.st!.observedMs);
  return (
    <Sheet id="health" subtitle={`Monitoring coverage report · ${dayText(now)}`} eyebrow="Monitoring coverage report" title={list.length ? `${online.length} of ${list.length} cameras online` : "No camera configured"} page={page}>
      <MetaStrip items={[["Snapshot", stamp(now)], ["Deployment", "Rig-local"], ["Source", "Camera status reported by the host"], ["Operator", localOperator.name]]} />
      {list.length === 0 ? (
        <NoData>No data for this period. No camera has been configured.</NoData>
      ) : (
        <>
          <KpiGrid>
            <Kpi label="Camera streams" value={<span>{online.length}<small> /{list.length}</small></span>} delta={down.length ? `${down.length} not online` : "All online"} />
            <Kpi label="Average frame rate" value={online.length ? online.reduce((s, c) => s + c.st!.fps, 0) / online.length > 0 ? (online.reduce((s, c) => s + c.st!.fps, 0) / online.length).toFixed(1) : "0" : "—"} />
            <Kpi label="Reconnects" value={String(list.reduce((s, c) => s + (c.st?.reconnects ?? 0), 0))} delta="Since the app started" />
            <Kpi label="Availability" value={avail.length ? <span>{((avail.reduce((s, v) => s + v, 0) / avail.length) * 100).toFixed(1)}<small>%</small></span> : "—"} delta="Since the app started" />
          </KpiGrid>
          <div><h3>Camera status</h3></div>
          <DataTable head={[<th key="c">Camera</th>, <th key="s">State</th>, <th key="u">Since</th>, <th key="d">Detail</th>]}>
            {list.map((c) => (
              <tr key={c.id}>
                <td className="mono">{c.id}</td>
                <td><StatusPill tone={c.st?.state === "online" ? "g" : c.st?.state === "connecting" ? "y" : "r"}>{c.st ? c.st.state : "no report"}</StatusPill></td>
                <td>{c.st ? stamp(c.st.changedAt) : "—"}</td>
                <td>{c.st?.reason || (c.st?.state === "online" ? `${c.st.fps.toFixed(1)} fps · ${Math.round(c.st.kbps)} kbps` : "—")}</td>
              </tr>
            ))}
          </DataTable>
          {down.length > 0 && (
            <ActionNote>
              <b>Required. </b>A lost feed is not an all-clear condition. Restore {down.map((d) => d.id).join(", ")} or post a manual watch until the feed returns.
            </ActionNote>
          )}
        </>
      )}
      <InfoNote>Evidence storage and enterprise sync are not measured by this version, so they are not reported here.</InfoNote>
    </Sheet>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function RealPilotReport({ page }: { page: string }) {
  const rows = useAlertRows();
  const status = useCameras();
  const cams = useRealCameras(useStreamSources());
  const online = cams.filter((c) => status[c.id]?.state === "online").length;
  const measured: Record<string, [string, string, Pill]> = {
    "Camera placement": [cams.length ? camLine(online, cams.length) : "No camera configured", "Not assessed", "n"],
    "Rules and use cases": ["Not recorded", "Not recorded", "n"],
    "Detection quality": ["Not measured", "Not measured", "n"],
    "Alert latency": ["Not measured", "Not measured", "n"],
    "Alarm interface": ["No field output connected", "Not connected", "n"],
    "Site UAT": ["Not recorded", "Not recorded", "n"],
  };
  return (
    <Sheet id="pilot" subtitle="Pilot results report · This rig" eyebrow="Pilot results report · This rig" title="Evidence before expansion" page={page}>
      <MetaStrip items={[["Pilot rig", "This rig"], ["Pilot window", <span key="w"><span className="ph">[START DATE]</span> – <span className="ph">[END DATE]</span></span>], ["Sponsor", <span key="s" className="ph">[CLIENT SPONSOR]</span>], ["Prepared by", "Satyaki Solutions"]]} />
      <p>
        This report decides whether RigMind is ready to move beyond the pilot rig. Each criterion below needs measured evidence from site acceptance testing. Items not measured are shown as open and are never counted as passed.
      </p>
      <div><h3>Readiness criteria</h3></div>
      <DataTable head={[<th key="c">Criterion</th>, <th key="t">Target</th>, <th key="m">Measured</th>, <th key="s">Status</th>]}>
        {pilotCriteria.map((c) => {
          const m = measured[c.criterion] ?? ["Not measured", "Not measured", "n" as Pill];
          return (
            <tr key={c.criterion}>
              <td>{c.criterion}</td>
              <td>{c.target}</td>
              <td>{m[0]}</td>
              <td><StatusPill tone={m[2]}>{m[1]}</StatusPill></td>
            </tr>
          );
        })}
      </DataTable>
      <KpiGrid cols={3}>
        <Kpi label="Criteria met" value={<span>0<small> /{pilotCriteria.length}</small></span>} delta="None measured against a target yet" />
        <Kpi label="Alerts recorded on this device" value={rows === null ? "…" : String(rows.filter(isEventRow).length)} delta="Not validated against ground truth" />
        <Kpi label="Decision" value="Pending" delta="Needs site UAT sign-off" />
      </KpiGrid>
      <SignatureRow items={[{ name: <span className="ph">[CLIENT SPONSOR]</span>, caption: "Pilot sponsor · approve / hold" }, { name: <span className="ph">[SATYAKI LEAD]</span>, caption: "Satyaki Solutions · delivery lead" }]} />
    </Sheet>
  );
}

// ---------------------------------------------------------------------------------------------------------------------

export function registerRowsFrom(rows: LiveAlertRow[]) {
  return rows.filter(isEventRow).map((r) => ({
    id: r.id,
    occurred: stamp(r.raisedMs),
    severity: sev(r.severity),
    type: r.condition,
    location: r.place,
    review: r.status,
    tone: statusTone(r.phase ?? r.status),
    owner: r.owner || "Unassigned",
  }));
}

export function RealRegisterTable({ rows }: { rows: ReturnType<typeof registerRowsFrom> }) {
  return (
    <DataTable head={[<th key="i">Event ID</th>, <th key="o">Occurred</th>, <th key="s">Severity</th>, <th key="t">Event type</th>, <th key="r">Area</th>, <th key="rs">Status</th>, <th key="ow">Owner</th>]}>
      {rows.map((r) => (
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
  );
}

export function RealAuditReport({ page }: { page: string }) {
  const rows = useLoaded(() => loadAuditEntries(AUDIT_ACTIONS, 200), [] as AuditEntry[]);
  return (
    <Sheet id="audit" subtitle="Audit trail extract" eyebrow="Audit trail extract" title={rows === null ? "Loading…" : rows.length ? `${rows.length} recorded entries` : "No audit entries recorded"} page={page}>
      <MetaStrip items={[["Entity", "All entities"], ["Entries", rows ? String(rows.length) : "…"], ["Time zone", "Local time of this computer"], ["Extracted", stamp(Date.now())]]} />
      <InfoNote>
        <b>Append-only record. </b>Entries are written by the host and by the pages when a decision or change is made. Camera-open, alarm, alert-response, footage and change entries are listed; button clicks and navigation are not.
      </InfoNote>
      {rows !== null && rows.length === 0 ? (
        <NoData>No data for this period. {isNative() ? "No audit entries have been recorded yet." : "The audit log is read from the RigMind desktop app."}</NoData>
      ) : (
        <DataTable head={[<th key="t">Time</th>, <th key="a">Actor</th>, <th key="ac">Action</th>, <th key="e">Entity</th>, <th key="r">Detail</th>]}>
          {(rows ?? []).map((r) => (
            <tr key={`${r.action}-${r.id}`}>
              <td>{stamp(r.tsMs)}</td>
              <td>{auditActor(r.source)}</td>
              <td>{auditActionLabel(r.action)}</td>
              <td><span className="tag">{r.target}</span></td>
              <td>{r.detail}</td>
            </tr>
          ))}
        </DataTable>
      )}
    </Sheet>
  );
}
