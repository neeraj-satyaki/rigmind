import { useState, useSyncExternalStore } from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import { isAlertMuted, setAlertMuted, subscribeAlertMuted } from "@/lib/alert-sound";
import { Volume2, VolumeX, Settings, Grid2x2, Focus } from "lucide-react";
import { cameras as mockCameras, severityTone } from "@/data/mock";
import { NoteDialog } from "@/components/rig/workflow";
import { useAppState } from "@/components/rig/AppState";
import { StreamVideo } from "@/components/rig/StreamVideo";
import { DetectionControls } from "@/components/rig/DetectionControls";
import { useCameras } from "@/lib/camera-status";
import { isNative } from "@/lib/native-bridge";
import { demoDataEnabled } from "@/lib/demo-mode";
import { clock, useRealCameras, useRealEvents, useStreamSources } from "@/lib/real-state";
import {
  Btn,
  Card,
  CardHead,
  CameraTile,
  Chip,
  Mono,
  PageHeader,
  Row,
  Tabs,
  pageHead,
} from "@/components/rig/ui";

export const Route = createFileRoute("/_shell/live")({
  head: pageHead("Live monitoring", "Live camera monitoring with active safety rule overlays."),
  component: Live,
});

function Live() {
  const [tab, setTab] = useState("derrick");
  const muted = useSyncExternalStore(subscribeAlertMuted, isAlertMuted, () => false);
  const { items, respond } = useAppState();
  const demo = demoDataEnabled();
  const ack = items.find((a) => a.id === "EV-1042")?.status !== "Open";
  const [focus, setFocus] = useState(false);
  // camera id -> rtsp:// URI, saved in device storage; only the host can open streams
  const sources = useStreamSources(true);
  const realCameras = useRealCameras(sources);
  const cameras = demo ? mockCameras : realCameras;
  const { events } = useRealEvents(50);
  const visible = cameras.filter(
    (c) =>
      !demo ||
      tab === "all" ||
      (tab === "derrick" && c.area === "Derrick board") ||
      (tab === "floor" && c.area === "Drill floor") ||
      (tab === "pipe" && c.area === "Pipe racking"),
  );
  const main = visible[0] ?? (demo ? cameras[0] : undefined);
  const mainUri = main ? sources[main.id] : undefined;
  const liveCameras = useCameras();
  const mainState = main ? liveCameras[main.id]?.state : undefined;
  const native = isNative();
  // The overlay follows the open alert for the camera's area, not a fixed flag.
  const openAlert = main ? items.find((a) => a.status === "Open" && a.area === main.area) : undefined;
  // Real data: the current alarm episode is the newest alert the host has not cleared.
  const active = demo ? undefined : items.find((a) => a.status !== "Cleared");
  const activeEvent = active ? events.find((e) => e.id === active.id) : undefined;
  const activeAck = active?.status === "Acknowledged";
  const online = cameras.filter((c) => liveCameras[c.id]?.state === "online").length;
  return (
    <>
      <PageHeader
        eyebrow="Workspace / Live monitoring"
        title="Live monitoring"
        subtitle={demo ? "Demo rig 01 · Derrick board" : `${cameras.length} camera${cameras.length === 1 ? "" : "s"} configured`}
        actions={
          <>
            <Btn onClick={() => setTab("all")}>
              <Grid2x2 className="size-4" /> {cameras.length} cameras
            </Btn>
            <Btn variant="primary" onClick={() => setFocus(!focus)}>
              <Focus className="size-4" /> {focus ? "Exit focus" : "Focus mode"}
            </Btn>
          </>
        }
      />
      {demo && (
        <Tabs
          value={tab}
          onChange={setTab}
          tabs={[
            { id: "derrick", label: "Derrick board" },
            { id: "floor", label: "Drill floor" },
            { id: "pipe", label: "Pipe handling" },
            { id: "all", label: "All cameras" },
          ]}
        />
      )}
      <div className={focus ? "grid gap-4" : "grid gap-4 xl:grid-cols-[1fr_340px]"}>
        <div className="space-y-4">
          <div>
            {!main && (
              <Card className="p-8 text-center">
                <div className="font-semibold">No cameras configured</div>
                <p className="mt-1 text-sm text-muted-foreground">Add a camera under Administration / Cameras to see live video here.</p>
                <Link to="/admin/cameras/new">
                  <Btn className="mt-3">Add a camera</Btn>
                </Link>
              </Card>
            )}
            {main && (
              <CameraTile
                id={main.id}
                area={main.area}
                overlay={demo ? !!openAlert : !!active && activeEvent?.camId === main.id && !activeEvent.cleared}
                overlayLabel={demo ? openAlert?.condition : active?.condition}
                live={!!mainUri}
                offline={(native || !demo) && !mainUri}
              >
                {mainUri && <StreamVideo camId={main.id} uri={mainUri} />}
              </CameraTile>
            )}
            {main && (
              <div className="-mt-px flex flex-wrap items-center gap-2 rounded-b-lg border border-border bg-card px-3 py-2">
                <Btn
                  variant="ghost"
                  className="px-2"
                  onClick={() => setAlertMuted(!muted)}
                  aria-label={muted ? "Unmute alert sounds" : "Mute alert sounds"}
                  title={muted ? "Alert sounds off. Click to unmute" : "Alert sounds on. Click to mute"}
                  aria-pressed={muted}
                >
                  {muted ? <VolumeX className="size-4" /> : <Volume2 className="size-4" />}
                </Btn>
                <Mono className="text-muted-foreground">
                  {native
                    ? mainState === "online"
                      ? "Live"
                      : mainState === "offline"
                        ? "Camera offline · reconnecting"
                        : "Connecting"
                    : demo
                      ? "09:42:18 AST"
                      : "Camera offline"}
                </Mono>
                {mainUri && <DetectionControls camId={main.id} />}
                {demo && (
                  <Chip tone="advisory" className="ml-auto">
                    Rule DZ-01 active
                  </Chip>
                )}
                <NoteDialog
                  title="Camera settings"
                  label="Settings"
                  icon={<Settings className="size-4" />}
                  text="Use Administration / Cameras to validate streams and Administration / Zones to edit geometry."
                />
              </div>
            )}
          </div>
          <div className="grid gap-4 md:grid-cols-3">
            {visible.slice(1).map((c) => (
              <CameraTile key={c.id} id={c.id} area={c.area} live={!!sources[c.id]} offline={(native || !demo) && !sources[c.id]}>
                {sources[c.id] ? <StreamVideo camId={c.id} uri={sources[c.id] as string} /> : null}
              </CameraTile>
            ))}
            <Card className="flex flex-col justify-center p-4">
              <div className="eyebrow">Monitoring health</div>
              <div className="mt-2 font-mono text-2xl font-semibold">
                {demo ? cameras.length : online} / {cameras.length}{" "}
                <span className="text-sm font-normal text-muted-foreground">{demo ? "cameras" : "online"}</span>
              </div>
              <div className="text-xs text-muted-foreground">
                {demo ? "Frames and inference current" : cameras.length ? "Cameras reporting to the host" : "No cameras configured"}
              </div>
              <div className="mt-3 h-1.5 rounded-full bg-success-bg">
                <div
                  className="h-full rounded-full bg-success"
                  style={{ width: demo ? "100%" : cameras.length ? `${(online / cameras.length) * 100}%` : "0%" }}
                />
              </div>
            </Card>
          </div>
        </div>
        <div className={focus ? "hidden" : "space-y-4"}>
          {demo ? (
            <>
              <Card>
                <div className="space-y-3 p-4">
                  <div className="flex items-center justify-between">
                    <Chip tone="critical" dot>
                      Critical
                    </Chip>
                    <Mono>EV-1042</Mono>
                  </div>
                  <div className="text-base font-semibold">Occupied zone during equipment movement</div>
                  <p className="text-[13px] text-muted-foreground">
                    A person overlaps the active movement zone.
                  </p>
                  <div>
                    <Row k="First observed" v={<Mono>09:42:18 AST</Mono>} />
                    <Row k="Hazard state" v={<Chip tone="critical">Active</Chip>} />
                    <Row
                      k="Response"
                      v={
                        ack ? (
                          <Chip tone="success">Acknowledged</Chip>
                        ) : (
                          <Chip tone="warning">Unacknowledged</Chip>
                        )
                      }
                    />
                  </div>
                  {ack ? (
                    <Btn variant="muted" className="w-full" disabled>
                      Acknowledged by Faisal A.
                    </Btn>
                  ) : (
                    <Btn
                      variant="danger"
                      className="w-full"
                      onClick={() => respond("EV-1042", "Acknowledged")}
                    >
                      Acknowledge alert
                    </Btn>
                  )}
                  <Link to="/events/$eventId" params={{ eventId: "EV-1042" }}>
                    <Btn className="mt-2 w-full">Open event investigation</Btn>
                  </Link>
                  <p className="text-[11px] text-muted-foreground">
                    Acknowledging records your response. The hazard remains active.
                  </p>
                </div>
              </Card>
              <Card>
                <CardHead title="Context" />
                <div className="px-4 py-1">
                  <Row k="Activity" v="Tubular handling" />
                  <Row k="Rig signal" v={<Chip tone="warning">Movement active</Chip>} />
                  <Row k="Signal age" v="1 second" />
                </div>
              </Card>
            </>
          ) : (
            <>
              <Card>
                <div className="space-y-3 p-4">
                  {active ? (
                    <>
                      <div className="flex items-center justify-between">
                        <Chip tone={severityTone(active.severity)} dot>
                          {active.severity}
                        </Chip>
                        <Mono>{active.id}</Mono>
                      </div>
                      <div className="text-base font-semibold">{active.condition}</div>
                      {(active.rig || active.area) && (
                        <p className="text-[13px] text-muted-foreground">{[active.rig, active.area].filter(Boolean).join(" · ")}</p>
                      )}
                      <div>
                        <Row k="First observed" v={<Mono>{activeEvent ? clock(activeEvent.startMs) : `${active.age} ago`}</Mono>} />
                        <Row k="Alert state" v={<Chip tone={active.status === "Monitoring" ? "advisory" : "critical"}>{active.status === "Monitoring" ? "Monitoring" : "Active"}</Chip>} />
                        <Row k="Response" v={activeAck ? <Chip tone="success">Acknowledged</Chip> : <Chip tone="warning">Unacknowledged</Chip>} />
                      </div>
                      {activeAck ? (
                        <Btn variant="muted" className="w-full" disabled>
                          Acknowledged by {active.owner}
                        </Btn>
                      ) : (
                        <Btn variant="danger" className="w-full" onClick={() => respond(active.id, "Acknowledged")}>
                          Acknowledge alert
                        </Btn>
                      )}
                      <Link to="/events/$eventId" params={{ eventId: active.id }}>
                        <Btn className="mt-2 w-full">Open event investigation</Btn>
                      </Link>
                      <p className="text-[11px] text-muted-foreground">
                        Acknowledging records your response. The hazard remains active.
                      </p>
                    </>
                  ) : (
                    <>
                      <Chip tone="success" dot>
                        Clear
                      </Chip>
                      <div className="text-base font-semibold">No active alert</div>
                      <p className="text-[13px] text-muted-foreground">
                        Nothing is raised right now. Alarm episodes from the cameras appear here as they happen.
                      </p>
                    </>
                  )}
                </div>
              </Card>
              <Card>
                <CardHead title="Camera" sub={main?.id} />
                <div className="px-4 py-1">
                  {main ? (
                    <>
                      <Row
                        k="State"
                        v={
                          mainState ? (
                            <Chip tone={mainState === "online" ? "success" : mainState === "offline" ? "critical" : "warning"}>{mainState}</Chip>
                          ) : (
                            "Not reported"
                          )
                        }
                      />
                      <Row k="Resolution" v={liveCameras[main.id]?.width ? `${liveCameras[main.id]?.width} × ${liveCameras[main.id]?.height}` : "Unknown"} />
                      <Row k="Frame rate" v={liveCameras[main.id]?.fps ? `${liveCameras[main.id]?.fps.toFixed(1)} fps` : "Unknown"} />
                    </>
                  ) : (
                    <Row k="State" v="No camera" />
                  )}
                </div>
              </Card>
            </>
          )}
        </div>
      </div>
    </>
  );
}
