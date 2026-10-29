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
import { Btn, Callout, Card, CardHead, Kpi, PageHeader, Select } from "./ui";
import { NoteDialog } from "./workflow";
import { severityTrend, heatmap, eventsByType } from "@/data/mock";

function AnalyticsNav({ overview = false }: { overview?: boolean }) {
  return (
    <div className="mb-4 flex flex-wrap gap-2 border-b border-border pb-3">
      <Link to="/analytics">
        <Btn variant={overview ? "ghost" : "primary"}>Severity trends</Btn>
      </Link>
      <Link to="/analytics/overview">
        <Btn variant={overview ? "primary" : "ghost"}>Event overview</Btn>
      </Link>
      <NoteDialog
        title="Model quality"
        label="Model quality"
        text="Detection quality has not been measured. Site-approved validation is required before production expansion."
      />
    </div>
  );
}
const colors = ["var(--chart-crit)", "var(--chart-warn)", "var(--chart-adv)"];
export function SafetyAnalytics() {
  const [day, setDay] = useState("06 Oct");
  const selected = severityTrend.find((d) => d.day === day) ?? severityTrend[6];
  if (!selected) return null;
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
        subtitle="Verified events across usable monitoring time."
        actions={
          <>
            <Select
              className="w-36"
              options={severityTrend.map((d) => d.day)}
              value={day}
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
      <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi label={`Verified · ${day}`} value={String(total).padStart(2, "0")} />
        {breakdown.map((b, i) => (
          <Kpi
            key={b.name}
            label={b.name}
            value={String(b.value).padStart(2, "0")}
            note={`${((b.value / total) * 100).toFixed(1)}% of selected day`}
            {...(i === 0 ? { noteTone: "critical" as const } : {})}
          />
        ))}
      </div>
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Severity trends" sub="30 Sep–06 Oct 2026 · Verified events" />
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
          <CardHead title="Selected-day breakdown" sub={`${day} · ${total} verified events`} />
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
              <span className="text-xs text-muted-foreground">Verified</span>
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
                  {((b.value / total) * 100).toFixed(1)}%
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
        subtitle="06 October 2026 · All demonstration rigs"
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
      <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi label="Verified events" value="42" />
        <Kpi label="Median acknowledgement" value="8 s" />
        <Kpi label="P95 acknowledgement" value="24 s" note="38 acknowledged events" />
        <Kpi label="Cameras online now" value="95.8%" note="23 of 24" />
      </div>
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Activity heatmap" sub="2-hour buckets · AST (UTC+3)" />
          <div className="overflow-x-auto p-4">
            <div className="grid min-w-[540px] grid-cols-[40px_repeat(12,1fr)] gap-1.5 text-center font-mono text-xs">
              <span />
              {Array.from({ length: 12 }, (_, i) => (
                <span key={i} className="py-2 text-muted-foreground">
                  {String(i * 2).padStart(2, "0")}
                </span>
              ))}
              {heatmap.map((row) => (
                <div className="contents" key={row.day}>
                  <span className="self-center text-muted-foreground">{row.day}</span>
                  {row.values.map((v, i) => (
                    <span
                      key={i}
                      title={`${row.day} ${i * 2}:00 AST · ${v} events`}
                      className={`flex aspect-square items-center justify-center rounded-sm ${levels[v]}`}
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
          <CardHead title="Events by type" sub="42 verified events" />
          <div className="space-y-6 p-5">
            {eventsByType.map((e) => (
              <div key={e.type}>
                <div className="mb-2 flex justify-between text-xs">
                  <span>{e.type}</span>
                  <strong className="font-mono">{e.count}</strong>
                </div>
                <progress className="h-2 w-full accent-primary" value={e.count} max={18} />
              </div>
            ))}
          </div>
        </Card>
      </div>
    </>
  );
}
