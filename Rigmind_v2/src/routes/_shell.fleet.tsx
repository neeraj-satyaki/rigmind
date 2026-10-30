import { createFileRoute, Link } from "@tanstack/react-router";
import { CalendarDays, Download, AlertTriangle } from "lucide-react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
} from "recharts";
import {
  Btn,
  Card,
  CardHead,
  CameraTile,
  Chip,
  Kpi,
  Mono,
  PageHeader,
  pageHead,
} from "@/components/rig/ui";
import { NoteDialog } from "@/components/rig/workflow";
import { eventActivity, kpis, rigs, severityTone } from "@/data/mock";
import { demoDataEnabled } from "@/lib/demo-mode";
import { StreamVideo } from "@/components/rig/StreamVideo";
import { useAppState } from "@/components/rig/AppState";
import { availability, useCameras } from "@/lib/camera-status";
import { dayLabel, useRealCameras, useRealEvents, useStreamSources } from "@/lib/real-state";

export const Route = createFileRoute("/_shell/fleet")({
  head: pageHead("Fleet overview", "Fleet-wide safety status, coverage and alerts."),
  component: Fleet,
});

const DAY_MS = 86400000;
const SEV_ORDER = ["Critical", "Warning", "Advisory"];

function Fleet() {
  const demo = demoDataEnabled();
  const { items } = useAppState();
  const status = useCameras();
  const sources = useStreamSources();
  const realCams = useRealCameras(sources);
  const { events } = useRealEvents();
  const open = items.filter((a) => a.status === "Open" || a.status === "Escalated" || a.status === "Monitoring");
  const worst = [...open].sort((a, b) => SEV_ORDER.indexOf(a.severity) - SEV_ORDER.indexOf(b.severity))[0];
  const online = realCams.filter((c) => status[c.id]?.state === "online").length;
  const avail = realCams.length
    ? (realCams.reduce((n, c) => n + availability(status[c.id]), 0) / realCams.length) * 100
    : 0;
  const todayMs = new Date().setHours(0, 0, 0, 0);
  // Events per day for the last 7 days, from the host's alarm records.
  const activity = Array.from({ length: 7 }, (_, i) => {
    const from = todayMs - (6 - i) * DAY_MS;
    return { day: dayLabel(from).slice(0, 6), events: events.filter((e) => e.startMs >= from && e.startMs < from + DAY_MS).length };
  });
  return (
    <>
      <PageHeader
        eyebrow="Operations / Overview"
        title="Fleet overview"
        subtitle={
          demo
            ? "Tuesday, 6 October 2026 · Day shift"
            : new Date().toLocaleDateString("en-GB", { weekday: "long", day: "numeric", month: "long", year: "numeric" })
        }
        actions={
          <>
            <NoteDialog
              title="Selected shift"
              label="Today"
              icon={<CalendarDays className="size-4" />}
              text={
                demo
                  ? "06 October 2026 · Day shift · 06:00–18:00 AST. These are the available illustrative shift records."
                  : "Today, from the events and camera state recorded on this device."
              }
            />
            <Link to="/report/$reportId" params={{ reportId: "weekly" }}>
              <Btn variant="primary">
                <Download className="size-4" /> Export overview
              </Btn>
            </Link>
          </>
        }
      />
      <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        {demo ? (
          <>
            <Kpi label="Monitored rigs" value={kpis.rigs.value} note={kpis.rigs.note} />
            <Kpi label="Cameras online" value={kpis.cameras.value} note={kpis.cameras.note} noteTone="success" />
            <Kpi label="Open safety alerts" value={kpis.alerts.value} note={kpis.alerts.note} noteTone="critical" />
            <Kpi label="Verified events" value={kpis.verified.value} note={kpis.verified.note} />
          </>
        ) : (
          <>
            <Kpi
              label="Configured cameras"
              value={String(realCams.length).padStart(2, "0")}
              note={realCams.length ? undefined : "None added yet"}
            />
            <Kpi
              label="Cameras online"
              value={`${online}/${realCams.length}`}
              note={realCams.length ? `${avail.toFixed(1)}% availability since start` : undefined}
              {...(realCams.length && online === realCams.length ? { noteTone: "success" as const } : {})}
            />
            <Kpi
              label="Open safety alerts"
              value={String(open.length).padStart(2, "0")}
              note={open.length ? `${open.filter((a) => a.severity === "Critical").length} critical` : "No alerts yet"}
              {...(open.some((a) => a.severity === "Critical") ? { noteTone: "critical" as const } : {})}
            />
            <Kpi
              label="Events recorded"
              value={String(events.length).padStart(2, "0")}
              note={events.length ? `${events.filter((e) => e.startMs >= todayMs).length} today` : "No events recorded"}
            />
          </>
        )}
      </div>
      <div className="mt-4 grid gap-4 xl:grid-cols-3">
        <Card className="xl:col-span-2">
          <CardHead title="Event activity" sub={demo ? "Verified events · last 7 days" : "Recorded events · last 7 days"} />
          <div className="h-64 p-3">
            {!demo && events.length === 0 ? (
              <div className="flex h-full items-center justify-center text-sm text-muted-foreground">No events recorded</div>
            ) : (
              <ResponsiveContainer>
                <LineChart data={demo ? eventActivity : activity}>
                  <CartesianGrid stroke="var(--border)" vertical={false} />
                  <XAxis
                    dataKey="day"
                    tick={{ fontSize: 11, fill: "var(--muted-foreground)" }}
                    axisLine={false}
                    tickLine={false}
                  />
                  <YAxis
                    tick={{ fontSize: 11, fill: "var(--muted-foreground)" }}
                    axisLine={false}
                    tickLine={false}
                    width={28}
                    allowDecimals={false}
                  />
                  <Tooltip />
                  <Line
                    type="monotone"
                    dataKey="events"
                    stroke="var(--primary)"
                    strokeWidth={2}
                    dot={{ r: 3 }}
                  />
                </LineChart>
              </ResponsiveContainer>
            )}
          </div>
        </Card>
        {demo ? (
          <Card>
            <CardHead
              title="Needs your attention"
              right={
                <Chip tone="critical" dot>
                  Critical
                </Chip>
              }
            />
            <div className="space-y-3 p-4">
              <div className="flex items-center gap-2 text-xs text-muted-foreground">
                <AlertTriangle className="size-4 text-critical" /> 8 seconds ago
              </div>
              <div className="text-base font-semibold">Occupied zone during equipment movement</div>
              <div className="text-[13px] text-muted-foreground">
                Demo rig 01 · Derrick board · <Mono>EV-1042</Mono>
              </div>
              <Link to="/live">
                <Btn variant="danger" className="w-full">
                  Review alert
                </Btn>
              </Link>
            </div>
          </Card>
        ) : (
          <Card>
            <CardHead
              title="Needs your attention"
              right={
                worst ? (
                  <Chip tone={severityTone(worst.severity)} dot>
                    {worst.severity}
                  </Chip>
                ) : undefined
              }
            />
            <div className="space-y-3 p-4">
              {worst ? (
                <>
                  <div className="flex items-center gap-2 text-xs text-muted-foreground">
                    <AlertTriangle className="size-4 text-critical" /> {worst.age} since first seen
                  </div>
                  <div className="text-base font-semibold">{worst.condition}</div>
                  <div className="text-[13px] text-muted-foreground">
                    {[worst.rig, worst.area].filter(Boolean).join(" · ")}
                    {worst.rig || worst.area ? " · " : ""}
                    <Mono>{worst.id}</Mono>
                  </div>
                  <Link to="/live">
                    <Btn variant="danger" className="w-full">
                      Review alert
                    </Btn>
                  </Link>
                </>
              ) : (
                <div className="text-sm text-muted-foreground">No active alert. Nothing needs attention right now.</div>
              )}
            </div>
          </Card>
        )}
        {demo ? (
          <Card>
            <CardHead title="Fleet readiness" />
            <div className="divide-y divide-border">
              {rigs.map((r) => (
                <div key={r.id} className="flex items-center justify-between px-4 py-3">
                  <div>
                    <div className="font-medium">{r.name}</div>
                    <div className="text-xs text-muted-foreground">{r.region}</div>
                  </div>
                  <div className="text-right">
                    <Chip tone={r.status === "Ready" ? "success" : "warning"}>{r.status}</Chip>
                    <div className="mt-1 text-[11px] text-muted-foreground">
                      {r.openAlerts} open alert{r.openAlerts > 1 ? "s" : ""}
                    </div>
                  </div>
                </div>
              ))}
            </div>
          </Card>
        ) : (
          <Card>
            <CardHead title="Camera status" />
            <div className="divide-y divide-border">
              {realCams.map((c) => {
                const st = status[c.id]?.state;
                return (
                  <div key={c.id} className="flex items-center justify-between px-4 py-3">
                    <Mono className="font-semibold">{c.id}</Mono>
                    <Chip tone={st === "online" ? "success" : st === "offline" ? "critical" : "warning"}>
                      {st ?? "Not reported"}
                    </Chip>
                  </div>
                );
              })}
              {!realCams.length && <div className="px-4 py-6 text-sm text-muted-foreground">No cameras configured.</div>}
            </div>
          </Card>
        )}
        <Card className="xl:col-span-2">
          <CardHead
            title="Monitoring snapshot"
            sub={demo ? "Demo rig 01" : undefined}
            right={
              <Link to="/live">
                <Btn>Open live view</Btn>
              </Link>
            }
          />
          <div className="grid gap-3 p-4 sm:grid-cols-2">
            {demo ? (
              <>
                <CameraTile id="CAM-01" area="Derrick board" />
                <CameraTile id="CAM-05" area="Pipe racking" />
              </>
            ) : (
              realCams.slice(0, 2).map((c) => (
                <CameraTile key={c.id} id={c.id} area={c.area} offline={status[c.id]?.state !== "online"} live>
                  {/* The output stream (with the detections), transcoded small for the tile; Live view has the full size. */}
                  {/* An offline camera already shows "Camera offline" in the tile itself: nothing more to say. */}
                  {sources[c.id] && status[c.id]?.state === "online" && <StreamVideo camId={c.id} uri={sources[c.id]!} profile="small" />}
                </CameraTile>
              ))
            )}
            {!demo && !realCams.length && <div className="text-sm text-muted-foreground">No cameras configured.</div>}
          </div>
        </Card>
      </div>
    </>
  );
}
