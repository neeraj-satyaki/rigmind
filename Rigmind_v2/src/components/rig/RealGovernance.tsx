// Governance pages with sample data off: health from the camera status store and host, rule drafts saved by the user,
// and honest "not configured" states where RigMind has no real source yet (license, SCADA).
import { useEffect, useState } from "react";
import { Link } from "@tanstack/react-router";
import { Activity, RefreshCw, Save } from "lucide-react";
import { toast } from "sonner";
import { Btn, Callout, Card, CardHead, Chip, Field, Kpi, Mono, PageHeader, Row, Select, TableWrap, Tabs, inputCls, td, th } from "./ui";
import { useCameras } from "@/lib/camera-status";
import { isNative, nativeCall } from "@/lib/native-bridge";
import { cameraHealthRows, summarizeCameras } from "@/lib/live-analytics";
import { localOperator, useRealCameras, useStreamSources } from "@/lib/real-state";
import { RULES_KEY, loadAuditEntries, recordChange, stamp, useRecord, type RuleDraft } from "@/lib/real-data";

type Footage = { id: number; camId: string; timestampMs: number; clipCount: number; expiresMs: number };

export function SystemHealthReal() {
  const status = useCameras();
  const cams = useRealCameras(useStreamSources());
  const ids = cams.map((c) => c.id);
  const sum = summarizeCameras(status, ids);
  const rows = cameraHealthRows(ids, status);
  const [footage, setFootage] = useState<Footage[] | null>(null);
  const load = () => {
    if (!isNative()) return;
    nativeCall<Footage[]>("footage.list", { limit: 500 })
      .then((r) => setFootage(Array.isArray(r) ? r : []))
      .catch(() => setFootage(null));
  };
  useEffect(load, []);
  const down = rows.filter((r) => r.status !== "Online");
  return (
    <>
      <PageHeader
        eyebrow="Governance / Health"
        title="System health"
        subtitle="Monitoring coverage and service availability."
        actions={
          <>
            <Btn
              onClick={() => {
                nativeCall("stream.status").catch(() => {});
                load();
                toast.success("Health snapshot requested");
              }}
            >
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
      {!isNative() && (
        <div className="mb-4">
          <Callout title="No data yet">Camera and service health is reported by the RigMind desktop app. Open this page there to see it.</Callout>
        </div>
      )}
      <div className="mb-4 grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi
          label="Camera streams"
          value={sum.total ? `${sum.online}/${sum.total}` : "—"}
          note={sum.total ? (sum.total - sum.online ? `${sum.total - sum.online} not online` : "All online") : "No camera configured"}
          {...(sum.total && sum.online < sum.total ? { noteTone: "critical" as const } : {})}
        />
        <Kpi label="Average frame rate" value={sum.online ? sum.avgFps.toFixed(1) : "—"} note={sum.online ? `${Math.round(sum.totalKbps)} kbps total` : "No stream online"} />
        <Kpi label="Reconnects" value={String(sum.reconnects)} note="Since the app started" />
        <Kpi
          label="Saved alarm footage"
          value={footage ? String(footage.length) : "—"}
          note={footage ? `${footage.reduce((a, f) => a + f.clipCount, 0)} clips` : isNative() ? "Not available" : "Desktop app only"}
        />
      </div>
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Camera streams" sub={sum.availability !== null ? `${(sum.availability * 100).toFixed(1)}% average availability since the app started` : undefined} />
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
              {rows.map((r) => (
                <tr key={r.name}>
                  <td className={td}>
                    <Mono>{r.name}</Mono>
                  </td>
                  <td className={td}>
                    <Chip tone={r.tone} dot>
                      {r.status}
                    </Chip>
                  </td>
                  <td className={td}>{status[r.name] ? r.seen : "No report from the host yet"}</td>
                  <td className={td}>{status[r.name]?.state === "online" ? `${status[r.name]?.fps.toFixed(1)} fps · ${Math.round(status[r.name]?.kbps ?? 0)} kbps` : "-"}</td>
                </tr>
              ))}
              {!rows.length && (
                <tr>
                  <td colSpan={4} className="p-8 text-center text-muted-foreground">
                    No data yet. No camera has been configured.
                  </td>
                </tr>
              )}
            </tbody>
          </TableWrap>
        </Card>
        <Card>
          <CardHead title="Monitoring gap" right={down.length ? <Chip tone="warning">Degraded coverage</Chip> : undefined} />
          <div className="p-4">
            {down.length === 0 ? (
              <p className="text-sm text-muted-foreground">{rows.length ? "No camera is currently unavailable." : "No camera configured."}</p>
            ) : (
              <>
                <h2 className="font-semibold">{down.map((d) => d.name).join(" · ")} unavailable</h2>
                {down.map((d) => (
                  <Row key={d.name} k={d.name} v={d.seen} />
                ))}
              </>
            )}
          </div>
        </Card>
      </div>
      <div className="mt-4">
        <Callout title="Monitoring coverage" tone="warning">
          Unavailable cameras must remain visible in fleet summaries. A lost feed is not an all-clear condition. Evidence storage and enterprise sync are not measured by this version.
        </Callout>
      </div>
    </>
  );
}

const blankRule = { id: "", severity: "Critical", scope: "", trigger: "", confirmation: "", clearance: "", missing: "" };

export function SafetyRulesReal() {
  const rules = useRecord<RuleDraft[]>(RULES_KEY, []);
  const [tab, setTab] = useState("rules"),
    [form, setForm] = useState({ ...blankRule }),
    [history, setHistory] = useState<{ time: string; text: string }[]>([]);
  useEffect(() => {
    if (tab !== "history") return;
    loadAuditEntries(["rule.draft.save"]).then((r) => setHistory(r.map((x) => ({ time: stamp(x.tsMs), text: x.target })))).catch(() => {});
  }, [tab, rules.value]);
  const set = (k: keyof typeof form, v: string) => setForm({ ...form, [k]: v });
  const save = () => {
    const draft: RuleDraft = { ...form, id: form.id.trim(), savedAt: Date.now() };
    rules.save([...rules.value.filter((r) => r.id !== draft.id), draft]);
    recordChange("rule.draft.save", draft.id, draft.severity);
    toast.success("Draft saved; deployed detection behavior is unchanged");
  };
  return (
    <>
      <PageHeader eyebrow="Governance / Configuration" title="Monitoring configuration" subtitle="Rule drafts and policy. Controlled changes." />
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
            <CardHead title="New rule draft" sub="Saved on this device. A draft does not change what the detector does." />
            <div className="space-y-4 p-4">
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
                      <Select value={form[key]} onChange={(v) => set(key, v)} options={["Critical", "Warning", "Advisory"]} />
                    ) : (
                      <input className={inputCls} value={form[key]} onChange={(e) => set(key, e.target.value)} />
                    )}
                  </Field>
                ))}
              </div>
              <Btn variant="primary" disabled={!form.id.trim()} onClick={save}>
                <Save className="size-4" />
                Save draft
              </Btn>
            </div>
          </Card>
          <Card>
            <CardHead title="Saved drafts" />
            <div className="p-4">
              {rules.value.length === 0 && <p className="text-sm text-muted-foreground">No rule drafts yet.</p>}
              {rules.value.map((r) => (
                <Row key={r.id} k={<Mono>{r.id}</Mono>} v={<span>{r.severity} · {stamp(r.savedAt)}</span>} />
              ))}
            </div>
          </Card>
        </div>
      ) : tab === "history" ? (
        <Card>
          <CardHead title="Change history" />
          <div className="p-4">
            {history.length === 0 && <p className="text-sm text-muted-foreground">No rule changes recorded yet.</p>}
            {history.map((h, i) => (
              <Row key={i} k={h.time} v={`${h.text} · Draft saved`} />
            ))}
          </div>
        </Card>
      ) : tab === "retention" ? (
        <Card>
          <CardHead title="Evidence retention" />
          <div className="p-4">
            <Row k="Alarm footage" v="7 days, then deleted (built into the host)" />
            <p className="mt-3 text-xs text-muted-foreground">Other retention periods are not configurable in this version.</p>
          </div>
        </Card>
      ) : (
        <Card>
          <CardHead title="Notification policy" />
          <div className="p-4">
            <Row k="Raised alerts" v="Toast, bell and push notification in this app" />
            <Row k="Physical warning outputs" v="Not connected" />
          </div>
        </Card>
      )}
      <div className="mt-4">
        <Callout title="Changes are not live">Saving a draft does not change deployed detection behavior.</Callout>
      </div>
    </>
  );
}

export function LicenseCapacityReal() {
  const cams = useRealCameras(useStreamSources());
  return (
    <>
      <PageHeader eyebrow="Governance / Entitlement" title="License & capacity" subtitle="Rig-local deployment and stream allocation." />
      <Card>
        <CardHead title="License & deployment" sub="No license package imported" right={<Chip tone="neutral">Not licensed</Chip>} />
        <div className="grid gap-8 p-5 md:grid-cols-2">
          <div>
            <div className="eyebrow">RigMind</div>
            <Row k="License ID" v="—" />
            <Row k="Valid through" v="—" />
            <Row k="Deployment mode" v="Rig-local" />
            <Row k="Operator" v={localOperator.name} />
            <p className="mt-3 text-xs text-muted-foreground">License import and validation are not available in this version.</p>
          </div>
          <div>
            <h2 className="font-semibold">Stream allocation</h2>
            <div className="my-4 font-mono text-4xl">{cams.length}</div>
            <p className="text-xs text-muted-foreground">cameras configured on this installation. Capacity limit: not set.</p>
          </div>
        </div>
      </Card>
    </>
  );
}

export function IntegrationsReal() {
  return (
    <>
      <PageHeader eyebrow="Governance / Interfaces" title="SCADA & alarm integration" subtitle="Context signals, output requests and field feedback." />
      <Callout title="No interface configured">
        No SCADA, PLC or alarm output is connected. Physical outputs and machinery stop are not enabled.
      </Callout>
      <div className="mt-4 grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Signal mapping" />
          <p className="p-4 text-sm text-muted-foreground">No data yet. No signals are mapped.</p>
        </Card>
        <Card>
          <CardHead title="Interface readiness" />
          <div className="p-4">
            <Row k="Connection" v={<Chip tone="neutral">Not configured</Chip>} />
            <Row k="Output mode" v="Disabled" />
            <Row k="Machinery stop" v="Not enabled" />
          </div>
        </Card>
      </div>
    </>
  );
}
