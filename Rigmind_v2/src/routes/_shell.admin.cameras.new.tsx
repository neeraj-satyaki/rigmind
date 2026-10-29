import { useEffect, useState } from "react";
import { createFileRoute, Link } from "@tanstack/react-router";
import { Switch } from "@/components/ui/switch";
import {
  Btn,
  Card,
  CardHead,
  CameraTile,
  Chip,
  Field,
  PageHeader,
  Row,
  Select,
  inputCls,
  pageHead,
} from "@/components/rig/ui";
import { Loader } from "@/components/rig/Loader";
import { readDevice, writeDevice } from "@/lib/device-db";
import { toast } from "sonner";
export const Route = createFileRoute("/_shell/admin/cameras/new")({
  head: pageHead("Connect a camera", "Register and validate a new rig camera stream."),
  component: NewCamera,
});
// Puts the camera login into the RTSP URI (rtsp://user:pass@host/path); the pipeline reads it from there.
function withCredentials(base: string, user: string, secret: string) {
  if (!user) return base;
  const url = new URL(base);
  url.username = user;
  url.password = secret;
  return url.toString();
}

function NewCamera() {
  const [step, setStep] = useState(0),
    [enabled, setEnabled] = useState(false),
    [valid, setValid] = useState(false),
    [busy, setBusy] = useState(false),
    [saved, setSaved] = useState(false),
    [zone, setZone] = useState("Derrick exclusion zone"),
    [name, setName] = useState("Derrick board / CAM-01"),
    [uri, setUri] = useState("rtsp://demo-camera.local/stream01"),
    [cameraId, setCameraId] = useState("CAM-01"),
    [user, setUser] = useState(""),
    [secret, setSecret] = useState(""),
    [rig, setRig] = useState("Demo rig 01"),
    [area, setArea] = useState("Derrick board");
  const steps = ["Connection details", "Stream validation", "Zone assignment"];
  useEffect(() => {
    readDevice<{ name: string; uri: string; rig: string; area: string; zone: string; enabled: boolean; cameraId?: string; user?: string; secret?: string }>("camera").then(camera => {
      if (!camera) return;
      setName(camera.name); setUri(camera.uri); setRig(camera.rig); setArea(camera.area); setZone(camera.zone); setEnabled(camera.enabled);
      setCameraId(camera.cameraId ?? "CAM-01"); setUser(camera.user ?? ""); setSecret(camera.secret ?? "");
    }).catch(() => toast.error("Saved camera could not be loaded"));
  }, []);
  const test = async () => {
    if (!name.trim() || !uri.match(/^(rtsp|https?):\/\//)) {
      toast.error("Enter a camera name and a valid stream URI");
      return;
    }
    setBusy(true);
    await new Promise((r) => setTimeout(r, 700));
    setValid(true);
    setBusy(false);
    toast.success("Simulated stream validation passed");
  };
  return (
    <>
      <PageHeader
        eyebrow="Administration / Cameras"
        title="Connect a camera"
        subtitle="Register a stream and validate it before monitoring."
      />
      <div className="mb-4 flex flex-wrap gap-2">
        {steps.map((s, i) => (
          <Btn
            key={s}
            variant={step === i ? "primary" : "outline"}
            onClick={() => setStep(i)}
            aria-current={step === i ? "step" : undefined}
          >
            <span className="font-mono">{i + 1}</span>
            {s}
          </Btn>
        ))}
      </div>
      <div className="grid gap-4 xl:grid-cols-[1fr_380px]">
        <Card>
          <CardHead title={steps[step]} sub="Simulation only · No real stream connection" />
          {busy ? (
            <Loader />
          ) : step === 0 ? (
            <div className="grid gap-4 p-4 md:grid-cols-2">
              <Field label="Camera name">
                <input
                  className={inputCls}
                  value={name}
                  onChange={(e) => {
                    setName(e.target.value);
                    setValid(false);
                  }}
                />
              </Field>
              <Field label="Assigned rig">
                <Select
                  value={rig}
                  onChange={setRig}
                  options={["Demo rig 01", "Demo rig 02", "Demo rig 03", "Demo rig 04"]}
                />
              </Field>
              <Field label="Connection protocol">
                <Select options={["RTSP / ONVIF", "RTSP", "HLS"]} />
              </Field>
              <Field label="Area">
                <input
                  className={inputCls}
                  value={area}
                  onChange={(e) => setArea(e.target.value)}
                />
              </Field>
              <Field label="Stream URI">
                <input
                  className={inputCls}
                  value={uri}
                  onChange={(e) => {
                    setUri(e.target.value);
                    setValid(false);
                  }}
                />
              </Field>
              <Field label="Camera ID">
                <input className={inputCls} value={cameraId} onChange={(e) => setCameraId(e.target.value.trim())} />
              </Field>
              <Field label="Camera username">
                <input className={inputCls} value={user} autoComplete="off" onChange={(e) => setUser(e.target.value)} />
              </Field>
              <Field label="Camera password">
                <input type="password" className={inputCls} value={secret} autoComplete="new-password" onChange={(e) => setSecret(e.target.value)} />
              </Field>
              <label className="flex items-center gap-3">
                <Switch checked={enabled} onCheckedChange={setEnabled} />
                Enable monitoring after approval
              </label>
            </div>
          ) : step === 1 ? (
            <div className="space-y-3 p-4">
              <Row k="Stream URI" v={uri} />
              <Row
                k="Authentication"
                v={
                  <Chip tone={valid ? "success" : "neutral"}>
                    {valid ? "Passed" : "Not tested"}
                  </Chip>
                }
              />
              <Row k="Resolution" v={valid ? "1920 × 1080" : "Awaiting validation"} />
              <Row k="Stream rate" v={valid ? "25 fps" : "Awaiting validation"} />
              <Row k="Clock synchronization" v={valid ? "Passed" : "Not tested"} />
              <Btn onClick={test}>{valid ? "Revalidate stream" : "Run stream validation"}</Btn>
            </div>
          ) : (
            <div className="space-y-4 p-4">
              <Field label="Assigned zone">
                <Select
                  options={[
                    "Derrick exclusion zone",
                    "Drill-floor restricted zone",
                    "Pipe-handling zone",
                  ]}
                  value={zone}
                  onChange={setZone}
                />
              </Field>
              <Row k="Camera" v={name} />
              <Row k="Rig" v={rig} />
              <Row k="Rule" v="DZ-01 · Approval required" />
              <Link to="/admin/zones">
                <Btn>Edit zone geometry</Btn>
              </Link>
              <p className="text-xs text-muted-foreground">
                A connection alone does not activate safety rules.
              </p>
            </div>
          )}
          <div className="flex flex-wrap gap-2 border-t p-4">
            <Btn disabled={busy} onClick={test}>
              Test connection
            </Btn>
            <Link to="/admin/health">
              <Btn variant="ghost">Cancel</Btn>
            </Link>
            {step > 0 && <Btn onClick={() => setStep(step - 1)}>Back</Btn>}
            {step < 2 ? (
              <Btn
                variant="primary"
                disabled={busy}
                onClick={() => {
                  if (step === 0 && (!name.trim() || !uri.match(/^(rtsp|https?):\/\//))) {
                    toast.error("Enter a valid camera name and stream URI");
                    return;
                  }
                  if (step === 1 && !valid) {
                    toast.error("Validate the stream before assigning a zone");
                    return;
                  }
                  setStep(step + 1);
                }}
              >
                Continue
              </Btn>
            ) : (
              <Btn
                variant="primary"
                disabled={!valid || busy}
                onClick={async () => {
                  setBusy(true);
                  try {
                    await writeDevice("camera", { name, uri, rig, area, zone, enabled, cameraId, user, secret });
                    // The Live page opens streams from stream.sources, keyed by camera id; credentials go in the URI.
                    const sources = (await readDevice<Record<string, string>>("stream.sources")) ?? {};
                    await writeDevice("stream.sources", { ...sources, [cameraId]: withCredentials(uri, user, secret) });
                    setSaved(true);
                    toast.success("Camera saved on this device; monitoring awaits approval");
                  } catch {
                    toast.error("Camera could not be saved");
                  } finally {
                    setBusy(false);
                  }
                }}
              >
                {saved ? "Camera saved" : "Save camera"}
              </Btn>
            )}
          </div>
        </Card>
        <div className="space-y-4">
          <CameraTile id="CAM-01" area={area} overlay={step === 2} />
          <Card>
            <CardHead title="Validation results" />
            <div className="p-4">
              <Row
                k="Stream"
                v={
                  <Chip tone={valid ? "success" : "neutral"}>
                    {valid ? "Validated" : "Not tested"}
                  </Chip>
                }
              />
              <Row k="Zone" v={step === 2 ? zone : "Not assigned"} />
              <Row k="Monitoring" v={enabled ? "Pending approval" : "Not enabled"} />
            </div>
          </Card>
        </div>
      </div>
    </>
  );
}
