import { useEffect, useMemo, useState } from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import { Search, SlidersHorizontal } from "lucide-react";
import {
  Btn,
  Card,
  Chip,
  Mono,
  PageHeader,
  Select,
  TableWrap,
  Tabs,
  inputCls,
  pageHead,
  td,
  th,
} from "@/components/rig/ui";
import { alerts, severityTone, type Severity } from "@/data/mock";
import { useAppState } from "@/components/rig/AppState";
import { NoteDialog } from "@/components/rig/workflow";
import { cn } from "@/lib/utils";
import { demoOr } from "@/lib/demo-mode";
import { isNative } from "@/lib/native-bridge";
import { describeAlert, loadPlainSummaries, type PlainSummary } from "@/lib/plain-summary";
import { useRealCameras, useStreamSources } from "@/lib/real-state";

export const Route = createFileRoute("/_shell/alerts")({
  head: pageHead("Alert inbox", "Prioritized safety events across your assigned rigs."),
  component: Alerts,
});

const summary: { sev: Severity; count: string; note: string }[] = [
  { sev: "Critical", count: "02", note: "Immediate attention" },
  { sev: "Warning", count: "03", note: "Review and respond" },
  { sev: "Advisory", count: "02", note: "Review observations" },
];

function Alerts() {
  const { items, respond } = useAppState();
  const cams = useRealCameras(useStreamSources(false, isNative()));
  // Plain-language summaries (BitNet or template) stored on this device, by alert id.
  const [plain, setPlain] = useState<Record<string, PlainSummary>>({});
  const [busy, setBusy] = useState<string>("");
  const refreshPlain = () =>
    loadPlainSummaries()
      .then((list) => setPlain(Object.fromEntries(list.map((s) => [s.alertId, s]))))
      .catch(() => {});
  useEffect(() => {
    refreshPlain();
    const timer = setInterval(refreshPlain, 5000); // automatic summaries arrive while this page is open
    return () => clearInterval(timer);
  }, []);
  const summarise = async (a: (typeof items)[number]) => {
    setBusy(a.id);
    const place = [a.rig, a.area].filter(Boolean).join(" · ");
    await describeAlert(
      { id: a.id, severity: a.severity, condition: a.condition, place },
      cams[0]?.id ?? "",
      place,
      () => {},
    ).catch(() => {});
    await refreshPlain();
    setBusy("");
  };
  const [tab, setTab] = useState("open");
  const [sev, setSev] = useState("All severities");
  const [rig, setRig] = useState("All rigs");
  const [q, setQ] = useState("");
  // Rig names: the sample fleet, or the rigs that actual alerts came from.
  const rigOptions = useMemo(
    () => demoOr(["All rigs", "Demo rig 01", "Demo rig 02", "Demo rig 03", "Demo rig 04"], ["All rigs", ...new Set(items.map((a) => a.rig).filter(Boolean))]),
    [items],
  );
  const rows = useMemo(
    () =>
      items.filter(
        (a) =>
          (tab === "all" || tab === "open"
            ? a.status === "Open" || tab === "all"
            : a.status.toLowerCase() === tab) &&
          (sev === "All severities" || a.severity === sev) &&
          (rig === "All rigs" || a.rig === rig) &&
          (q === "" ||
            `${a.id} ${a.condition} ${a.area} ${a.owner}`.toLowerCase().includes(q.toLowerCase())),
      ),
    [items, tab, sev, rig, q],
  );
  return (
    <>
      <PageHeader
        eyebrow="Workspace / Alerts"
        title="Alert inbox"
        subtitle="Prioritized safety events across your assigned rigs."
        actions={
          <NoteDialog
            title="Escalation rules"
            label="Escalation rules"
            icon={<SlidersHorizontal className="size-4" />}
            text="Critical events: operator and HSE desk. Monitoring gaps: operator and maintenance. Escalation records the response; it does not clear the hazard."
          />
        }
      />
      <div className="mb-4 grid gap-4 sm:grid-cols-3">
        {summary.map((s) => (
          <button
            key={s.sev}
            onClick={() => setSev(sev === s.sev ? "All severities" : s.sev)}
            className={cn(
              "rounded-lg border bg-card p-4 text-left transition",
              sev === s.sev
                ? "border-primary ring-2 ring-primary/15"
                : "border-border hover:border-primary/40",
            )}
          >
            <Chip tone={severityTone(s.sev)}>{s.sev}</Chip>
            <div className="mt-2 font-mono text-[28px] font-semibold leading-none">
              {String(
                items.filter((a) => a.severity === s.sev && a.status === "Open").length,
              ).padStart(2, "0")}
            </div>
            <div className="mt-1 text-xs text-muted-foreground">{s.note}</div>
          </button>
        ))}
      </div>
      <Card>
        <div className="px-4 pt-2">
          <Tabs
            value={tab}
            onChange={setTab}
            tabs={[
              {
                id: "open",
                label: (
                  <>
                    Open alerts{" "}
                    <span className="ml-1 rounded-full bg-critical-bg px-1.5 text-critical">
                      {items.filter((a) => a.status === "Open").length}
                    </span>
                  </>
                ),
              },
              { id: "monitoring", label: "Monitoring" },
              { id: "acknowledged", label: "Acknowledged" },
              { id: "escalated", label: "Escalated" },
              { id: "all", label: "All alerts" },
            ]}
          />
        </div>
        <div className="flex flex-wrap gap-2 px-4 pb-3">
          <div className="relative min-w-[200px] flex-1">
            <Search className="absolute left-2.5 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground" />
            <input
              value={q}
              onChange={(e) => setQ(e.target.value)}
              placeholder="Search alerts"
              className={cn(inputCls, "pl-8")}
            />
          </div>
          <Select
            className="w-44"
            value={sev}
            onChange={setSev}
            options={["All severities", "Critical", "Warning", "Advisory"]}
          />
          <Select
            className="w-40"
            value={rig}
            onChange={setRig}
            options={rigOptions}
          />
        </div>
        <TableWrap>
          <thead>
            <tr>
              {["Event", "Severity", "Detected condition", "Age", "Owner", ""].map((h) => (
                <th key={h} className={th}>
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((a) => (
              <tr key={a.id} className="hover:bg-field">
                <td className={td}>
                  <Mono className="font-semibold">{a.id}</Mono>
                  <div className="text-xs text-muted-foreground">{a.rig}</div>
                  {a.status !== "Open" && (
                    <Chip tone={a.status === "Escalated" ? "warning" : a.status === "Acknowledged" ? "success" : "neutral"}>
                      {a.status}
                    </Chip>
                  )}
                </td>
                <td className={td}>
                  <Chip tone={severityTone(a.severity)}>{a.severity}</Chip>
                </td>
                <td className={td}>
                  <div className="font-medium">{a.condition}</div>
                  <div className="text-xs text-muted-foreground">{a.area}</div>
                  {plain[a.id] && (
                    <div className="mt-1 max-w-md text-xs">
                      {plain[a.id]!.text}{" "}
                      <span className="text-muted-foreground">({plain[a.id]!.source === "slm" ? "BitNet" : "Template"})</span>
                    </div>
                  )}
                </td>
                <td className={td}>
                  <Mono>{a.age}</Mono>
                </td>
                <td className={cn(td, a.owner === "Unassigned" && "text-muted-foreground")}>
                  {a.owner}
                </td>
                <td className={cn(td, "text-right")}>
                  <div className="flex justify-end gap-1">
                    {demoOr(a.id === "EV-1042", false) ? (
                      <Link to="/live">
                        <Btn>Review</Btn>
                      </Link>
                    ) : demoOr(true, false) ? (
                      <Link to="/events">
                        <Btn>Review</Btn>
                      </Link>
                    ) : (
                      <Link to="/events/$eventId" params={{ eventId: a.id }}>
                        <Btn>Review</Btn>
                      </Link>
                    )}
                    {isNative() && (
                      <Btn onClick={() => summarise(a)}>{busy === a.id ? "Writing…" : "Plain words"}</Btn>
                    )}
                    {a.status !== "Acknowledged" && (
                      <Btn onClick={() => respond(a.id, "Acknowledged")}>Acknowledge</Btn>
                    )}
                    {a.status !== "Escalated" && a.status !== "Monitoring" && a.status !== "Cleared" && (
                      <Btn onClick={() => respond(a.id, "Escalated")}>Escalate</Btn>
                    )}
                  </div>
                </td>
              </tr>
            ))}
            {rows.length === 0 && (
              <tr>
                <td colSpan={6} className="p-8 text-center text-muted-foreground">
                  {items.length === 0 ? "No alerts yet. Alerts raised by the monitoring service appear here." : "No alerts match these filters."}
                </td>
              </tr>
            )}
          </tbody>
        </TableWrap>
        <div className="px-4 py-3 text-xs text-muted-foreground">
          Showing {rows.length} {tab === "all" ? "" : tab} alerts · Ordered by severity, then age
        </div>
      </Card>
    </>
  );
}
