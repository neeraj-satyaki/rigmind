import { useEffect, useState } from "react";
import { Link } from "@tanstack/react-router";
import { ChevronRight, Download, Play, Pause, RefreshCw, Plus, Grid2X2 } from "lucide-react";
import { toast } from "sonner";
import {
  Avatar,
  Btn,
  Callout,
  CameraTile,
  Card,
  CardHead,
  Chip,
  Field,
  Kpi,
  Mono,
  PageHeader,
  Row,
  Select,
  TableWrap,
  Tabs,
  pageHead,
  td,
  th,
} from "./ui";
import { textareaCls } from "./workflow";
import {
  eventHistory,
  clips,
  responseTimeline,
  severityTone,
  reviewDefaults,
  demoPolicy,
} from "@/data/mock";
import { useServerFn } from "@tanstack/react-start";
import { draftEventSummary, type EventSummaryDraft } from "@/lib/event-summary.functions";
import {
  loadReview,
  loadSummary,
  saveReview,
  saveSummary,
  type HseApproval,
  type StoredSummary,
} from "@/lib/event-review-store";

const stamp = () =>
  new Date().toLocaleString("en-GB", { timeZone: "Asia/Riyadh", dateStyle: "medium", timeStyle: "short" }) + " AST";

function DraftSummary({ eventId, context, sources }: { eventId: string; context: string; sources: string[] }) {
  const run = useServerFn(draftEventSummary);
  const [stored, setStored] = useState<StoredSummary | null>(null),
    [loading, setLoading] = useState(false),
    [error, setError] = useState("");
  useEffect(() => {
    loadSummary(eventId).then((s) => s && setStored(s)).catch(() => {});
  }, [eventId]);
  const draft: EventSummaryDraft | null = stored?.draft ?? null;
  const approval: HseApproval | null = stored?.approval ?? null;
  const approved = approval !== null;
  const persist = async (next: StoredSummary) => {
    setStored(next);
    try {
      await saveSummary(next);
    } catch {
      toast.error("Could not save on this device");
    }
  };
  const approve = async () => {
    if (!stored) return;
    await persist({ ...stored, approval: { by: "Maha S.", role: "HSE reviewer", at: stamp() } });
    toast.success("Summary approved by HSE for report inclusion");
  };
  const generate = async () => {
    setLoading(true);
    setError("");
    try {
      const r = await run({ data: { eventId, context } });
      if (r.error) setError(r.error);
      else if (r.draft)
        await persist({ eventId, draft: r.draft, sources, generatedAt: stamp(), approval: null });
    } catch {
      setError("The draft summary could not be generated.");
    } finally {
      setLoading(false);
    }
  };
  return (
    <Card>
      <CardHead
        title="Draft summary"
        right={<Chip tone={approved ? "success" : "warning"}>{approved ? "HSE approved for reports" : "Requires HSE review"}</Chip>}
      />
      <div className="space-y-3 p-4 text-[13px]">
        {!draft && !loading && (
          <p className="text-muted-foreground">Generate an AI draft from the event history, evidence references and notes.</p>
        )}
        {loading && <p className="text-muted-foreground">Drafting summary…</p>}
        {error && <p className="text-critical">{error}</p>}
        {draft && (
          <>
            <p>{draft.summary}</p>
            <div>
              <div className="eyebrow mb-1">Recorded facts</div>
              <ul className="list-disc space-y-1 pl-5">{draft.recordedFacts.map((f) => <li key={f}>{f}</li>)}</ul>
            </div>
            <div>
              <div className="eyebrow mb-1">Missing information</div>
              <ul className="list-disc space-y-1 pl-5 text-warning">{draft.missingInformation.map((f) => <li key={f}>{f}</li>)}</ul>
            </div>
            <div>
              <div className="eyebrow mb-1">Source references</div>
              <ul className="list-disc pl-5 text-[12px] text-muted-foreground">{stored?.sources.map((f) => <li key={f}>{f}</li>)}</ul>
            </div>
            <p className="text-[11px] text-muted-foreground">
              {approval
                ? `Approved by ${approval.by} (${approval.role}) · ${approval.at}. Included in weekly reports.`
                : `AI draft generated ${stored?.generatedAt ?? ""}. Not included in reports until an HSE reviewer approves it.`}
            </p>
          </>
        )}
        <div className="flex flex-wrap gap-2">
          <Btn onClick={generate} disabled={loading}>{draft ? "Regenerate draft" : "Generate draft"}</Btn>
          {draft && (
            <Btn variant="primary" disabled={approved} onClick={approve}>
              {approved ? `Approved by ${approval.by} (HSE)` : "Approve as HSE reviewer"}
            </Btn>
          )}
        </div>
      </div>
    </Card>
  );
}

export function EventHistory() {
  const [severity, setSeverity] = useState("All severities"),
    [review, setReview] = useState("All review statuses");
  const rows = eventHistory.filter(
    (e) =>
      (severity === "All severities" || e.severity === severity) &&
      (review === "All review statuses" || e.review === review),
  );
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
        <Kpi label="Verified today" value="42" />
        <Kpi label="Open alerts" value="07" note="2 critical need attention" noteTone="critical" />
        <Kpi label="Awaiting review" value="04" />
        <Kpi
          label="Data scope"
          value={<span className="font-sans text-lg">Test events excluded</span>}
        />
      </div>
      <Card>
        <div className="grid gap-3 p-4 sm:grid-cols-2 xl:grid-cols-4">
          <Field label="Period">
            <Select options={["06 Oct 2026"]} />
          </Field>
          <Field label="Rig">
            <Select options={["All assigned rigs"]} />
          </Field>
          <Field label="Severity">
            <Select
              options={["All severities", "Critical", "Warning", "Advisory"]}
              value={severity}
              onChange={setSeverity}
            />
          </Field>
          <Field label="Review status">
            <Select
              options={["All review statuses", "In review", "Acknowledged", "Closed"]}
              value={review}
              onChange={setReview}
            />
          </Field>
        </div>
        <TableWrap>
          <thead>
            <tr>
              {["Event ID", "Occurred", "Severity", "Event type", "Review status", "Owner", ""].map(
                (h) => (
                  <th className={th} key={h}>
                    {h}
                  </th>
                ),
              )}
            </tr>
          </thead>
          <tbody>
            {rows.map((e) => (
              <tr key={e.id} className="hover:bg-field">
                <td className={td}>
                  <Link className="text-primary" to="/events/$eventId" params={{ eventId: e.id }}>
                    <Mono>{e.id}</Mono>
                  </Link>
                </td>
                <td className={td}>
                  <Mono>{e.occurred}</Mono>
                </td>
                <td className={td}>
                  <Chip tone={severityTone(e.severity)}>{e.severity}</Chip>
                </td>
                <td className={td}>{e.type}</td>
                <td className={td}>
                  <Chip tone={e.review === "Closed" ? "success" : "neutral"}>{e.review}</Chip>
                </td>
                <td className={td}>
                  <Avatar initials={e.owner} />
                </td>
                <td className={td}>
                  <Link
                    to="/events/$eventId"
                    params={{ eventId: e.id }}
                    aria-label={`Investigate ${e.id}`}
                  >
                    <ChevronRight className="size-4 text-primary" />
                  </Link>
                </td>
              </tr>
            ))}
            {!rows.length && (
              <tr>
                <td colSpan={7} className="p-8 text-center text-muted-foreground">
                  No events match these filters.
                </td>
              </tr>
            )}
          </tbody>
        </TableWrap>
        <div className="flex items-center justify-between p-4 text-xs text-muted-foreground">
          <span>{rows.length} illustrative records</span>
          <div className="flex items-center gap-3">
            <Btn disabled aria-label="Previous page">
              ‹
            </Btn>
            Page 1 of 1
            <Btn disabled aria-label="Next page">
              ›
            </Btn>
          </div>
        </div>
      </Card>
    </>
  );
}

export function EventInvestigation({ eventId }: { eventId: string }) {
  const event = eventHistory.find((e) => e.id === eventId);
  const [playing, setPlaying] = useState(false),
    [position, setPosition] = useState(10),
    [classification, setClassification] = useState(reviewDefaults.classification),
    [notes, setNotes] = useState(reviewDefaults.notes),
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
  useEffect(() => {
    if (!playing) return;
    const id = setInterval(
      () =>
        setPosition((p) => {
          if (p >= 30) {
            setPlaying(false);
            return 30;
          }
          return p + 1;
        }),
      1000,
    );
    return () => clearInterval(id);
  }, [playing]);
  if (!event)
    return (
      <>
        <PageHeader
          eyebrow="Review / Events"
          title="Event unavailable"
          subtitle={`No demonstration record matches ${eventId}.`}
        />
        <Link to="/events">
          <Btn>Back to event history</Btn>
        </Link>
      </>
    );
  const main = event.id === "EV-1042";
  return (
    <>
      <PageHeader
        eyebrow={`Review / ${event.id}`}
        title="Event investigation"
        subtitle="Demo rig 01 / Derrick board / 06 Oct 2026"
        actions={
          <>
            <Link
              to="/report/$reportId"
              params={{ reportId: "event" }}
              search={{ eventId: event.id }}
            >
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
        <Chip tone="success" dot>
          Hazard cleared
        </Chip>
        <Chip>{saved ? "Review saved" : main ? "Review in progress" : event.review}</Chip>
        {main && (
          <>
            <Chip>44 s exposure</Chip>
            <Chip>Acknowledged in 8 s</Chip>
          </>
        )}
      </div>
      <div className="grid gap-4 xl:grid-cols-[minmax(0,1.65fr)_minmax(300px,1fr)]">
        <div className="space-y-4">
          <CameraTile id="CAM-01" area="Derrick board" overlay={main}>
            <span className="absolute left-3 top-3 rounded-sm bg-slate px-2 py-1 text-[10px] font-semibold text-slate-foreground">
              EVENT REPLAY
            </span>
          </CameraTile>
          <div className="rounded-lg border border-border bg-card p-4">
            <div className="flex items-center gap-3">
              <Btn
                aria-label={playing ? "Pause replay" : "Play replay"}
                title={playing ? "Pause replay" : "Play replay"}
                onClick={() => {
                  if (position === 30) setPosition(0);
                  setPlaying(!playing);
                }}
              >
                {playing ? <Pause className="size-4" /> : <Play className="size-4" />}
              </Btn>
              <Mono>00:{String(position).padStart(2, "0")} / 00:30</Mono>
              <Chip>Simulated replay</Chip>
            </div>
            <input
              aria-label="Replay position"
              className="mt-4 w-full accent-primary"
              type="range"
              min={0}
              max={30}
              value={position}
              onChange={(e) => setPosition(Number(e.target.value))}
            />
            <div className="flex justify-between font-mono text-[10px] text-muted-foreground">
              <span>09:42:08</span>
              <span>Event trigger · 09:42:18</span>
              <span>09:42:38</span>
            </div>
          </div>
          <Card>
            <CardHead title="Response timeline" />
            <div className="p-4">
              {responseTimeline.map((t, i) => (
                <div key={t.time} className="flex gap-4 pb-5 last:pb-0">
                  <div className="flex flex-col items-center">
                    <span className="flex size-6 items-center justify-center rounded-full bg-success-bg text-xs text-success">
                      ✓
                    </span>
                    {i < 3 && <span className="mt-1 w-px flex-1 bg-border" />}
                  </div>
                  <div>
                    <div className="flex items-center gap-3">
                      <strong>{t.label}</strong>
                      <Mono className="text-muted-foreground">{t.time}</Mono>
                    </div>
                    <p className="text-xs text-muted-foreground">{t.note}</p>
                  </div>
                </div>
              ))}
            </div>
          </Card>
        </div>
        <div className="space-y-4">
          <DraftSummary
            eventId={event.id}
            sources={[
              `Event history: ${event.id} (${event.occurred}, ${event.severity})`,
              ...(main ? ["Response timeline: 4 recorded steps"] : []),
              ...clips.filter((c) => c.id === event.id).map((c) => `Evidence: ${c.id} clip, ${c.cam} ${c.area}, ${c.time}`),
              `Safety rule DZ-01 v${demoPolicy.deployedRuleVersion}; model demo-v1`,
              "Review notes: Maha S. (HSE reviewer)",
            ]}
            context={[
              `Event history: ${event.id} at ${event.occurred}, severity ${event.severity}, type ${event.type}, review status ${event.review}, owner ${event.owner}.`,
              main ? `Response timeline:\n${responseTimeline.map((t) => `${t.time} ${t.label} - ${t.note}`).join("\n")}` : "Response timeline: not recorded for this event.",
              `Evidence references: ${clips.filter((c) => c.id === event.id).map((c) => `${c.id} clip on ${c.cam} (${c.area}) at ${c.time}, 30 s`).join("; ") || "none recorded"}. Safety rule DZ-01 version ${demoPolicy.deployedRuleVersion}; model demo-v1.`,
              `Operator/reviewer notes (classification: ${classification}): ${notes}`,
            ].join("\n\n")}
          />
          <Card>
            <CardHead title="Review decision" right={<Chip tone="advisory">Assigned</Chip>} />
            <div className="space-y-4 p-4">
              <div className="flex gap-2">
                <Avatar initials="MS" />
                <div>
                  <strong>Maha S.</strong>
                  <p className="text-xs text-muted-foreground">HSE reviewer</p>
                </div>
              </div>
              <Field label="Classification">
                <Select
                  value={classification}
                  onChange={(v) => {
                    setClassification(v);
                    setSaved(false);
                  }}
                  options={[
                    reviewDefaults.classification,
                    "False positive",
                    "Insufficient evidence",
                  ]}
                />
              </Field>
              <Row k="Evidence confidence" v={reviewDefaults.confidence} />
              <Field label="Review notes">
                <textarea
                  className={textareaCls}
                  value={notes}
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
                    await saveReview(eventId, { classification, notes, savedAt: stamp(), by: "Maha S." });
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
              <Row
                k="Safety rule"
                v={<Mono>DZ-01 / version {demoPolicy.deployedRuleVersion}</Mono>}
              />
              <Row k="Activity" v="Tubular handling" />
              <Row k="Model version" v={<Mono>demo-v1</Mono>} />
              <Row k="Evidence" v={<Chip tone="success">Clip ready</Chip>} />
            </div>
          </Card>
        </div>
      </div>
    </>
  );
}

export function EvidenceLibrary() {
  const [tab, setTab] = useState("clips"),
    [camera, setCamera] = useState("All cameras"),
    [grid, setGrid] = useState(true);
  const rows = clips.filter((c) => camera === "All cameras" || c.cam === camera);
  return (
    <>
      <PageHeader
        eyebrow="Review & insights / Evidence"
        title="Evidence library"
        subtitle="Recorded moments. Traceable decisions."
        actions={
          <>
            <Btn onClick={() => setGrid(!grid)}>
              <Grid2X2 className="size-4" />
              {grid ? "List view" : "Grid view"}
            </Btn>
            <Btn onClick={() => toast.success("3 demonstration clips are ready")}>
              <RefreshCw className="size-4" />
              Refresh clips
            </Btn>
          </>
        }
      />
      <Tabs
        value={tab}
        onChange={setTab}
        tabs={[
          { id: "clips", label: "Event clips" },
          { id: "continuous", label: "Continuous recordings" },
          { id: "hold", label: "On hold" },
        ]}
      />
      <div className="mb-4 flex flex-wrap items-center gap-2">
        <Select className="w-36" options={["Today"]} />
        <Select
          className="w-40"
          options={["All cameras", "CAM-01", "CAM-02", "CAM-03"]}
          value={camera}
          onChange={setCamera}
        />
        <Select className="w-40" options={["Ready"]} />
        <span className="ml-auto text-xs text-muted-foreground">
          {tab === "clips" ? rows.length : 0} illustrative clips
        </span>
      </div>
      {tab === "clips" ? (
        <div className={grid ? "grid gap-4 md:grid-cols-2 xl:grid-cols-3" : "grid gap-4"}>
          {rows.map((c) => (
            <div key={c.id} className="space-y-2">
              <CameraTile id={c.cam} area={c.area} overlay={c.id === "EV-1042"}>
                <span className="absolute bottom-3 right-3 rounded-sm bg-slate px-2 py-1 font-mono text-xs text-slate-foreground">
                  00:30
                </span>
              </CameraTile>
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div>
                  <Mono>{c.id}</Mono>
                  <span className="ml-2 text-xs text-muted-foreground">{c.time}</span>
                  <div className="mt-1 flex gap-2">
                    <Chip tone={severityTone(c.severity)}>{c.severity}</Chip>
                    <Chip tone="success" dot>
                      Ready
                    </Chip>
                  </div>
                </div>
                <Link to="/events/$eventId" params={{ eventId: c.id }}>
                  <Btn>
                    <Play className="size-3.5" />
                    Review clip
                  </Btn>
                </Link>
              </div>
            </div>
          ))}
        </div>
      ) : (
        <div className="py-16 text-center text-muted-foreground">
          {tab === "hold"
            ? "No evidence is on hold."
            : "No continuous recordings are available in this demonstration."}
        </div>
      )}
      <div className="mt-6 border-t border-border pt-5">
        <h2 className="font-semibold">Evidence policy</h2>
        <div className="my-3 grid gap-4 sm:grid-cols-3">
          <Row k="Rolling footage" v={`${demoPolicy.rollingDays} days`} />
          <Row k="Event clips" v={`${demoPolicy.eventDays} days`} />
          <Row k="Role restricted" v="Downloads are audited" />
        </div>
        <Callout title="Demonstration policy">
          Production retention requires customer approval.
        </Callout>
      </div>
    </>
  );
}
