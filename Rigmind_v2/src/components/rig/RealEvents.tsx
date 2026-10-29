// Event history and event investigation built only from what the host recorded (events.recent) and the live alert list.
// Used when sample data is off. No invented rows, names or numbers: nothing recorded shows an empty state.
import { useEffect, useState } from "react";
import { Link } from "@tanstack/react-router";
import { ChevronRight, Download, Plus } from "lucide-react";
import { toast } from "sonner";
import { Avatar, Btn, Card, CardHead, Chip, Field, Kpi, Mono, PageHeader, Row, Select, TableWrap, td, th } from "./ui";
import { textareaCls } from "./workflow";
import { useAppState } from "./AppState";
import { severityTone, type Alert } from "@/data/mock";
import { loadReview, saveReview } from "@/lib/event-review-store";
import { clock, dayLabel, localOperator, useRealEvents, type RealEvent } from "@/lib/real-state";

type Review = "In review" | "Acknowledged" | "Closed";

/** Review status of one event, from the alert row of the same id (or from the episode being cleared). */
export function reviewOf(e: RealEvent, alert: Alert | undefined): Review {
  if (alert?.status === "Acknowledged" || alert?.status === "Escalated") return "Acknowledged";
  if (e.cleared || alert?.status === "Cleared") return "Closed";
  return "In review";
}

const initials = (name: string) =>
  name
    .split(/\s+/)
    .map((p) => p[0] ?? "")
    .join("")
    .slice(0, 2)
    .toUpperCase();

const duration = (e: RealEvent) => {
  if (e.endMs === null) return "ongoing";
  const s = Math.max(0, Math.round((e.endMs - e.startMs) / 1000));
  return s < 60 ? `${s} s` : `${Math.floor(s / 60)} m ${String(s % 60).padStart(2, "0")} s`;
};

export function RealEventHistory() {
  const { events, loaded, error } = useRealEvents();
  const { items } = useAppState();
  const [severity, setSeverity] = useState("All severities"),
    [review, setReview] = useState("All review statuses");
  const rows = events
    .map((e) => ({ e, alert: items.find((a) => a.id === e.id) }))
    .map((r) => ({ ...r, review: reviewOf(r.e, r.alert) }))
    .filter(
      (r) =>
        (severity === "All severities" || r.e.severity === severity) &&
        (review === "All review statuses" || r.review === review),
    );
  const today = new Date().setHours(0, 0, 0, 0);
  const openAlerts = items.filter((a) => a.status !== "Cleared" && a.status !== "Acknowledged").length;
  return (
    <>
      <PageHeader
        eyebrow="Review & insights / Events"
        title="Event history"
        subtitle="An auditable record of detections, responses and review outcomes."
        actions={
          <Link to="/report/$reportId" params={{ reportId: "register" }}>
            <Btn>
              <Download className="size-4" />
              Export register
            </Btn>
          </Link>
        }
      />
      <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi label="Recorded today" value={String(events.filter((e) => e.startMs >= today).length).padStart(2, "0")} />
        <Kpi label="Open alerts" value={String(openAlerts).padStart(2, "0")} />
        <Kpi
          label="Awaiting review"
          value={String(events.filter((e) => reviewOf(e, items.find((a) => a.id === e.id)) === "In review").length).padStart(2, "0")}
        />
        <Kpi label="Data scope" value={<span className="font-sans text-lg">Recorded by this device</span>} />
      </div>
      <Card>
        <div className="grid gap-3 p-4 sm:grid-cols-2">
          <Field label="Severity">
            <Select options={["All severities", "Critical", "Warning", "Advisory"]} value={severity} onChange={setSeverity} />
          </Field>
          <Field label="Review status">
            <Select options={["All review statuses", "In review", "Acknowledged", "Closed"]} value={review} onChange={setReview} />
          </Field>
        </div>
        <TableWrap>
          <thead>
            <tr>
              {["Event ID", "Occurred", "Severity", "Event type", "Review status", "Owner", ""].map((h) => (
                <th className={th} key={h}>
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map(({ e, alert, review: rv }) => (
              <tr key={e.id} className="hover:bg-field">
                <td className={td}>
                  <Link className="text-primary" to="/events/$eventId" params={{ eventId: e.id }}>
                    <Mono>{e.id}</Mono>
                  </Link>
                </td>
                <td className={td}>
                  <Mono>
                    {dayLabel(e.startMs)} {clock(e.startMs)}
                  </Mono>
                </td>
                <td className={td}>
                  <Chip tone={severityTone(e.severity)}>{e.severity}</Chip>
                </td>
                <td className={td}>
                  {e.condition || "Safety condition"}
                  <div className="text-xs text-muted-foreground">{e.camId}</div>
                </td>
                <td className={td}>
                  <Chip tone={rv === "Closed" ? "success" : "neutral"}>{rv}</Chip>
                </td>
                <td className={td}>
                  {alert && alert.owner && alert.owner !== "Unassigned" ? <Avatar initials={initials(alert.owner)} /> : <span className="text-muted-foreground">Unassigned</span>}
                </td>
                <td className={td}>
                  <Link to="/events/$eventId" params={{ eventId: e.id }} aria-label={`Investigate ${e.id}`}>
                    <ChevronRight className="size-4 text-primary" />
                  </Link>
                </td>
              </tr>
            ))}
            {!rows.length && (
              <tr>
                <td colSpan={7} className="p-8 text-center text-muted-foreground">
                  {error
                    ? "Events could not be read from the host."
                    : !loaded
                      ? "Loading events…"
                      : events.length
                        ? "No events match these filters."
                        : "No events recorded."}
                </td>
              </tr>
            )}
          </tbody>
        </TableWrap>
        <div className="p-4 text-xs text-muted-foreground">{rows.length} recorded events</div>
      </Card>
    </>
  );
}

const STATE_NOTE: Record<string, string> = {
  Yellow: "Monitoring (advisory)",
  Orange: "Warning",
  Red: "Critical",
  None: "Condition cleared",
};

const CLASSES = ["Confirmed unsafe exposure", "False positive", "Insufficient evidence"];

export function RealEventInvestigation({ eventId }: { eventId: string }) {
  const { events, loaded } = useRealEvents();
  const { items } = useAppState();
  const [classification, setClassification] = useState(CLASSES[0]!),
    [notes, setNotes] = useState(""),
    [saved, setSaved] = useState(false);
  useEffect(() => {
    loadReview(eventId)
      .then((r) => {
        if (!r) return;
        setClassification(r.classification);
        setNotes(r.notes);
        setSaved(true);
      })
      .catch(() => {});
  }, [eventId]);
  const event = events.find((e) => e.id === eventId);
  if (!event)
    return (
      <>
        <PageHeader
          eyebrow="Review / Events"
          title={loaded ? "Event unavailable" : "Loading event"}
          subtitle={loaded ? `No recorded event matches ${eventId}.` : "Reading the event record from the host."}
        />
        <Link to="/events">
          <Btn>Back to event history</Btn>
        </Link>
      </>
    );
  const alert = items.find((a) => a.id === event.id);
  return (
    <>
      <PageHeader
        eyebrow={`Review / ${event.id}`}
        title="Event investigation"
        subtitle={[event.place || event.camId, dayLabel(event.startMs)].filter(Boolean).join(" / ")}
        actions={
          <>
            <Link to="/report/$reportId" params={{ reportId: "event" }} search={{ eventId: event.id }}>
              <Btn>
                <Download className="size-4" />
                Export evidence
              </Btn>
            </Link>
            <Link to="/actions">
              <Btn variant="primary">
                <Plus className="size-4" />
                Create action
              </Btn>
            </Link>
          </>
        }
      />
      <div className="mb-4 flex flex-wrap gap-2">
        <Chip tone={severityTone(event.severity)}>{event.severity}</Chip>
        <Chip tone={event.cleared ? "success" : "critical"} dot>
          {event.cleared ? "Hazard cleared" : "Hazard active"}
        </Chip>
        <Chip>{saved ? "Review saved" : reviewOf(event, alert)}</Chip>
        <Chip>Duration {duration(event)}</Chip>
      </div>
      <div className="grid gap-4 xl:grid-cols-[minmax(0,1.65fr)_minmax(300px,1fr)]">
        <div className="space-y-4">
          <Card>
            <CardHead title="State changes" sub={`${event.camId} · recorded by the host`} />
            <div className="p-4">
              {event.steps.map((t, i) => (
                <div key={`${t.ms}-${i}`} className="flex gap-4 pb-5 last:pb-0">
                  <div className="flex flex-col items-center">
                    <span className="flex size-6 items-center justify-center rounded-full bg-success-bg text-xs text-success">✓</span>
                    {i < event.steps.length - 1 && <span className="mt-1 w-px flex-1 bg-border" />}
                  </div>
                  <div>
                    <div className="flex items-center gap-3">
                      <strong>{STATE_NOTE[t.to] ?? t.to}</strong>
                      <Mono className="text-muted-foreground">{clock(t.ms)}</Mono>
                    </div>
                    <p className="text-xs text-muted-foreground">
                      {t.from} to {t.to}
                    </p>
                  </div>
                </div>
              ))}
            </div>
          </Card>
          <Card>
            <CardHead title="Evidence" />
            <div className="space-y-2 p-4 text-sm text-muted-foreground">
              Saved clips for critical events are listed in the Evidence library.
              <div>
                <Link to="/evidence">
                  <Btn>Open evidence library</Btn>
                </Link>
              </div>
            </div>
          </Card>
        </div>
        <div className="space-y-4">
          <Card>
            <CardHead title="Review decision" />
            <div className="space-y-4 p-4">
              <Field label="Classification">
                <Select
                  value={classification}
                  onChange={(v) => {
                    setClassification(v);
                    setSaved(false);
                  }}
                  options={CLASSES}
                />
              </Field>
              <Field label="Review notes">
                <textarea
                  className={textareaCls}
                  value={notes}
                  placeholder="What was observed and what was done."
                  onChange={(e) => {
                    setNotes(e.target.value);
                    setSaved(false);
                  }}
                />
              </Field>
              <Btn
                variant="primary"
                onClick={async () => {
                  try {
                    await saveReview(eventId, {
                      classification,
                      notes,
                      savedAt: new Date().toLocaleString("en-GB"),
                      by: localOperator.name,
                    });
                    setSaved(true);
                    toast.success("Review decision saved on this device");
                  } catch {
                    toast.error("Could not save the review on this device");
                  }
                }}
              >
                {saved ? "Review saved" : "Save review"}
              </Btn>
            </div>
          </Card>
          <Card>
            <CardHead title="Detection context" />
            <div className="px-4 pb-3">
              <Row k="Camera" v={<Mono>{event.camId}</Mono>} />
              <Row k="Condition" v={event.condition || "Unknown"} />
              <Row k="Place" v={event.place || "Unknown"} />
              <Row k="Response" v={alert ? `${alert.status}${alert.owner && alert.owner !== "Unassigned" ? ` by ${alert.owner}` : ""}` : "None recorded"} />
            </div>
          </Card>
        </div>
      </div>
    </>
  );
}
