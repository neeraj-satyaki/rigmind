import { createFileRoute, Link } from "@tanstack/react-router";
import { ArrowRightLeft, MonitorPlay, AlertTriangle, CheckCircle2 } from "lucide-react";
import feed from "@/assets/rig-feed.jpg";
import { Btn, Card, CardHead, Chip, Mono, PageHeader, Row, pageHead } from "@/components/rig/ui";
import { monitoredAreas } from "@/data/mock";
import { demoDataEnabled } from "@/lib/demo-mode";
import { useAppState } from "@/components/rig/AppState";
import { useCameras } from "@/lib/camera-status";
import { clock, useRealCameras, useRealEvents, useStreamSources } from "@/lib/real-state";

export const Route = createFileRoute("/_shell/rig")({
  head: pageHead("Rig workspace", "Rig status, coverage and shift context."),
  component: Rig,
});

function Rig() {
  return demoDataEnabled() ? <DemoRig /> : <RealRig />;
}

function RealRig() {
  const { items } = useAppState();
  const status = useCameras();
  const cams = useRealCameras(useStreamSources());
  const { events } = useRealEvents(20);
  const open = items.filter((a) => a.status === "Open" || a.status === "Escalated" || a.status === "Monitoring");
  const online = cams.filter((c) => status[c.id]?.state === "online").length;
  const count = (sev: string) => open.filter((a) => a.severity === sev).length;
  return (
    <>
      <PageHeader
        eyebrow="Workspace / Rig"
        title="Rig workspace"
        subtitle="Status and coverage from this device"
        actions={
          <Link to="/live">
            <Btn variant="primary">
              <MonitorPlay className="size-4" /> Open monitoring
            </Btn>
          </Link>
        }
      />
      <Card className="grid overflow-hidden md:grid-cols-[1fr_200px]">
        <div className="p-5">
          {cams.length === 0 ? (
            <Chip tone="neutral">No cameras configured</Chip>
          ) : online === cams.length ? (
            <Chip tone="success" dot>
              Monitoring ready
            </Chip>
          ) : (
            <Chip tone="warning" dot>
              {online === 0 ? "No camera online" : "Partial coverage"}
            </Chip>
          )}
          <div className="mt-3 text-lg font-semibold">Rig status and coverage</div>
          <div className="mt-2 flex gap-4 text-[13px] text-muted-foreground">
            <span>
              {online} of {cams.length} camera{cams.length === 1 ? "" : "s"} online
            </span>
          </div>
        </div>
        <div className="border-t border-border p-5 md:border-l md:border-t-0">
          <div className="eyebrow">Open alerts</div>
          <div className="mt-1 font-mono text-[32px] font-semibold">{String(open.length).padStart(2, "0")}</div>
          <div className="text-xs text-muted-foreground">
            {open.length ? `${count("Critical")} critical / ${count("Warning")} warning / ${count("Advisory")} advisory` : "No alerts yet"}
          </div>
        </div>
      </Card>
      <h2 className="mb-3 mt-6 font-semibold">Cameras</h2>
      <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        {cams.map((c) => {
          const st = status[c.id];
          return (
            <Card key={c.id} className="p-4">
              <div className="flex items-start justify-between">
                <Mono className="font-semibold">{c.id}</Mono>
                <Chip tone={st?.state === "online" ? "success" : st?.state === "offline" ? "critical" : "warning"}>{st?.state ?? "Not reported"}</Chip>
              </div>
              <div className="mt-3 text-[13px] text-muted-foreground">
                {st?.state === "online" && st.width ? `${st.width} × ${st.height}` : st?.reason || "No stream information yet"}
              </div>
            </Card>
          );
        })}
        {!cams.length && (
          <Card className="p-4 text-sm text-muted-foreground sm:col-span-2 xl:col-span-4">
            No cameras configured. Add one under Administration / Cameras.
          </Card>
        )}
      </div>
      <div className="mt-4">
        <Card>
          <CardHead title="Recent activity" />
          <div className="divide-y divide-border">
            {events.slice(0, 5).map((e) => (
              <div key={e.id} className="flex gap-3 px-4 py-3">
                {e.cleared ? <CheckCircle2 className="size-4 text-success" /> : <AlertTriangle className="size-4 text-critical" />}
                <div className="flex-1">
                  <div className="font-medium">{e.condition || "Safety condition"}</div>
                  <div className="text-xs text-muted-foreground">
                    {e.camId} · <Mono>{e.id}</Mono>
                    {e.cleared ? " · cleared" : ""}
                  </div>
                </div>
                <Mono className="text-muted-foreground">{clock(e.startMs)}</Mono>
              </div>
            ))}
            {!events.length && <div className="px-4 py-6 text-sm text-muted-foreground">No events recorded.</div>}
          </div>
        </Card>
      </div>
    </>
  );
}

function DemoRig() {
  return (
    <>
      <PageHeader eyebrow="Workspace / Rig" title="Demo rig 01" subtitle="Eastern Province · Day shift 06:00–18:00 AST"
        actions={<><Link to="/actions"><Btn><ArrowRightLeft className="size-4" /> Shift handover</Btn></Link><Link to="/live"><Btn variant="primary"><MonitorPlay className="size-4" /> Open monitoring</Btn></Link></>} />
      <Card className="grid overflow-hidden md:grid-cols-[1fr_240px_200px]">
        <div className="p-5">
          <Chip tone="success" dot>Monitoring ready</Chip>
          <div className="mt-3 text-lg font-semibold">Rig status & shift coverage</div>
          <div className="mt-2 flex gap-4 text-[13px] text-muted-foreground"><span>6 cameras online</span><span>Synced 2 seconds ago</span></div>
        </div>
        <img src={feed} alt="Rig floor" loading="lazy" width={1280} height={720} className="h-40 w-full object-cover md:h-full" />
        <div className="border-t border-border p-5 md:border-l md:border-t-0">
          <div className="eyebrow">Open alerts</div>
          <div className="mt-1 font-mono text-[32px] font-semibold">03</div>
          <div className="text-xs text-muted-foreground">1 critical / 1 warning / 1 advisory</div>
        </div>
      </Card>
      <h2 className="mb-3 mt-6 font-semibold">Monitored areas</h2>
      <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        {monitoredAreas.map((a) => (
          <Card key={a.name} className="p-4">
            <div className="flex items-start justify-between"><div className="font-semibold">{a.name}</div><Chip tone={a.tone}>{a.status}</Chip></div>
            <Mono className="text-muted-foreground">{a.cams}</Mono>
            <div className="mt-3 text-[13px] text-muted-foreground">{a.note}</div>
          </Card>
        ))}
      </div>
      <div className="mt-4 grid gap-4 lg:grid-cols-2">
        <Card>
          <CardHead title="Operational context" />
          <div className="px-4 py-1">
            <Row k="Current activity" v="Tubular handling" />
            <Row k="Equipment state" v={<Chip tone="warning">Movement active</Chip>} />
            <Row k="Last update" v="1 second ago" />
            <Row k="Shift supervisor" v="Faisal A." />
          </div>
        </Card>
        <Card>
          <CardHead title="Recent activity" />
          <div className="divide-y divide-border">
            <div className="flex gap-3 px-4 py-3"><AlertTriangle className="size-4 text-critical" /><div className="flex-1"><div className="font-medium">Critical event detected</div><div className="text-xs text-muted-foreground">Derrick board · <Mono>EV-1042</Mono></div></div><Mono className="text-muted-foreground">09:42</Mono></div>
            <div className="flex gap-3 px-4 py-3"><CheckCircle2 className="size-4 text-success" /><div className="flex-1"><div className="font-medium">Shift checklist completed</div><div className="text-xs text-muted-foreground">Faisal A. · 6 cameras ready</div></div><Mono className="text-muted-foreground">06:12</Mono></div>
          </div>
        </Card>
      </div>
    </>
  );
}
