import { useEffect, useRef, useState } from "react";
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
import { StreamVideo } from "@/components/rig/StreamVideo";
import { PasswordInput } from "@/components/rig/PasswordInput";
import { readDevice, writeDevice } from "@/lib/device-db";
import { toast } from "sonner";
import { demoDataEnabled } from "@/lib/demo-mode";
import { useCamera } from "@/lib/camera-status";
import { isNative, nativeCall } from "@/lib/native-bridge";
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
  const demo = demoDataEnabled();
  const [step, setStep] = useState(0),
    [enabled, setEnabled] = useState(false),
    [valid, setValid] = useState(false),
    [busy, setBusy] = useState(false),
    [saved, setSaved] = useState(false),
    [zone, setZone] = useState(demo ? "Derrick exclusion zone" : "Not assigned"),
    [name, setName] = useState(demo ? "Derrick board / CAM-01" : "CAM-01"),
    [uri, setUri] = useState(demo ? "rtsp://demo-camera.local/stream01" : "rtsp://"),
    [cameraId, setCameraId] = useState("CAM-01"),
    [user, setUser] = useState(""),
    [secret, setSecret] = useState(""),
    [rig, setRig] = useState(demo ? "Demo rig 01" : "This rig"),
    [area, setArea] = useState(demo ? "Derrick board" : "Camera stream"),
    [problem, setProblem] = useState(""),
    [previewUri, setPreviewUri] = useState(""); // the stream being validated: shown live in the right-hand panel
  const live = useCamera(cameraId);
  const checked = useRef({ current: "connecting" as string, reason: "" }).current;
  checked.current = live?.state ?? "connecting";
  checked.reason = live?.reason ?? "";
  const steps = ["Connection details", "Stream validation", "Zone assignment"];
  useEffect(() => {
    readDevice<{ name: string; uri: string; rig: string; area: string; zone: string; enabled: boolean; cameraId?: string; user?: string; secret?: string }>("camera").then(camera => {
      if (!camera) return;
      setName(camera.name); setUri(camera.uri); setRig(camera.rig); setArea(camera.area); setZone(camera.zone); setEnabled(camera.enabled);
      setCameraId(camera.cameraId ?? "CAM-01"); setUser(camera.user ?? ""); setSecret(camera.secret ?? "");
    }).catch(() => toast.error("Saved camera could not be loaded"));
  }, []);
  // Opens the stream through the host and waits for the pipeline to report it online (or why it could not connect).
  const test = async () => {
    if (!name.trim() || !uri.match(/^(rtsp|https?):\/\//) || uri.trim().length < 9) {
      toast.error("Enter a camera name and a valid stream URI");
      return;
    }
    if (!isNative()) {
      setProblem("Stream validation runs in the RigMind desktop app.");
      toast.error("Stream validation needs the RigMind desktop app");
      return;
    }
    setBusy(true);
    setProblem("");
    setValid(false);
    setPreviewUri(""); // a fresh viewer for every validation
    try {
      const target = withCredentials(uri, user, secret);
      await nativeCall("stream.open", { cam: cameraId, uri: target });
      setPreviewUri(target);
      const end = Date.now() + 20_000;
      while (Date.now() < end) {
        await new Promise((r) => setTimeout(r, 500));
        if (checked.current === "online") break;
      }
      if (checked.current === "online") {
        setValid(true);
        toast.success("Stream is online");
      } else {
        setPreviewUri("");
        setProblem(checked.reason || "The camera did not answer within 20 seconds. Check the address, the network and the login.");
        toast.error("Stream could not be validated", { description: checked.reason || undefined });
      }
    } catch (e) {
      setPreviewUri("");
      setProblem(String(e));
      toast.error("Stream could not be opened", { description: String(e) });
    } finally {
      setBusy(false);
    }
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
          <CardHead title={steps[step]} sub={demo ? "Register a stream and validate it" : "The stream is opened for real and must come online before it can be saved"} />
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
                  options={demo ? ["Demo rig 01", "Demo rig 02", "Demo rig 03", "Demo rig 04"] : ["This rig"]}
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
                <PasswordInput value={secret} autoComplete="new-password" onChange={(e) => setSecret(e.target.value)} />
              </Field>
              <div className="flex items-center gap-2 md:col-span-2">
                <Btn
                  variant="ghost"
                  onClick={() => {
                    setUser("");
                    setSecret("");
                    setValid(false);
                    toast("Camera login ignored", { description: "The stream will be opened without a username or password." });
                  }}
                >
                  Ignore (no login needed)
                </Btn>
                <span className="text-xs text-muted-foreground">Leave the login empty for cameras that do not ask for one.</span>
              </div>
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
              <Row k="Resolution" v={valid && live?.width ? `${live.width} × ${live.height}` : "Awaiting validation"} />
              <Row k="Stream rate" v={valid && live?.streamFps ? `${Math.round(live.streamFps)} fps` : "Awaiting validation"} />
              {problem && <p className="text-xs text-critical">{problem}</p>}
              <Btn onClick={test}>{valid ? "Revalidate stream" : "Run stream validation"}</Btn>
            </div>
          ) : (
            <div className="space-y-4 p-4">
              <Field label="Assigned zone">
                <Select
                  options={
                    demo
                      ? ["Derrick exclusion zone", "Drill-floor restricted zone", "Pipe-handling zone"]
                      : ["Not assigned"]
                  }
                  value={zone}
                  onChange={setZone}
                />
              </Field>
              <Row k="Camera" v={name} />
              <Row k="Rig" v={rig} />
              {demo && <Row k="Rule" v="DZ-01 · Approval required" />}
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
          <CameraTile id={cameraId} area={area} overlay={demo && step === 2 && !previewUri} live={!!previewUri}>
            {previewUri && <StreamVideo camId={cameraId} uri={previewUri} />}
          </CameraTile>
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
