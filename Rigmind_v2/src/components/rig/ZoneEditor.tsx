import { useRef, useState } from "react";
import { MousePointer2, Plus, Pentagon, Trash2, RotateCcw, Save, Play } from "lucide-react";
import { toast } from "sonner";
import feed from "@/assets/rig-feed.jpg";
import { zoneDefaults } from "@/data/mock";
import {
  Btn,
  Callout,
  Card,
  CardHead,
  DetectionOverlay,
  Field,
  PageHeader,
  Select,
  inputCls,
} from "./ui";
import { Check } from "./workflow";

export function ZoneEditor() {
  const [points, setPoints] = useState<[number, number][]>(zoneDefaults.map((p) => [...p])),
    [selected, setSelected] = useState(0),
    [mode, setMode] = useState("select"),
    [overlay, setOverlay] = useState(true),
    [compare, setCompare] = useState(false),
    [name, setName] = useState("Derrick exclusion zone");
  const drag = useRef<number | null>(null);
  const svg = useRef<SVGSVGElement>(null);
  const reset = () => {
    setPoints(zoneDefaults.map((p) => [...p]));
    setSelected(0);
  };
  const pointAt = (clientX: number, clientY: number): [number, number] => {
    const rect = svg.current?.getBoundingClientRect();
    if (!rect) return [50, 50];
    return [
      Math.max(0, Math.min(100, ((clientX - rect.left) / rect.width) * 100)),
      Math.max(0, Math.min(100, ((clientY - rect.top) / rect.height) * 100)),
    ];
  };
  return (
    <>
      <PageHeader
        eyebrow="Governance / Geometry"
        title="Zone editor"
        subtitle="CAM-01 / Derrick board / Demo rig 01"
        actions={
          <>
            <Btn onClick={reset}>
              <RotateCcw className="size-4" />
              Reset
            </Btn>
            <Btn
              variant="primary"
              onClick={() => toast.success("Zone draft saved; approved geometry unchanged")}
            >
              <Save className="size-4" />
              Save zone draft
            </Btn>
          </>
        }
      />
      <div className="grid gap-4 xl:grid-cols-[1.7fr_1fr]">
        <div>
          <div className="relative aspect-video overflow-hidden rounded-lg bg-slate">
            <img
              src={feed}
              alt="Derrick board simulated camera image"
              className="h-full w-full object-cover"
            />
            {overlay && <DetectionOverlay />}
            <svg
              ref={svg}
              viewBox="0 0 100 100"
              preserveAspectRatio="none"
              className="absolute inset-0 h-full w-full touch-none"
              onPointerMove={(e) => {
                const index = drag.current;
                if (index === null) return;
                const p = pointAt(e.clientX, e.clientY);
                setPoints((old) => old.map((v, i) => (i === index ? p : v)));
              }}
              onPointerUp={() => {
                drag.current = null;
              }}
              onPointerCancel={() => {
                drag.current = null;
              }}
              onClick={(e) => {
                if (mode !== "add") return;
                setPoints((old) => [...old, pointAt(e.clientX, e.clientY)]);
                setMode("select");
              }}
            >
              {compare && (
                <polygon
                  points={zoneDefaults.map((p) => p.join(",")).join(" ")}
                  className="fill-deployed/10 stroke-deployed"
                  strokeWidth="0.5"
                  strokeDasharray="2 1"
                />
              )}
              <polygon
                points={points.map((p) => p.join(",")).join(" ")}
                className="fill-zone/15 stroke-zone"
                strokeWidth="0.5"
                strokeDasharray="2 1"
              />
              {points.map((p, i) => (
                <ellipse
                  key={i}
                  cx={p[0]}
                  cy={p[1]}
                  rx={1}
                  ry={1.8}
                  role="button"
                  tabIndex={0}
                  aria-label={`Zone vertex ${i + 1}`}
                  className={
                    selected === i
                      ? "fill-zone stroke-foreground cursor-move"
                      : "fill-card stroke-zone cursor-move"
                  }
                  strokeWidth="0.3"
                  onPointerDown={(e) => {
                    e.stopPropagation();
                    if (mode !== "select") return;
                    setSelected(i);
                    drag.current = i;
                    e.currentTarget.setPointerCapture(e.pointerId);
                  }}
                  onKeyDown={(e) => {
                    const offsets: Record<string, [number, number]> = {
                      ArrowLeft: [-1, 0],
                      ArrowRight: [1, 0],
                      ArrowUp: [0, -1],
                      ArrowDown: [0, 1],
                    };
                    const offset = offsets[e.key];
                    if (!offset) return;
                    e.preventDefault();
                    setPoints((old) =>
                      old.map((v, j) =>
                        i === j
                          ? [
                              Math.max(0, Math.min(100, v[0] + offset[0])),
                              Math.max(0, Math.min(100, v[1] + offset[1])),
                            ]
                          : v,
                      ),
                    );
                  }}
                />
              ))}
            </svg>
            <div className="absolute left-3 top-3 flex gap-1 rounded-md bg-slate p-1">
              {[
                { id: "select", Icon: MousePointer2, title: "Select vertex" },
                { id: "add", Icon: Plus, title: "Add point" },
                { id: "polygon", Icon: Pentagon, title: "Reset polygon" },
                { id: "delete", Icon: Trash2, title: "Delete selected vertex" },
              ].map(({ id, Icon, title }) => (
                <Btn
                  key={id}
                  aria-label={title}
                  title={title}
                  variant={mode === id ? "primary" : "ghost"}
                  className="w-8 px-0 text-slate-foreground"
                  disabled={id === "delete" && points.length <= 3}
                  onClick={() => {
                    if (id === "delete") {
                      setPoints((old) => old.filter((_, i) => i !== selected));
                      setSelected(0);
                    } else if (id === "polygon") {
                      reset();
                      setMode("select");
                    } else setMode(id);
                  }}
                >
                  <Icon className="size-4" />
                </Btn>
              ))}
            </div>
            <span className="absolute bottom-3 left-3 rounded-sm bg-slate px-2 py-1 text-[11px] text-slate-foreground">
              {mode === "add"
                ? "Choose a point on the image"
                : "Select a point to adjust the boundary"}
            </span>
            <span className="absolute bottom-3 right-3 text-[10px] text-slate-foreground">
              SIMULATED IMAGE
            </span>
          </div>
          <p className="mt-2 font-mono text-xs text-muted-foreground">
            {points.length} vertices · Draft geometry · CAM-01
          </p>
        </div>
        <Card>
          <CardHead title="Zone properties" />
          <div className="space-y-4 p-4">
            <Field label="Zone name">
              <input className={inputCls} value={name} onChange={(e) => setName(e.target.value)} />
            </Field>
            <Field label="Zone type">
              <Select options={["Restricted occupancy", "Movement exclusion"]} />
            </Field>
            <Field label="Active during">
              <Select options={["Tubular handling", "All activities"]} />
            </Field>
            <Check checked={overlay} onChange={setOverlay}>
              Show detection overlays
            </Check>
            <Check checked={compare} onChange={setCompare}>
              Compare approved geometry
            </Check>
            <Btn
              onClick={() =>
                toast.success(
                  "Draft geometry validated against simulated replay; approval still required",
                )
              }
            >
              <Play className="size-4" />
              Validate on replay
            </Btn>
          </div>
        </Card>
      </div>
      <div className="mt-4">
        <Callout title="Camera geometry matters">
          Review this zone after camera movement. Pixel geometry alone does not establish distance
          in metres.
        </Callout>
      </div>
    </>
  );
}
