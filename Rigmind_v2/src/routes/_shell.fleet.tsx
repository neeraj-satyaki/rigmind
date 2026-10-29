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
import { eventActivity, kpis, rigs } from "@/data/mock";

export const Route = createFileRoute("/_shell/fleet")({
  head: pageHead("Fleet overview", "Fleet-wide safety status, coverage and alerts."),
  component: Fleet,
});

function Fleet() {
  return (
    <>
      <PageHeader
        eyebrow="Operations / Overview"
        title="Fleet overview"
        subtitle="Tuesday, 6 October 2026 · Day shift"
        actions={
          <>
            <NoteDialog
              title="Selected shift"
              label="Today"
              icon={<CalendarDays className="size-4" />}
              text="06 October 2026 · Day shift · 06:00–18:00 AST. These are the available illustrative shift records."
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
        <Kpi label="Monitored rigs" value={kpis.rigs.value} note={kpis.rigs.note} />
        <Kpi
          label="Cameras online"
          value={kpis.cameras.value}
          note={kpis.cameras.note}
          noteTone="success"
        />
        <Kpi
          label="Open safety alerts"
          value={kpis.alerts.value}
          note={kpis.alerts.note}
          noteTone="critical"
        />
        <Kpi label="Verified events" value={kpis.verified.value} note={kpis.verified.note} />
      </div>
      <div className="mt-4 grid gap-4 xl:grid-cols-3">
        <Card className="xl:col-span-2">
          <CardHead title="Event activity" sub="Verified events · last 7 days" />
          <div className="h-64 p-3">
            <ResponsiveContainer>
              <LineChart data={eventActivity}>
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
          </div>
        </Card>
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
        <Card className="xl:col-span-2">
          <CardHead
            title="Monitoring snapshot"
            sub="Demo rig 01"
            right={
              <Link to="/live">
                <Btn>Open live view</Btn>
              </Link>
            }
          />
          <div className="grid gap-3 p-4 sm:grid-cols-2">
            <CameraTile id="CAM-01" area="Derrick board" />
            <CameraTile id="CAM-05" area="Pipe racking" />
          </div>
        </Card>
      </div>
    </>
  );
}
