import { formatRate } from "@/lib/format-rate";
import { useState } from "react";
import { Link } from "@tanstack/react-router";
import {
  BarChart,
  Bar,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  PieChart,
  Pie,
  Cell,
} from "recharts";
import { Download } from "lucide-react";
import { Btn, Callout, Card, CardHead, Chip, Kpi, PageHeader, Select } from "./ui";
import { severityTrend as demoTrend, heatmap, eventsByType, cameras as demoCameras } from "@/data/mock";
import { useCameras } from "@/lib/camera-status";
import { isNative } from "@/lib/native-bridge";
import { demoDataEnabled } from "@/lib/demo-mode";
import { useRealCameras, useStreamSources } from "@/lib/real-state";
import {
  countSeverity,
  eventsByCondition,
  formatDuration,
  heatLevel,
  mergeTrend,
  realHeatmap,
  realTrend,
  startOfDay,
  summarizeCameras,
  todaysAlerts,
  totalOf,
  useLiveAlerts,
} from "@/lib/live-analytics";

type SevCounts = { critical: number; warning: number; advisory: number };

function LiveChip({ children = "Live" }: { children?: string }) {
  return (
    <Chip tone="success" dot>
      {children}
    </Chip>
  );
}

function AnalyticsNav({ overview = false }: { overview?: boolean }) {
  return (
    <div className="mb-4 flex flex-wrap gap-2 border-b border-border pb-3">
      <Link to="/analytics">
        <Btn variant={overview ? "ghost" : "primary"}>Severity trends</Btn>
      </Link>
      <Link to="/analytics/overview">
        <Btn variant={overview ? "primary" : "ghost"}>Event overview</Btn>
      </Link>
    </div>
  );
}
const colors = ["var(--chart-crit)", "var(--chart-warn)", "var(--chart-adv)"];
export function SafetyAnalytics() {
  const demo = demoDataEnabled();
  const [day, setDay] = useState("06 Oct");
  const allAlerts = useLiveAlerts();
  const now = Date.now();
  const liveRows = todaysAlerts(allAlerts, now);
  const liveCounts = countSeverity(liveRows);
  const liveTotal = totalOf(liveCounts);
  // Sample on: demo history with live alerts on top. Sample off: only the alerts the host has raised.
  const severityTrend = demo ? mergeTrend(demoTrend, liveCounts) : realTrend(allAlerts, now);
  const today = severityTrend[severityTrend.length - 1]?.day;
  const selected = severityTrend.find((d) => d.day === day) ?? severityTrend[severityTrend.length - 1];
  if (!selected) return null;
  const weekTotal = severityTrend.reduce((a, d) => a + d.critical + d.warning + d.advisory, 0);
  const showLive = demo && liveTotal > 0 && selected.day === today;
  const total = selected.critical + selected.warning + selected.advisory;
  const breakdown = [
    { name: "Critical", value: selected.critical },
    { name: "Warning", value: selected.warning },
    { name: "Advisory", value: selected.advisory },
  ];
  return (
    <>
      <PageHeader
        eyebrow="Review & insights / Analytics"
        title="Safety analytics"
        subtitle={demo ? "Verified events across usable monitoring time." : "Alerts raised on this device over the last 7 days."}
        actions={
          <>
            {demo && liveTotal > 0 && <LiveChip>{`Live +${liveTotal} today`}</LiveChip>}
            <Select
              className="w-36"
              options={severityTrend.map((d) => d.day)}
              value={selected.day}
              onChange={setDay}
            />
            <Link to="/report/$reportId" params={{ reportId: "weekly" }}>
              <Btn>
                <Download className="size-4" />
                Export report
              </Btn>
            </Link>
          </>
        }
      />
      <AnalyticsNav />
      {!demo && weekTotal === 0 && (
        <div className="mb-4">
          <Callout title="No data yet">
            {isNative()
              ? "No alerts have been raised in the last 7 days. Counts appear here as the detection pipeline raises alerts."
              : "Alert history is read from the RigMind desktop app. Open this page there to see it."}
          </Callout>
        </div>
      )}
      <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi
          label={`${demo ? "Verified" : "Alerts"} · ${selected.day}`}
          value={String(total).padStart(2, "0")}
          {...(showLive ? { note: <>Demo {total - liveTotal} + <LiveChip>{`Live ${liveTotal}`}</LiveChip></> } : {})}
        />
        {breakdown.map((b, i) => (
          <Kpi
            key={b.name}
            label={b.name}
            value={String(b.value).padStart(2, "0")}
            note={`${total ? ((b.value / total) * 100).toFixed(1) : "0.0"}% of selected day${showLive ? ` · ${liveCounts[b.name.toLowerCase() as "critical"]} live` : ""}`}
            {...(i === 0 ? { noteTone: "critical" as const } : {})}
          />
        ))}
      </div>
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead
            title="Severity trends"
            sub={demo ? "30 Sep–06 Oct 2026 · Verified events · demo history" : `${severityTrend[0]?.day ?? ""}–${today ?? ""} · Alerts raised on this device`}
            right={demo && liveTotal > 0 ? <LiveChip>{`Live +${liveTotal} on ${today}`}</LiveChip> : undefined}
          />
          <div className="h-80 p-4">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={severityTrend}>
                <CartesianGrid stroke="var(--border)" vertical={false} />
                <XAxis dataKey="day" tick={{ fill: "var(--muted-foreground)", fontSize: 11 }} />
                <YAxis tick={{ fill: "var(--muted-foreground)", fontSize: 11 }} />
                <Tooltip />
                {["critical", "warning", "advisory"].map((key, i) => (
                  <Bar
                    key={key}
                    dataKey={key}
                    stackId="events"
                    fill={colors[i]}
                    cursor="pointer"
                    onClick={(data) => {
                      if (typeof data.day === "string") setDay(data.day);
                    }}
                  />
                ))}
              </BarChart>
            </ResponsiveContainer>
          </div>
          <div className="flex justify-center gap-4 pb-4 text-xs">
            {breakdown.map((b, i) => (
              <span key={b.name} className="flex items-center gap-1.5">
                <span
                  className={`size-2.5 rounded-sm ${["bg-chart-crit", "bg-chart-warn", "bg-chart-adv"][i]}`}
                />
                {b.name}
              </span>
            ))}
          </div>
        </Card>
        <Card>
          <CardHead title="Selected-day breakdown" sub={`${selected.day} · ${total} ${demo ? "verified events" : "alerts"}`} />
          <div className="relative h-60">
            <ResponsiveContainer>
              <PieChart>
                <Pie
                  data={breakdown}
                  dataKey="value"
                  innerRadius={65}
                  outerRadius={88}
                  paddingAngle={2}
                >
                  {breakdown.map((b, i) => (
                    <Cell key={b.name} fill={colors[i]} />
                  ))}
                </Pie>
                <Tooltip />
              </PieChart>
            </ResponsiveContainer>
            <div className="pointer-events-none absolute inset-0 flex flex-col items-center justify-center">
              <strong className="font-mono text-3xl">{total}</strong>
              <span className="text-xs text-muted-foreground">{demo ? "Verified" : "Alerts"}</span>
            </div>
          </div>
          <div className="space-y-2 px-5 pb-5">
            {breakdown.map((b, i) => (
              <div key={b.name} className="flex items-center gap-2 text-xs">
                <span
                  className={`size-2.5 rounded-sm ${["bg-chart-crit", "bg-chart-warn", "bg-chart-adv"][i]}`}
                />
                <span className="flex-1">{b.name}</span>
                <strong>{b.value}</strong>
                <span className="w-12 text-right text-muted-foreground">
                  {total ? ((b.value / total) * 100).toFixed(1) : "0.0"}%
                </span>
              </div>
            ))}
          </div>
        </Card>
      </div>
      <div className="mt-4">
        <Callout title="Review volume in context">
          Compare event counts alongside usable monitoring time. Lower counts alone do not establish
          a safer operation.
        </Callout>
      </div>
    </>
  );
}
export function EventOverview() {
  const demo = demoDataEnabled();
  const native = isNative();
  const allAlerts = useLiveAlerts();
  const now = Date.now();
  const liveCounts = countSeverity(todaysAlerts(allAlerts, now));
  const liveTotal = totalOf(liveCounts);
  const camStatus = useCameras();
  const realCams = useRealCameras(useStreamSources());
  const cams = summarizeCameras(
    camStatus,
    demo ? demoCameras.map((c) => c.id) : realCams.map((c) => c.id),
  );
  const weekCounts = realTrend(allAlerts, now).reduce<SevCounts>(
    (a, d) => ({ critical: a.critical + d.critical, warning: a.warning + d.warning, advisory: a.advisory + d.advisory }),
    { critical: 0, warning: 0, advisory: 0 },
  );
  const weekTotal = totalOf(weekCounts);
  const heat = demo ? heatmap : realHeatmap(allAlerts, now);
  const heatMax = Math.max(0, ...heat.flatMap((r) => r.values));
  const types = demo ? eventsByType : eventsByCondition(allAlerts, startOfDay(now) - 6 * 86_400_000);
  const typeMax = Math.max(1, ...types.map((t) => t.count));
  const typeTotal = types.reduce((a, t) => a + t.count, 0);
  const levels = [
    "bg-field text-muted-foreground",
    "bg-primary/10 text-primary",
    "bg-primary/20 text-primary",
    "bg-primary/40 text-foreground",
    "bg-primary/70 text-primary-foreground",
    "bg-primary text-primary-foreground",
  ];
  return (
    <>
      <PageHeader
        eyebrow="Review & insights / Analytics"
        title="Event overview"
        subtitle={demo ? "06 October 2026 · All demonstration rigs" : `${new Date(now).toLocaleDateString("en-GB", { day: "2-digit", month: "long", year: "numeric" })} · This rig · last 7 days`}
        actions={
          <Link to="/report/$reportId" params={{ reportId: "weekly" }}>
            <Btn>
              <Download className="size-4" />
              Export
            </Btn>
          </Link>
        }
      />
      <AnalyticsNav overview />
      {!demo && (
        <>
          {weekTotal === 0 && (
            <div className="mb-4">
              <Callout title="No data yet">
                {native
                  ? "No alerts have been raised in the last 7 days. This page fills in as the detection pipeline raises alerts."
                  : "Alert history is read from the RigMind desktop app. Open this page there to see it."}
              </Callout>
            </div>
          )}
          <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
            <Kpi
              label="Alerts · last 7 days"
              value={String(weekTotal)}
              note={`${weekCounts.critical} critical, ${weekCounts.warning} warning, ${weekCounts.advisory} advisory`}
            />
            <Kpi label="Median acknowledgement" value="—" note="Not recorded yet" />
            <Kpi label="P95 acknowledgement" value="—" note="Not recorded yet" />
            <Kpi
              label="Cameras online now"
              value={cams.total ? `${cams.online} of ${cams.total}` : "—"}
              note={cams.total ? (cams.availability !== null ? `${(cams.availability * 100).toFixed(1)}% available since app start` : "No stream reported yet") : "No camera configured"}
            />
          </div>
        </>
      )}
      {demo && <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi
          label="Verified events"
          value={String(42 + liveTotal)}
          note={
            liveTotal > 0 ? (
              <>
                Demo 42 + <LiveChip>{`Live ${liveTotal}`}</LiveChip> · {liveCounts.critical} critical,{" "}
                {liveCounts.warning} warning, {liveCounts.advisory} advisory
              </>
            ) : native ? (
              <>
                Demo 42 · <LiveChip>Live 0 today</LiveChip>
              </>
            ) : undefined
          }
        />
        <Kpi label="Median acknowledgement" value="8 s" />
        <Kpi label="P95 acknowledgement" value="24 s" note="38 acknowledged events" />
        <Kpi label="Cameras online now" value="95.8%" note="23 of 24" />
      </div>}
      {native && !demo && cams.total > 0 && (
        <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
          <Kpi
            label="Live stream health"
            value={`${cams.online} of ${cams.total}`}
            note={
              <>
                <LiveChip /> {cams.online ? `${cams.avgFps.toFixed(1)} fps avg · ${formatRate(cams.totalKbps)}` : "No stream online"}
                {" · "}
                {cams.reconnects} reconnects
                {cams.online > 0 && ` · up ${formatDuration(cams.longestOnlineSec)}`}
              </>
            }
            noteTone={cams.online === cams.total ? "success" : "critical"}
          />
        </div>
      )}
      {native && demo && (
        <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
          <Kpi
            label="Live cameras online"
            value={`${cams.online} of ${cams.total}`}
            note={
              <>
                <LiveChip /> {cams.online ? `${cams.avgFps.toFixed(1)} fps avg · ${formatRate(cams.totalKbps)}` : "No stream online"}
                {" · "}
                {cams.reconnects} reconnects
                {cams.availability !== null && ` · ${(cams.availability * 100).toFixed(1)}% available`}
                {cams.online > 0 && ` · up ${formatDuration(cams.longestOnlineSec)}`}
              </>
            }
            noteTone={cams.online === cams.total && cams.total > 0 ? "success" : "critical"}
          />
        </div>
      )}
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Activity heatmap" sub={demo ? "2-hour buckets · AST (UTC+3)" : "2-hour buckets · local time · last 7 days"} />
          <div className="overflow-x-auto p-4">
            <div className="grid min-w-[540px] grid-cols-[40px_repeat(12,1fr)] gap-1.5 text-center font-mono text-xs">
              <span />
              {Array.from({ length: 12 }, (_, i) => (
                <span key={i} className="py-2 text-muted-foreground">
                  {String(i * 2).padStart(2, "0")}
                </span>
              ))}
              {heat.map((row) => (
                <div className="contents" key={row.day}>
                  <span className="self-center text-muted-foreground">{row.day}</span>
                  {row.values.map((v, i) => (
                    <span
                      key={i}
                      title={`${row.day} ${i * 2}:00 ${demo ? "AST" : "local"} · ${v} events`}
                      className={`flex aspect-square items-center justify-center rounded-sm ${levels[demo ? v : heatLevel(v, heatMax)]}`}
                    >
                      {v}
                    </span>
                  ))}
                </div>
              ))}
            </div>
            <div className="mt-4 flex items-center justify-end gap-1 text-xs text-muted-foreground">
              <span className="mr-2">Less</span>
              {levels.map((l, i) => (
                <span key={i} className={`size-4 rounded-sm ${l}`} />
              ))}
              <span className="ml-2">More</span>
            </div>
          </div>
        </Card>
        <Card>
          <CardHead title="Events by type" sub={demo ? "42 verified events" : `${typeTotal} alerts · last 7 days`} />
          <div className="space-y-6 p-5">
            {!demo && types.length === 0 && <p className="text-sm text-muted-foreground">No data yet.</p>}
            {types.map((e) => (
              <div key={e.type}>
                <div className="mb-2 flex justify-between text-xs">
                  <span>{e.type}</span>
                  <strong className="font-mono">{e.count}</strong>
                </div>
                <progress className="h-2 w-full accent-primary" value={e.count} max={demo ? 18 : typeMax} />
              </div>
            ))}
          </div>
        </Card>
      </div>
    </>
  );
}
