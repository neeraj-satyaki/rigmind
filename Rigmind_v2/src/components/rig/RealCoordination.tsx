// Actions & handover and Users & access with sample data off. Everything shown here is typed in by the user and kept in
// the host's records, or read from the live alert and camera data. Nothing is pre-filled.
import { useState } from "react";
import { Link } from "@tanstack/react-router";
import { Download, Plus, Send, ClipboardCheck, FileText } from "lucide-react";
import { toast } from "sonner";
import {
  Avatar, Btn, Callout, Card, CardHead, Chip, Field, Kpi, Mono, PageHeader, Row, Select, TableWrap, Tabs, inputCls, td, th,
} from "./ui";
import { Check, Prompt, textareaCls } from "./workflow";
import { useCameras } from "@/lib/camera-status";
import { useLiveAlerts } from "@/lib/live-analytics";
import { localOperator, useRealCameras, useStreamSources } from "@/lib/real-state";
import {
  ACTIONS_KEY, HANDOVER_KEY, dayText, isEventRow, nextActionId, recordChange, stamp, timeText, useRecord,
  type ActionStatus, type CorrectiveAction, type HandoverSnapshot,
} from "@/lib/real-data";

const STATUSES: ActionStatus[] = ["Open", "In progress", "Closed"];
const statusTone = (s: ActionStatus) => (s === "Closed" ? "success" : s === "In progress" ? "advisory" : "warning");

export function ActionsHandoverReal() {
  const actions = useRecord<CorrectiveAction[]>(ACTIONS_KEY, []);
  const handover = useRecord<HandoverSnapshot | null>(HANDOVER_KEY, null);
  const alerts = useLiveAlerts();
  const camStatus = useCameras();
  const cams = useRealCameras(useStreamSources());
  const [tab, setTab] = useState("actions"),
    [notes, setNotes] = useState<Record<string, string>>({}),
    [dialog, setDialog] = useState(false),
    [form, setForm] = useState({ title: "", owner: "", approver: "", linkedEvent: "", due: "" }),
    [outgoing, setOutgoing] = useState(""),
    [receiving, setReceiving] = useState(""),
    [checks, setChecks] = useState([false, false, false]);

  const list = actions.value;
  const openAlerts = alerts.filter((r) => isEventRow(r) && (r.status === "Open" || r.status === "Escalated"));
  const gaps = cams.filter((c) => camStatus[c.id]?.state !== "online");
  const pending = list.filter((a) => a.status !== "Closed");
  const snap = handover.value;

  const create = () => {
    const a: CorrectiveAction = {
      id: nextActionId(list),
      title: form.title.trim(),
      owner: form.owner.trim(),
      approver: form.approver.trim(),
      linkedEvent: form.linkedEvent.trim(),
      due: form.due.trim(),
      status: "Open",
      createdAt: Date.now(),
      updates: [],
    };
    actions.save([...list, a]);
    recordChange("action.create", a.id, a.title);
    setForm({ title: "", owner: "", approver: "", linkedEvent: "", due: "" });
    setDialog(false);
    setTab("actions");
    toast.success(`${a.id} created`);
  };
  const patch = (id: string, change: Partial<CorrectiveAction>, detail: string) => {
    actions.save(list.map((a) => (a.id === id ? { ...a, ...change } : a)));
    recordChange("action.update", id, detail);
  };
  const post = (a: CorrectiveAction) => {
    const text = (notes[a.id] ?? "").trim();
    if (!text) return;
    patch(a.id, { updates: [...a.updates, { at: Date.now(), who: a.owner || localOperator.name, text }] }, "update posted");
    setNotes({ ...notes, [a.id]: "" });
    toast.success("Update posted");
  };

  const prepare = () => {
    const next: HandoverSnapshot = {
      preparedAt: Date.now(),
      outgoing: outgoing.trim(),
      receiving: receiving.trim(),
      accepted: false,
      acceptedAt: null,
      hazards: openAlerts.map((r) => ({ id: r.id, severity: r.severity, detail: `${r.condition}${r.place ? ` · ${r.place}` : ""}` })),
      gaps: gaps.map((c) => ({ cam: c.id, state: camStatus[c.id]?.state ?? "no report", since: camStatus[c.id] ? stamp(camStatus[c.id]!.changedAt) : "not reported" })),
      pending: pending.map((a) => ({ id: a.id, title: a.title, owner: a.owner, due: a.due })),
      checklist: ["Open hazards", "Monitoring gaps", "Pending actions"].map((text, i) => ({ text, done: checks[i] ?? false })),
    };
    handover.save(next);
    recordChange("handover.prepare", "handover", `${next.hazards.length} hazards, ${next.gaps.length} gaps, ${next.pending.length} actions`);
    toast.success("Handover prepared; awaiting receiving supervisor");
  };

  return (
    <>
      <PageHeader
        eyebrow="Review & insights / Coordination"
        title="Actions & handover"
        subtitle="Evidence-led follow-up and explicit shift acceptance."
        actions={
          <>
            <Btn onClick={() => setTab("handover")}>
              <ClipboardCheck className="size-4" />
              Shift handover
            </Btn>
            <Btn variant="primary" onClick={() => setDialog(true)}>
              <Plus className="size-4" />
              New action
            </Btn>
          </>
        }
      />
      <Tabs value={tab} onChange={setTab} tabs={[{ id: "actions", label: "Corrective actions" }, { id: "handover", label: "Shift handover" }]} />
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <div className="space-y-4">
          {tab === "actions" ? (
            <>
              {list.length === 0 && (
                <Card>
                  <CardHead title="No corrective actions yet" />
                  <p className="p-4 text-sm text-muted-foreground">
                    {actions.loaded ? "Create one with New action. Actions you create are saved on this device." : "Loading saved actions…"}
                  </p>
                </Card>
              )}
              {list.map((a) => (
                <Card key={a.id}>
                  <CardHead
                    title={`${a.id} · ${a.title}`}
                    sub={`Created ${stamp(a.createdAt)}`}
                    right={
                      <Link to="/report/$reportId" params={{ reportId: "action" }} search={{ actionId: a.id }}>
                        <Btn>
                          <Download className="size-4" />
                          Export action
                        </Btn>
                      </Link>
                    }
                  />
                  <div className="p-4">
                    <div className="mb-4 flex flex-wrap items-center gap-2">
                      <Chip tone={statusTone(a.status)}>{a.status}</Chip>
                      {a.linkedEvent && (
                        <Link to="/events/$eventId" params={{ eventId: a.linkedEvent }}>
                          <Chip>Linked {a.linkedEvent}</Chip>
                        </Link>
                      )}
                      {a.due && <Chip tone="warning">Due {a.due}</Chip>}
                      <Select className="ml-auto w-36" options={STATUSES} value={a.status} onChange={(v) => patch(a.id, { status: v as ActionStatus }, `status ${v}`)} />
                    </div>
                    <Row k="Assigned owner" v={a.owner || "Not assigned"} />
                    <Row k="Closure approver" v={a.approver || "Not assigned"} />
                    <h3 className="mb-3 mt-5 font-semibold">Progress updates</h3>
                    {a.updates.length === 0 && <p className="text-xs text-muted-foreground">No updates yet.</p>}
                    <div className="space-y-3">
                      {a.updates.map((u, i) => (
                        <div key={i} className="border-l-2 border-primary/30 pl-3">
                          <div className="flex items-center gap-2 text-xs">
                            <strong>{u.who}</strong>
                            <Mono className="text-muted-foreground">{stamp(u.at)}</Mono>
                          </div>
                          <p className="text-[13px]">{u.text}</p>
                        </div>
                      ))}
                    </div>
                    <div className="mt-5">
                      <Field label="Add an update">
                        <textarea className={textareaCls} value={notes[a.id] ?? ""} onChange={(e) => setNotes({ ...notes, [a.id]: e.target.value })} />
                      </Field>
                      <div className="mt-2 flex justify-end">
                        <Btn variant="primary" disabled={!(notes[a.id] ?? "").trim()} onClick={() => post(a)}>
                          <Send className="size-4" />
                          Post update
                        </Btn>
                      </div>
                    </div>
                  </div>
                </Card>
              ))}
            </>
          ) : (
            <Card>
              <CardHead title="Handover register" sub={`Now · ${dayText(Date.now())} · ${timeText(Date.now())}`} />
              <div className="p-4">
                <Row k="Open hazards" v={openAlerts.length ? `${openAlerts.length} open alerts · ${openAlerts.filter((r) => r.severity === "Critical").length} critical` : "None open"} />
                <Row k="Monitoring gaps" v={gaps.length ? gaps.map((c) => c.id).join(" · ") : cams.length ? "None" : "No camera configured"} />
                <Row k="Pending actions" v={pending.length ? pending.map((a) => a.id).join(" · ") : "None"} />
                <Row k="Receiving supervisor" v={snap ? `${snap.receiving || "Not named"} · ${snap.accepted ? "Accepted" : "Acceptance required"}` : "Not prepared yet"} />
                <Link to="/alerts">
                  <Btn className="mt-4">Review open alerts</Btn>
                </Link>
              </div>
            </Card>
          )}
        </div>
        <Card>
          <CardHead
            title="Shift handover"
            sub={snap ? `Prepared ${stamp(snap.preparedAt)}` : "Not prepared"}
            right={<Chip tone={snap?.accepted ? "success" : "warning"}>{snap?.accepted ? "Accepted" : snap ? "Prepared" : "Pending"}</Chip>}
          />
          <div className="space-y-3 p-4">
            <Field label="Outgoing supervisor">
              <input className={inputCls} value={outgoing} onChange={(e) => setOutgoing(e.target.value)} />
            </Field>
            <Field label="Receiving supervisor">
              <input className={inputCls} value={receiving} onChange={(e) => setReceiving(e.target.value)} />
            </Field>
            {["Open hazards", "Monitoring gaps", "Pending actions"].map((c, i) => (
              <Check key={c} checked={checks[i] ?? false} onChange={(v) => setChecks(checks.map((old, j) => (i === j ? v : old)))}>
                {c} reviewed
              </Check>
            ))}
            <p className="text-xs text-muted-foreground">Receiving supervisor — explicit acceptance required.</p>
            <Btn variant="primary" disabled={!checks.every(Boolean) || !outgoing.trim() || !receiving.trim()} onClick={prepare}>
              Prepare handover
            </Btn>
            {snap && (
              <Check
                checked={snap.accepted}
                onChange={(v) => {
                  handover.save({ ...snap, accepted: v, acceptedAt: v ? Date.now() : null });
                  recordChange("handover.accept", "handover", v ? snap.receiving : "withdrawn");
                  if (v) toast.success(`${snap.receiving || "Receiving supervisor"} accepted the handover`);
                }}
              >
                Accepted by {snap.receiving || "receiving supervisor"}
              </Check>
            )}
            {snap && (
              <Link to="/report/$reportId" params={{ reportId: "handover" }} className="inline-block">
                <Btn>
                  <FileText className="size-4" />
                  View handover report
                </Btn>
              </Link>
            )}
          </div>
        </Card>
      </div>
      <div className="mt-4">
        <Callout title="Closure requires review">The action owner submits evidence. The HSE reviewer accepts or reopens the action.</Callout>
      </div>
      <Prompt title="New corrective action" description="Saved on this device." open={dialog} onClose={() => setDialog(false)}>
        <div className="space-y-3">
          <Field label="Action title">
            <input className={inputCls} value={form.title} onChange={(e) => setForm({ ...form, title: e.target.value })} />
          </Field>
          <Field label="Assigned owner">
            <input className={inputCls} value={form.owner} onChange={(e) => setForm({ ...form, owner: e.target.value })} />
          </Field>
          <Field label="Closure approver">
            <input className={inputCls} value={form.approver} onChange={(e) => setForm({ ...form, approver: e.target.value })} />
          </Field>
          <Field label="Linked event ID (optional)">
            <input className={inputCls} value={form.linkedEvent} onChange={(e) => setForm({ ...form, linkedEvent: e.target.value })} />
          </Field>
          <Field label="Due (optional)">
            <input className={inputCls} value={form.due} onChange={(e) => setForm({ ...form, due: e.target.value })} placeholder="e.g. 07 Nov 2026 12:00" />
          </Field>
          <Btn variant="primary" disabled={!form.title.trim()} onClick={create}>
            Create action
          </Btn>
        </div>
      </Prompt>
    </>
  );
}

export function UsersAccessReal() {
  return (
    <>
      <PageHeader eyebrow="Governance / Access" title="Users & access" subtitle="Who can use this installation." />
      <div className="mb-4 grid gap-4 sm:grid-cols-3">
        <Kpi label="Accounts" value="01" note="Local operator on this device" />
        <Kpi label="Roles enforced" value="00" note="No role system yet" />
      </div>
      <div className="mb-4">
        <Callout title="Accounts are not managed yet">
          RigMind currently runs as one local operator on this computer. There is no sign-in, role assignment or per-rig scope to review, so no users are listed and invitations are not available.
        </Callout>
      </div>
      <Card>
        <TableWrap>
          <thead>
            <tr>
              {["User", "Role", "Assigned scope", "Access"].map((h) => (
                <th key={h} className={th}>
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            <tr>
              <td className={td}>
                <div className="flex items-center gap-2">
                  <Avatar initials={localOperator.initials} />
                  {localOperator.name}
                </div>
              </td>
              <td className={td}>Local operator</td>
              <td className={td}>This installation</td>
              <td className={td}>
                <Chip tone="success">Active</Chip>
              </td>
            </tr>
          </tbody>
        </TableWrap>
      </Card>
    </>
  );
}
