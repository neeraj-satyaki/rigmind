import { useEffect, useRef, useState } from "react";
import { Link, useNavigate } from "@tanstack/react-router";
import { Download, Plus, Paperclip, Send, Search, ClipboardCheck, FileText } from "lucide-react";
import { toast } from "sonner";
import {
  Avatar,
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
import { Check, NoteDialog, Prompt, textareaCls } from "./workflow";
import {
  actionUpdates,
  currentUser,
  weeklyReportMeta,
  users,
  roles,
  reportTypes,
  reportSections,
  pilotReadiness,
} from "@/data/mock";
import { loadApprovedSummaries } from "@/lib/event-review-store";
import { demoDataEnabled, demoOr } from "@/lib/demo-mode";
import { ActionsHandoverReal, UsersAccessReal } from "./RealCoordination";
import { useCameras } from "@/lib/camera-status";
import { useRealCameras, useStreamSources } from "@/lib/real-state";

export function ActionsHandover() {
  return demoDataEnabled() ? <ActionsHandoverDemo /> : <ActionsHandoverReal />;
}

function ActionsHandoverDemo() {
  const [tab, setTab] = useState("actions"),
    [updates, setUpdates] = useState(actionUpdates),
    [note, setNote] = useState(""),
    [attachment, setAttachment] = useState(""),
    [prepared, setPrepared] = useState(false),
    [accepted, setAccepted] = useState(false),
    [checks, setChecks] = useState([false, false, false]),
    [dialog, setDialog] = useState(false),
    [newTitle, setNewTitle] = useState(""),
    [extra, setExtra] = useState<string[]>([]);
  const file = useRef<HTMLInputElement>(null);
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
      <Tabs
        value={tab}
        onChange={setTab}
        tabs={[
          { id: "actions", label: "Corrective actions" },
          { id: "handover", label: "Shift handover" },
        ]}
      />
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <div className="space-y-4">
          {tab === "actions" ? (
            <>
              <Card>
                <CardHead
                  title="CA-028 · Review derrick-zone exposure"
                  sub="Created by Maha S. · HSE"
                  right={
                    <Link
                      to="/report/$reportId"
                      params={{ reportId: "action" }}
                      search={{ actionId: "CA-028" }}
                    >
                      <Btn>
                        <Download className="size-4" />
                        Export action
                      </Btn>
                    </Link>
                  }
                />
                <div className="p-4">
                  <div className="mb-4 flex flex-wrap gap-2">
                    <Chip tone="advisory">In progress</Chip>
                    <Link to="/events/$eventId" params={{ eventId: "EV-1042" }}>
                      <Chip>Linked EV-1042</Chip>
                    </Link>
                    <Chip tone="warning">Due 07 Oct, 12:00 AST</Chip>
                  </div>
                  <Row k="Action" v="Review crew positioning procedure" />
                  <Row k="Assigned owner" v="Faisal A. / Shift supervisor" />
                  <Row k="Closure approver" v="Maha S. / HSE" />
                  <h3 className="mb-3 mt-5 font-semibold">Progress updates</h3>
                  <div className="space-y-3">
                    {updates.map((u, i) => (
                      <div key={i} className="border-l-2 border-primary/30 pl-3">
                        <div className="flex items-center gap-2 text-xs">
                          <strong>{u.who}</strong>
                          <Mono className="text-muted-foreground">{u.time}</Mono>
                        </div>
                        <p className="text-[13px]">{u.text}</p>
                      </div>
                    ))}
                  </div>
                  <div className="mt-5">
                    <Field label="Add an update">
                      <textarea
                        className={textareaCls}
                        value={note}
                        onChange={(e) => setNote(e.target.value)}
                      />
                    </Field>
                    <div className="mt-2 flex flex-wrap items-center gap-2">
                      <input
                        ref={file}
                        type="file"
                        className="hidden"
                        onChange={(e) => setAttachment(e.target.files?.[0]?.name ?? "")}
                      />
                      <Btn onClick={() => file.current?.click()}>
                        <Paperclip className="size-4" />
                        Attach evidence
                      </Btn>
                      <span className="flex-1 truncate text-xs text-muted-foreground">
                        {attachment}
                      </span>
                      <Btn
                        variant="primary"
                        disabled={!note.trim()}
                        onClick={() => {
                          setUpdates([
                            ...updates,
                            {
                              time: new Date().toLocaleTimeString("en-GB", {
                                hour: "2-digit",
                                minute: "2-digit",
                                timeZone: "Asia/Riyadh",
                              }),
                              who: currentUser.name,
                              text: note.trim() + (attachment ? ` · Evidence: ${attachment}` : ""),
                            },
                          ]);
                          setNote("");
                          setAttachment("");
                          toast.success("Update posted");
                        }}
                      >
                        <Send className="size-4" />
                        Post update
                      </Btn>
                    </div>
                  </div>
                </div>
              </Card>
              {extra.map((title, i) => (
                <Card key={i}>
                  <CardHead
                    title={`CA-${String(29 + i).padStart(3, "0")} · ${title}`}
                    sub="Created by Faisal A. · Demonstration session"
                    right={<Chip>Draft</Chip>}
                  />
                </Card>
              ))}
            </>
          ) : (
            <Card>
              <CardHead title="Day → Night handover register" sub="06 Oct · 18:00 AST" />
              <div className="p-4">
                <Row k="Open hazards" v="7 alerts · 2 critical" />
                <Row k="Monitoring gaps" v="CAM-06 · Demo rig 04" />
                <Row k="Pending actions" v="CA-028 · Crew positioning" />
                <Row
                  k="Receiving supervisor"
                  v={accepted ? "Nasser H. · Accepted" : "Nasser H. · Acceptance required"}
                />
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
            sub="Day → Night · 06 Oct · 18:00 AST"
            right={
              <Chip tone={accepted ? "success" : "warning"}>
                {accepted ? "Accepted" : prepared ? "Prepared" : "Pending"}
              </Chip>
            }
          />
          <div className="p-4">
            {["Open hazards", "Monitoring gaps", "Pending actions"].map((c, i) => (
              <Check
                key={c}
                checked={checks[i] ?? false}
                onChange={(v) => setChecks(checks.map((old, j) => (i === j ? v : old)))}
              >
                {c}
              </Check>
            ))}
            <p className="my-3 text-xs text-muted-foreground">
              Receiving supervisor — explicit acceptance required.
            </p>
            <Btn
              variant="primary"
              disabled={!checks.every(Boolean) || prepared}
              onClick={() => {
                setPrepared(true);
                toast.success("Handover prepared; awaiting receiving supervisor");
              }}
            >
              Prepare handover
            </Btn>
            {prepared && (
              <Check
                checked={accepted}
                onChange={(v) => {
                  setAccepted(v);
                  if (v) toast.success("Nasser H. accepted the demonstration handover");
                }}
              >
                Accepted by Nasser H. (demo)
              </Check>
            )}
            {prepared && (
              <Link
                to="/report/$reportId"
                params={{ reportId: "handover" }}
                className="mt-3 inline-block"
              >
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
        <Callout title="Closure requires review">
          The action owner submits evidence. The HSE reviewer accepts or reopens the action.
        </Callout>
      </div>
      <Prompt
        title="New corrective action"
        description="Create a draft in this demonstration session."
        open={dialog}
        onClose={() => setDialog(false)}
      >
        <Field label="Action title">
          <input
            className={inputCls}
            value={newTitle}
            onChange={(e) => setNewTitle(e.target.value)}
          />
        </Field>
        <Btn
          variant="primary"
          disabled={!newTitle.trim()}
          onClick={() => {
            setExtra([...extra, newTitle.trim()]);
            setNewTitle("");
            setDialog(false);
            setTab("actions");
          }}
        >
          Create draft
        </Btn>
      </Prompt>
    </>
  );
}

export function UsersAccess() {
  return demoDataEnabled() ? <UsersAccessDemo /> : <UsersAccessReal />;
}

function UsersAccessDemo() {
  const [tab, setTab] = useState("users"),
    [query, setQuery] = useState(""),
    [invite, setInvite] = useState(false),
    [email, setEmail] = useState(""),
    [sent, setSent] = useState<string[]>([]),
    [reviewed, setReviewed] = useState(false);
  const filtered = users.filter((u) =>
    `${u.name} ${u.role} ${u.scope}`.toLowerCase().includes(query.toLowerCase()),
  );
  return (
    <>
      <PageHeader
        eyebrow="Governance / Access"
        title="Users & access"
        subtitle="The right people. The right scope."
        actions={
          <>
            <Btn onClick={() => setTab("reviews")}>Review access</Btn>
            <Btn variant="primary" onClick={() => setInvite(true)}>
              <Plus className="size-4" />
              Invite user
            </Btn>
          </>
        }
      />
      <div className="mb-4 grid gap-4 sm:grid-cols-3">
        <Kpi label="Named accounts" value="05" />
        <Kpi label="Operational roles" value="05" />
        <Kpi
          label="Access boundary"
          value={<span className="font-sans text-lg">Scope enforced per rig</span>}
        />
      </div>
      <Tabs
        value={tab}
        onChange={setTab}
        tabs={[
          { id: "users", label: "Users" },
          { id: "roles", label: "Roles" },
          { id: "reviews", label: "Access reviews" },
        ]}
      />
      {tab === "users" ? (
        <Card>
          <div className="relative m-4 max-w-sm">
            <Search className="absolute left-3 top-3 size-3.5 text-muted-foreground" />
            <input
              className={`${inputCls} pl-8`}
              placeholder="Search users"
              aria-label="Search users"
              value={query}
              onChange={(e) => setQuery(e.target.value)}
            />
          </div>
          <TableWrap>
            <thead>
              <tr>
                {["User", "Role", "Assigned scope", "Access", "Last review"].map((h) => (
                  <th key={h} className={th}>
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {filtered.map((u) => (
                <tr key={u.name}>
                  <td className={td}>
                    <div className="flex items-center gap-2">
                      <Avatar initials={u.initials} />
                      {u.name}
                    </div>
                  </td>
                  <td className={td}>{u.role}</td>
                  <td className={td}>{u.scope}</td>
                  <td className={td}>
                    <Chip tone={u.access === "Active" ? "success" : "neutral"}>{u.access}</Chip>
                  </td>
                  <td className={td}>01 Oct 2026</td>
                </tr>
              ))}
              {!filtered.length && (
                <tr>
                  <td colSpan={5} className="p-8 text-center text-muted-foreground">
                    No users match this search.
                  </td>
                </tr>
              )}
            </tbody>
          </TableWrap>
        </Card>
      ) : tab === "roles" ? (
        <div className="grid gap-4 md:grid-cols-3">
          {roles.map((r) => (
            <Card key={r.title} className="p-5">
              <h2 className="font-semibold">{r.title}</h2>
              <p className="mt-2 text-sm text-muted-foreground">{r.note}</p>
            </Card>
          ))}
        </div>
      ) : (
        <Card>
          <CardHead
            title="Access review"
            sub="5 named accounts · Last review 01 Oct 2026"
            right={
              <Chip tone={reviewed ? "success" : "warning"}>
                {reviewed ? "Reviewed" : "Review due"}
              </Chip>
            }
          />
          <div className="p-4">
            <p className="mb-4 text-sm">
              Assigned rig scope and read-only auditor access remain unchanged.
            </p>
            <Btn
              disabled={reviewed}
              onClick={() => {
                setReviewed(true);
                toast.success("Demonstration access review recorded");
              }}
            >
              Record review
            </Btn>
            {sent.map((e) => (
              <Row key={e} k="Draft invitation" v={e} />
            ))}
          </div>
        </Card>
      )}
      <Prompt
        title="Invite user"
        description="Creates a demonstration invitation only; no email will be sent."
        open={invite}
        onClose={() => setInvite(false)}
      >
        <form
          className="space-y-4"
          onSubmit={(e) => {
            e.preventDefault();
            setSent([...sent, email]);
            setInvite(false);
            setEmail("");
            toast.success("Demonstration invitation drafted; no email sent");
          }}
        >
          <Field label="Work email">
            <input
              className={inputCls}
              type="email"
              required
              value={email}
              onChange={(e) => setEmail(e.target.value)}
            />
          </Field>
          <Field label="Role">
            <Select
              options={["Operator", "HSE reviewer", "Maintenance", "Rig manager", "Auditor"]}
            />
          </Field>
          <Field label="Assigned scope">
            <Select
              options={[
                "Demo rig 01",
                "Demo rig 02",
                "Demo rig 03",
                "Demo rig 04",
                "Approved evidence",
              ]}
            />
          </Field>
          <Btn type="submit" variant="primary">
            Create invitation draft
          </Btn>
        </form>
      </Prompt>
    </>
  );
}

export function Reports() {
  const nav = useNavigate();
  const [type, setType] = useState("weekly"),
    [sections, setSections] = useState(reportSections.map(() => true)),
    [saved, setSaved] = useState<string[]>([]),
    [busy, setBusy] = useState(false),
    [approvedCount, setApprovedCount] = useState(0);
  useEffect(() => {
    loadApprovedSummaries().then((a) => setApprovedCount(a.length)).catch(() => {});
  }, []);
  const selected = reportTypes.find((r) => r.id === type) ?? reportTypes[0];
  const camStatus = useCameras();
  const realCams = useRealCameras(useStreamSources());
  if (!selected) return null;
  const realPilot = [
    { item: "Camera streams", status: realCams.length ? `${realCams.filter((c) => camStatus[c.id]?.state === "online").length} of ${realCams.length} online` : "No camera configured", tone: "neutral" as const },
    { item: "Rules & use cases", status: "Not approved", tone: "neutral" as const },
    { item: "Detection quality", status: "Not measured", tone: "neutral" as const },
    { item: "Alert latency", status: "Not measured", tone: "neutral" as const },
    { item: "Alarm interface", status: "No field output connected", tone: "neutral" as const },
    { item: "Site UAT", status: "Not recorded", tone: "neutral" as const },
  ];
  return (
    <>
      <PageHeader
        eyebrow="Governance / Reports"
        title="Reports & pilot readiness"
        subtitle="Traceable shift summaries and controlled validation."
        actions={
          <Link to="/report/$reportId" params={{ reportId: "pack" }}>
            <Btn>
              <FileText className="size-4" />
              Saved reports
            </Btn>
          </Link>
        }
      />
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <Card>
          <CardHead title="Create a report" />
          <div className="space-y-5 p-4">
            <div className="grid gap-3 sm:grid-cols-3" role="radiogroup" aria-label="Report type">
              {reportTypes.map((r) => (
                <label
                  key={r.id}
                  className={`cursor-pointer rounded-md border p-3 ${type === r.id ? "border-primary bg-nav-active" : "border-border bg-field"}`}
                >
                  <input
                    className="mr-2 accent-primary"
                    type="radio"
                    name="report-type"
                    checked={type === r.id}
                    onChange={() => setType(r.id)}
                  />
                  <strong className="text-[13px]">{r.title}</strong>
                  <p className="mt-1 text-xs text-muted-foreground">{r.note}</p>
                </label>
              ))}
            </div>
            <div className="grid gap-3 sm:grid-cols-2">
              <Field label="Scope">
                <Select options={demoOr(["All demonstration rigs", "Demo rig 01"], ["This rig"])} />
              </Field>
              <Field label="Period">
                <Select options={demoDataEnabled() ? (type === "weekly" ? [`Week ${weeklyReportMeta.week} · ${weeklyReportMeta.period}`] : ["06 Oct / Day shift"]) : [type === "weekly" ? "Latest week with recorded alerts" : "Today so far"]} />
              </Field>
            </div>
            <div>
              {reportSections.map((s, i) => (
                <Check
                  key={s}
                  checked={sections[i] ?? false}
                  onChange={(v) => setSections(sections.map((old, j) => (i === j ? v : old)))}
                >
                  {s}
                </Check>
              ))}
            </div>
            <div className="flex flex-wrap items-center justify-between gap-3 border-t border-border pt-4">
              <span className="text-xs text-muted-foreground">
                {type === "weekly"
                  ? `Printable report · ${approvedCount} HSE-approved event summar${approvedCount === 1 ? "y" : "ies"} on file`
                  : "Printable report · opens in the report viewer"}
              </span>
              <Btn
                variant="primary"
                disabled={busy || !sections.some(Boolean)}
                onClick={() => {
                  setBusy(true);
                  setTimeout(() => {
                    setBusy(false);
                    setSaved([...saved, `${selected.title} · ${demoOr("06 Oct / Day shift", "Today so far")}`]);
                    toast.success(`${selected.title} ready`);
                    nav({ to: "/report/$reportId", params: { reportId: type } });
                  }, 600);
                }}
              >
                <Download className="size-4" />
                {busy ? "Preparing…" : "Generate report"}
              </Btn>
            </div>
          </div>
        </Card>
        <Card>
          <CardHead title="Pilot readiness" sub="Evidence before expansion." />
          <div className="p-4">
            <p className="mb-3 text-sm text-muted-foreground">
              Controlled validation on the selected rig.
            </p>
            {demoOr(pilotReadiness, realPilot).map((p) => (
              <Row key={p.item} k={p.item} v={<Chip tone={p.tone}>{p.status}</Chip>} />
            ))}
          </div>
        </Card>
      </div>
    </>
  );
}
