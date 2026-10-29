import { useEffect, useState } from "react";
import { Link } from "@tanstack/react-router";
import {
  Download,
  RefreshCw,
  Activity,
  Play,
  Upload,
  Check as CheckIcon,
  Save,
} from "lucide-react";
import { toast } from "sonner";
import {
  Btn,
  Callout,
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
  inputCls,
  td,
  th,
} from "./ui";
import { Check, NoteDialog } from "./workflow";
import { useCameras } from "@/lib/camera-status";
import { isNative, nativeCall } from "@/lib/native-bridge";
import { cameraHealthRows, summarizeCameras } from "@/lib/live-analytics";
import { demoDataEnabled } from "@/lib/demo-mode";
import { AUDIT_ACTIONS, auditActionLabel, auditActor, loadAuditEntries } from "@/lib/real-data";
import { IntegrationsReal, LicenseCapacityReal, SafetyRulesReal, SystemHealthReal } from "./RealGovernance";
import {
  cameras as demoCameras,
  services,
  demoPolicy,
  licenseLifecycle,
  signalMappings,
  integrationStages,
  ruleDefaults,
  auditLog,
} from "@/data/mock";

export function SafetyRules() {
  return demoDataEnabled() ? <SafetyRulesDemo /> : <SafetyRulesReal />;
}

function SafetyRulesDemo() {
  const [tab, setTab] = useState("rules"),
    [form, setForm] = useState({ ...ruleDefaults }),
    [step, setStep] = useState(0),
    [saved, setSaved] = useState(false);
  const update = (key: keyof typeof form, value: string) => {
    setForm({ ...form, [key]: value });
    setSaved(false);
  };
  return (
    <>
      <PageHeader
        eyebrow="Governance / Configuration"
        title="Monitoring configuration"
        subtitle="Approved rules. Controlled changes."
        actions={
          <>
            <Btn
              onClick={() => {
                setForm({ ...ruleDefaults });
                setStep(0);
                setSaved(false);
                toast("Draft changes discarded");
              }}
            >
              Discard
            </Btn>
            <Btn
              variant="primary"
              disabled={step < 1 || step >= 2}
              onClick={() => {
                setStep(2);
                toast.success("Draft submitted for HSE / operations review");
              }}
            >
              Submit for review
            </Btn>
          </>
        }
      />
      <Tabs
        value={tab}
        onChange={setTab}
        tabs={[
          { id: "rules", label: "Safety rules" },
          { id: "notifications", label: "Notifications" },
          { id: "retention", label: "Evidence retention" },
          { id: "history", label: "Change history" },
        ]}
      />
      {tab === "rules" ? (
        <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
          <Card>
            <CardHead
              title="Occupied movement zone"
              sub="Context-aware rule · Critical operations"
              right={
                <NoteDialog
                  title="Version changes"
                  label="View changes"
                  text="Draft v4: confirmation and clearance windows await site approval. Current deployed version v3 remains unchanged."
                />
              }
            />
            <div className="space-y-4 p-4">
              <div className="flex flex-wrap gap-2">
                <Chip tone="advisory">Draft v{demoPolicy.draftRuleVersion}</Chip>
                <Chip>Current deployed version: v{demoPolicy.deployedRuleVersion}</Chip>
              </div>
              <div className="grid gap-4 sm:grid-cols-2">
                {(
                  [
                    ["id", "Rule ID"],
                    ["severity", "Severity"],
                    ["scope", "Camera scope"],
                    ["trigger", "Trigger condition"],
                    ["confirmation", "Confirmation window"],
                    ["clearance", "Clearance window"],
                    ["missing", "Missing-input behavior"],
                  ] as [keyof typeof form, string][]
                ).map(([key, label]) => (
                  <Field label={label} key={key}>
                    {key === "severity" ? (
                      <Select
                        value={form[key]}
                        onChange={(v) => update(key, v)}
                        options={["Critical", "Warning", "Advisory"]}
                      />
                    ) : (
                      <input
                        className={inputCls}
                        value={form[key]}
                        readOnly={key === "id"}
                        onChange={(e) => update(key, e.target.value)}
                      />
                    )}
                  </Field>
                ))}
              </div>
              <div className="flex flex-wrap gap-2">
                <Btn
                  onClick={() => {
                    setStep(1);
                    toast.success("Replay validation complete — simulation only");
                  }}
                >
                  <Play className="size-4" />
                  Validate on replay
                </Btn>
                <Btn
                  variant="primary"
                  onClick={() => {
                    setSaved(true);
                    toast.success("Draft saved; deployed version remains v3");
                  }}
                >
                  <Save className="size-4" />
                  {saved ? "Draft saved" : "Save draft"}
                </Btn>
              </div>
            </div>
          </Card>
          <Card>
            <CardHead title="Release workflow" />
            <div className="space-y-6 p-5">
              {[
                "Draft created",
                "Validate on replay",
                "HSE / operations review",
                "Controlled deployment",
              ].map((s, i) => (
                <div className="flex items-center gap-3" key={s}>
                  <span
                    className={`flex size-7 shrink-0 items-center justify-center rounded-full ${i < step || i === 0 ? "bg-success-bg text-success" : i === step ? "bg-nav-active text-primary" : "bg-field text-muted-foreground"}`}
                  >
                    {i < step || i === 0 ? <CheckIcon className="size-4" /> : i + 1}
                  </span>
                  <div>
                    <strong className="text-sm">{s}</strong>
                    {i === 2 && step === 2 && (
                      <p className="text-xs text-warning">Awaiting approval</p>
                    )}
                  </div>
                </div>
              ))}
            </div>
          </Card>
        </div>
      ) : tab === "history" ? (
        <Card>
          <CardHead title="Change history" />
          <div className="p-4">
            <Row k="09:50:31 · Faisal A." v="DZ-01 v4 · Draft saved" />
            <Row k="Current deployment" v="DZ-01 v3 · Approved" />
          </div>
        </Card>
      ) : tab === "retention" ? (
        <Card>
          <CardHead title="Evidence retention" />
          <div className="p-4">
            <Row k="Rolling footage" v={`${demoPolicy.rollingDays} days`} />
            <Row k="Event clips" v={`${demoPolicy.eventDays} days`} />
            <p className="mt-3 text-xs text-muted-foreground">
              Demonstration policy. Production retention requires customer approval.
            </p>
          </div>
        </Card>
      ) : (
        <Card>
          <CardHead title="Notification policy" />
          <div className="p-4">
            <Row k="Critical events" v="Operator + HSE desk" />
            <Row k="Monitoring gaps" v="Operator + maintenance" />
            <Row k="Physical warning outputs" v="Simulation only" />
          </div>
        </Card>
      )}
      <div className="mt-4">
        <Callout title="Changes are not live">
          Saving a draft does not change deployed detection behavior.
        </Callout>
      </div>
    </>
  );
}

export function SystemHealth() {
  return demoDataEnabled() ? <SystemHealthDemo /> : <SystemHealthReal />;
}

function SystemHealthDemo() {
  const liveCams = useCameras();
  const liveSummary = summarizeCameras(
    liveCams,
    demoCameras.map((c) => c.id),
  );
  return (
    <>
      <PageHeader
        eyebrow="Governance / Health"
        title="System health"
        subtitle="Monitoring coverage and service availability."
        actions={
          <>
            <Btn onClick={() => toast.success("Health snapshot refreshed; CAM-06 remains offline")}>
              <RefreshCw className="size-4" />
              Refresh
            </Btn>
            <Link to="/report/$reportId" params={{ reportId: "health" }}>
              <Btn>
                <Activity className="size-4" />
                Diagnostics
              </Btn>
            </Link>
          </>
        }
      />
      <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi label="Camera streams" value="23/24" note="One camera offline" noteTone="critical" />
        <Kpi label="Inference pipelines" value="23/24" note="1 blocked by source outage" />
        <Kpi label="Evidence storage" value="68%" note="No write failures in demo" />
        <Kpi
          label="Enterprise sync"
          value={<span className="font-sans text-xl">Connected</span>}
          note="0 events awaiting sync"
        />
      </div>
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Service status" />
          <TableWrap>
            <thead>
              <tr>
                {["Service", "Status", "Last seen"].map((h) => (
                  <th key={h} className={th}>
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {services.map((s) => (
                <tr key={s.name}>
                  <td className={td}>
                    <Mono>{s.name}</Mono>
                  </td>
                  <td className={td}>
                    <Chip tone={s.tone} dot>
                      {s.status}
                    </Chip>
                  </td>
                  <td className={td}>{s.seen}</td>
                </tr>
              ))}
            </tbody>
          </TableWrap>
        </Card>
        <Card>
          <CardHead title="Monitoring gap" right={<Chip tone="warning">Degraded coverage</Chip>} />
          <div className="p-4">
            <h2 className="font-semibold">CAM-06 is unavailable</h2>
            <p className="mb-3 text-xs text-muted-foreground">Demo rig 04 · Access corridor</p>
            <Row k="Last frame" v="4 minutes ago" />
            <Row k="Assigned to" v="Omar S. / Maintenance" />
            <Row k="Affected rules" v="Access-zone monitoring" />
            <div className="mt-4">
              <NoteDialog
                title="CAM-06 diagnostics"
                label="Open camera diagnostics"
                text="Source offline. Last frame: 4 minutes ago. Maintenance owner: Omar S. Monitoring is degraded; restore and validate the source before clearing the gap."
              />
            </div>
          </div>
        </Card>
      </div>
      {isNative() && (
        <Card className="mt-4">
          <CardHead
            title="Live camera streams"
            sub={`${liveSummary.online} of ${liveSummary.total} online · ${liveSummary.reconnects} reconnects`}
            right={
              <Chip tone="success" dot>
                Live
              </Chip>
            }
          />
          <TableWrap>
            <thead>
              <tr>
                {["Camera", "Status", "Detail", "Measured"].map((h) => (
                  <th key={h} className={th}>
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {cameraHealthRows(
                demoCameras.map((c) => c.id),
                liveCams,
              ).map((r) => (
                <tr key={r.name}>
                  <td className={td}>
                    <Mono>{r.name}</Mono>
                  </td>
                  <td className={td}>
                    <Chip tone={r.tone} dot>
                      {r.status}
                    </Chip>
                  </td>
                  <td className={td}>{r.seen}</td>
                  <td className={td}>
                    {liveCams[r.name]?.state === "online"
                      ? `${liveCams[r.name]?.fps.toFixed(1)} fps · ${Math.round(liveCams[r.name]?.kbps ?? 0)} kbps`
                      : "-"}
                  </td>
                </tr>
              ))}
            </tbody>
          </TableWrap>
        </Card>
      )}
      <div className="mt-4">
        <Callout title="Monitoring coverage is reduced" tone="warning">
          Unavailable cameras must remain visible in fleet summaries. A lost feed is not an
          all-clear condition.
        </Callout>
      </div>
    </>
  );
}

export function LicenseCapacity() {
  return demoDataEnabled() ? <LicenseCapacityDemo /> : <LicenseCapacityReal />;
}

function LicenseCapacityDemo() {
  const [file, setFile] = useState("");
  return (
    <>
      <PageHeader
        eyebrow="Governance / Entitlement"
        title="License & capacity"
        subtitle="Rig-local deployment and stream allocation."
        actions={
          <>
            <NoteDialog
              title="License history"
              label="License history"
              text="RM-DEMO-2026-001 · Demonstration entitlement · Active through 31 Dec 2026. No prior license imports in this session."
            />
            <label className="inline-flex">
              <input
                className="sr-only"
                type="file"
                accept=".json,.lic"
                aria-label="Import license package"
                onChange={(e) => {
                  const f = e.target.files?.[0];
                  if (f) {
                    setFile(f.name);
                    toast("Package selected for demonstration review; entitlement unchanged");
                  }
                }}
              />
              <span className="inline-flex h-8 cursor-pointer items-center gap-2 rounded-md border border-primary bg-primary px-3 text-[13px] font-medium text-primary-foreground">
                <Upload className="size-4" />
                Import license
              </span>
            </label>
          </>
        }
      />
      <Card>
        <CardHead
          title="License & deployment"
          sub="Demonstration entitlement"
          right={
            <Chip tone="success" dot>
              Active
            </Chip>
          }
        />
        <div className="grid gap-8 p-5 md:grid-cols-2">
          <div>
            <div className="eyebrow">RigMind Enterprise</div>
            <Row k="License ID" v={<Mono>{demoPolicy.licenseId}</Mono>} />
            <Row k="Valid through" v={demoPolicy.validThrough} />
            <Row k="Deployment mode" v="Rig-local / hybrid" />
            <Row k="License validation" v="Signed offline package" />
            <Row k="Renewal owner" v="Platform administrator" />
            {file && <p className="mt-3 text-xs text-warning">Selected: {file} · Not activated</p>}
          </div>
          <div>
            <h2 className="font-semibold">Stream allocation</h2>
            <div className="my-4 font-mono text-4xl">
              {demoPolicy.allocatedStreams}{" "}
              <span className="text-lg text-muted-foreground">/ {demoPolicy.streamCapacity}</span>
            </div>
            <progress
              value={demoPolicy.allocatedStreams}
              max={demoPolicy.streamCapacity}
              className="h-3 w-full accent-primary"
            />
            <div className="mt-2 flex justify-between text-xs text-muted-foreground">
              <span>75% allocated</span>
              <span>
                {demoPolicy.streamCapacity - demoPolicy.allocatedStreams} streams available
              </span>
            </div>
          </div>
        </div>
      </Card>
      <div className="mt-5">
        <h2 className="mb-3 font-semibold">License lifecycle</h2>
        <TableWrap>
          <thead>
            <tr>
              {["State", "Behavior", "Response"].map((h) => (
                <th key={h} className={th}>
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {licenseLifecycle.map((l) => (
              <tr key={l.state}>
                <td className={td}>{l.state}</td>
                <td className={td}>{l.behavior}</td>
                <td className={td}>{l.response}</td>
              </tr>
            ))}
          </tbody>
        </TableWrap>
      </div>
    </>
  );
}

export function Integrations() {
  return demoDataEnabled() ? <IntegrationsDemo /> : <IntegrationsReal />;
}

function IntegrationsDemo() {
  const [stage, setStage] = useState(-1),
    [running, setRunning] = useState(false);
  useEffect(() => {
    if (!running) return;
    const timer = setInterval(
      () =>
        setStage((s) => {
          if (s >= 3) {
            setRunning(false);
            toast.success("Simulated field feedback confirmed — no physical output");
            return s;
          }
          return s + 1;
        }),
      650,
    );
    return () => clearInterval(timer);
  }, [running]);
  return (
    <>
      <PageHeader
        eyebrow="Governance / Interfaces"
        title="SCADA & alarm integration"
        subtitle="Context signals, output requests and field feedback."
        actions={
          <>
            <NoteDialog
              title="Mapping history"
              label="Mapping history"
              text="DEMO-PLC-01 · Modbus TCP demonstration mapping. No production output mapping is enabled."
            />
            <Btn
              variant="primary"
              disabled={running}
              onClick={() => {
                setStage(-1);
                setRunning(true);
              }}
            >
              <Play className="size-4" />
              {running ? "Simulation running" : "Run simulation"}
            </Btn>
          </>
        }
      />
      <Callout title="Simulation mode is active">
        No physical outputs are enabled. Output requests and field feedback are recorded separately.
      </Callout>
      <div className="my-6 grid gap-3 md:grid-cols-4">
        {integrationStages.map((s, i) => (
          <div
            key={s.title}
            className={`border-b-2 px-3 py-4 ${stage >= i ? "border-success bg-success-bg" : "border-dashed border-border"}`}
          >
            <div className="mb-2 text-xs text-muted-foreground">
              0{i + 1} {stage >= i && "✓"}
            </div>
            <strong>{s.title}</strong>
            <p className="text-xs text-muted-foreground">{s.note}</p>
          </div>
        ))}
      </div>
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead
            title="Signal mapping"
            sub="Protocol Modbus TCP · Connection profile DEMO-PLC-01"
          />
          <TableWrap>
            <thead>
              <tr>
                {["Name", "Signal", "Direction"].map((h) => (
                  <th key={h} className={th}>
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {signalMappings.map((s) => (
                <tr key={s.signal}>
                  <td className={td}>{s.name}</td>
                  <td className={td}>
                    <Mono>{s.signal}</Mono>
                  </td>
                  <td className={td}>{s.dir}</td>
                </tr>
              ))}
            </tbody>
          </TableWrap>
        </Card>
        <Card>
          <CardHead title="Interface readiness" />
          <div className="p-4">
            <Row k="Connection" v={<Chip tone="success">Available</Chip>} />
            <Row k="Heartbeat" v="Fresh" />
            <Row k="Output mode" v="Simulation only" />
            <Row
              k="Field feedback"
              v={stage === 3 ? "Simulated confirmation" : "Awaiting simulation"}
            />
            <Row k="Machinery stop" v="Not enabled" />
          </div>
        </Card>
      </div>
      <div className="mt-4">
        <Callout title="Production activation">
          Requires controls, OEM, operations and HSE approval.
        </Callout>
      </div>
    </>
  );
}

type AuditRow = { time: string; actor: string; action: string; entity: string; result: string; real?: boolean };
type HostAuditRow = { id: number; tsMs: number; target: string; detail: string; action?: string; source?: string };

/** Real alarm transitions recorded by the host (audit_log, action alarm.transition). Empty outside the native host. */
function useAlarmTransitions(): AuditRow[] {
  const [rows, setRows] = useState<AuditRow[]>([]);
  const demo = demoDataEnabled();
  useEffect(() => {
    if (!isNative()) return;
    let stopped = false;
    // Sample off: every real audit action (alarm, alert response, camera, footage and changes made on the pages).
    const load = () =>
      (demo
        ? nativeCall<HostAuditRow[]>("audit.recent", { action: "alarm.transition", limit: 200 })
        : loadAuditEntries(AUDIT_ACTIONS, 200).then((e) => e.map((x) => ({ id: x.id, tsMs: x.tsMs, target: x.target, detail: x.detail, action: x.action, source: x.source })))
      )
        .then((r) => {
          if (stopped || !Array.isArray(r)) return;
          setRows(
            r.map((x) => ({
              time: new Date(x.tsMs).toLocaleString("en-GB", { day: "2-digit", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }) + ` #${x.id}`,
              actor: demo ? "Host (alarm)" : auditActor(x.source ?? ""),
              action: demo ? "Alarm transition" : auditActionLabel(x.action ?? "alarm.transition"),
              entity: x.target,
              result: x.detail,
              real: true,
            })),
          );
        })
        .catch(() => {});
    load();
    const t = window.setInterval(load, 5000);
    return () => {
      stopped = true;
      window.clearInterval(t);
    };
  }, [demo]);
  return rows;
}

export function AuditTrail() {
  const [actor, setActor] = useState("All actors"),
    [action, setAction] = useState("All actions"),
    [entity, setEntity] = useState("All entities");
  const real = useAlarmTransitions();
  // Recorded alarm transitions come first; the illustrative rows stay below them.
  const demo = demoDataEnabled();
  const all: AuditRow[] = demo ? [...real, ...auditLog] : real;
  const rows = all.filter(
    (r) =>
      (actor === "All actors" || r.actor === actor) &&
      (action === "All actions" || r.action === action) &&
      (entity === "All entities" || r.entity === entity),
  );
  return (
    <>
      <PageHeader
        eyebrow="Governance / Audit"
        title="Audit trail"
        subtitle="Every decision, change and export stays traceable."
        actions={
          <Link to="/report/$reportId" params={{ reportId: "audit" }}>
            <Btn>
              <Download className="size-4" />
              Export audit
            </Btn>
          </Link>
        }
      />
      <Callout title="Append-only activity record">
        Recorded actions retain their actor, timestamp, entity and outcome.
      </Callout>
      <div className="my-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
        <Field label="Entity">
          <Select
            value={entity}
            onChange={setEntity}
            options={["All entities", ...new Set(all.map((r) => r.entity))]}
          />
        </Field>
        <Field label="Date">
          <Select options={[demo ? "06 Oct 2026" : "All recorded dates"]} />
        </Field>
        <Field label="Actor">
          <Select
            value={actor}
            onChange={setActor}
            options={["All actors", ...new Set(all.map((r) => r.actor))]}
          />
        </Field>
        <Field label="Action">
          <Select
            value={action}
            onChange={setAction}
            options={["All actions", ...new Set(all.map((r) => r.action))]}
          />
        </Field>
      </div>
      <Card>
        <CardHead title="Activity log" sub={demo ? (real.length ? `${real.length} recorded alarm transitions, then illustrative records` : `${rows.length} illustrative records`) : `${real.length} recorded entries, newest first`} />
        <TableWrap>
          <thead>
            <tr>
              {["Time", "Actor", "Action", "Entity", "Result"].map((h) => (
                <th key={h} className={th}>
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={`${r.real ? "real" : "demo"}-${r.time}-${r.entity}`}>
                <td className={td}>
                  <Mono>{r.time}</Mono>
                </td>
                <td className={td}>{r.actor}</td>
                <td className={td}>
                  {r.action}
                  {r.real && <Chip tone="neutral">Recorded</Chip>}
                </td>
                <td className={td}>
                  <Mono>{r.entity}</Mono>
                </td>
                <td className={td}>
                  <Chip tone={r.result === "Pending approval" ? "warning" : "neutral"}>
                    {r.result}
                  </Chip>
                </td>
              </tr>
            ))}
            {!rows.length && (
              <tr>
                <td colSpan={5} className="p-8 text-center text-muted-foreground">
                  {demo || real.length ? "No activity matches these filters." : "No data yet. Audit entries appear here as alarms, alert responses and changes are recorded."}
                </td>
              </tr>
            )}
          </tbody>
        </TableWrap>
        <p className="p-4 text-xs text-muted-foreground">{demo ? "06 October 2026 · Asia/Riyadh" : "Times are in this computer's local time zone."}</p>
      </Card>
    </>
  );
}
