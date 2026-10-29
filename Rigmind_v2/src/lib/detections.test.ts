import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { classColor, containRect, mapBox, mapPoint } from "@/lib/detection-geometry";

describe("containRect (object-contain letterbox)", () => {
  it("letterboxes a 16:9 frame in a taller container", () => {
    const r = containRect(800, 600, 1280, 720);
    expect(r.w).toBeCloseTo(800);
    expect(r.h).toBeCloseTo(450);
    expect(r.x).toBeCloseTo(0);
    expect(r.y).toBeCloseTo(75);
  });
  it("pillarboxes a 16:9 frame in a wider container", () => {
    const r = containRect(1600, 450, 1280, 720);
    expect(r.h).toBeCloseTo(450);
    expect(r.w).toBeCloseTo(800);
    expect(r.x).toBeCloseTo(400);
    expect(r.y).toBeCloseTo(0);
  });
  it("is empty before the video size is known", () => {
    expect(containRect(800, 600, 0, 0).w).toBe(0);
    expect(containRect(0, 0, 1280, 720).h).toBe(0);
  });
});

describe("mapBox", () => {
  it("maps frame pixels into the letterboxed rect", () => {
    const video = containRect(800, 600, 1280, 720); // scale 0.625, y offset 75
    const r = mapBox([640, 360, 1280, 720], 1280, 720, video);
    expect(r).toEqual({ x: 400, y: 75 + 225, w: 400, h: 225 });
  });
  it("maps into a pillarboxed rect", () => {
    const video = containRect(1600, 450, 1280, 720);
    const r = mapBox([0, 0, 1280, 720], 1280, 720, video);
    expect(r.x).toBeCloseTo(400);
    expect(r.w).toBeCloseTo(800);
  });
  it("clamps to the frame and fixes swapped corners", () => {
    const video = { x: 10, y: 20, w: 640, h: 360 };
    const r = mapBox([1400, 800, -50, -10], 1280, 720, video);
    expect(r).toEqual({ x: 10, y: 20, w: 640, h: 360 });
  });
});

describe("classColor", () => {
  it("differs by model and stable per class", () => {
    expect(classColor("ppe", 0)).not.toBe(classColor("idss", 0));
    expect(classColor("ppe", 1)).toBe(classColor("ppe", 1));
    expect(classColor("ppe", 1)).not.toBe(classColor("ppe", 2));
  });
});

type Handler = (e: { data: unknown }) => void;

describe("detections store", () => {
  let handlers: Handler[];
  beforeEach(() => {
    vi.resetModules();
    handlers = [];
    (globalThis as unknown as { chrome: unknown }).chrome = {
      webview: {
        postMessage: () => {},
        addEventListener: (_t: string, h: Handler) => handlers.push(h),
      },
    };
  });
  afterEach(() => {
    delete (globalThis as unknown as { chrome?: unknown }).chrome;
  });

  const det = (model: string, n = 1) => ({
    ev: "detections",
    cam: "CAM-01",
    model,
    ts: 1,
    w: 1280,
    h: 720,
    ms: 12,
    items: Array.from({ length: n }, (_, i) => ({
      cls: i,
      label: "helmet",
      conf: 0.87,
      box: [1, 2, 3, 4],
    })),
  });

  it("is fed by native events and keeps models apart", async () => {
    const store = await import("@/lib/detections");
    const { renderHook, act } = await import("@testing-library/react");
    const { result } = renderHook(() => store.useDetections("CAM-01"));
    expect(result.current.frames.ppe).toBeUndefined();
    expect(handlers.length).toBeGreaterThan(0);
    act(() => handlers.forEach((h) => h({ data: det("ppe", 2) })));
    act(() => handlers.forEach((h) => h({ data: det("idss", 1) })));
    act(() =>
      handlers.forEach((h) =>
        h({
          data: {
            ev: "inference",
            cam: "CAM-01",
            model: "ppe",
            fps: 5,
            avg_ms: 42,
            conf: 0.25,
            iou: 0.45,
          },
        }),
      ),
    );
    expect(result.current.frames.ppe?.items).toHaveLength(2);
    expect(result.current.frames.ppe?.items[0]).toMatchObject({
      label: "helmet",
      conf: 0.87,
      box: [1, 2, 3, 4],
    });
    expect(result.current.frames.idss?.items).toHaveLength(1);
    expect(result.current.stats.ppe).toMatchObject({ fps: 5, avgMs: 42, conf: 0.25, iou: 0.45 });
    expect(result.current.stats.idss).toBeUndefined();
    const other = renderHook(() => store.useDetections("CAM-02"));
    expect(other.result.current.frames).toEqual({});
  });

  it("ignores malformed events", async () => {
    const store = await import("@/lib/detections");
    store.ingest({ ev: "detections", cam: "CAM-01", model: "other" });
    store.ingest({ ev: "detections", model: "ppe" });
    store.ingest({
      ev: "detections",
      cam: "CAM-01",
      model: "ppe",
      items: [{ cls: 1, box: [1] }, null],
    });
    const { renderHook } = await import("@testing-library/react");
    const { result } = renderHook(() => store.useDetections("CAM-01"));
    expect(result.current.frames.ppe?.items).toEqual([]);
  });

  it("expires ppe after 1.5 s and idss after 4 s", async () => {
    const store = await import("@/lib/detections");
    store.ingest(det("ppe") as never, 10_000);
    store.ingest(det("idss") as never, 10_000);
    const { renderHook } = await import("@testing-library/react");
    const { result } = renderHook(() => store.useDetections("CAM-01"));
    const ppe = result.current.frames.ppe!;
    const idss = result.current.frames.idss!;
    expect(store.isStale(ppe, 11_400)).toBe(false);
    expect(store.isStale(ppe, 11_600)).toBe(true);
    expect(store.isStale(idss, 13_900)).toBe(false);
    expect(store.isStale(idss, 14_100)).toBe(true);
  });

  it("remembers overlay toggles in localStorage", async () => {
    // Own storage: Node 25 defines a global localStorage that is not functional without --localstorage-file.
    const backing = new Map<string, string>();
    vi.stubGlobal("localStorage", {
      getItem: (k: string) => backing.get(k) ?? null,
      setItem: (k: string, v: string) => void backing.set(k, v),
      removeItem: (k: string) => void backing.delete(k),
    });
    try {
      const store = await import("@/lib/detections");
      store.setOverlay("idss", false);
      expect(JSON.parse(backing.get("rigmind.overlay.prefs")!)).toEqual({
        ppe: true,
        idss: false,
      });
    } finally {
      vi.unstubAllGlobals();
    }
  });
});

describe("roi, in_roi and alarm", () => {
  let handlers: Handler[];
  beforeEach(() => {
    vi.resetModules();
    handlers = [];
    (globalThis as unknown as { chrome: unknown }).chrome = {
      webview: {
        postMessage: () => {},
        addEventListener: (_t: string, h: Handler) => handlers.push(h),
      },
    };
  });
  afterEach(() => {
    delete (globalThis as unknown as { chrome?: unknown }).chrome;
  });

  it("parses roi and per-item in_roi (default true)", async () => {
    const store = await import("@/lib/detections");
    store.ingest({
      ev: "detections",
      cam: "C",
      model: "idss",
      w: 1280,
      h: 720,
      roi: [100, 50, 900, 700],
      items: [
        { cls: 0, label: "Derrickman", conf: 0.9, box: [1, 2, 3, 4], in_roi: false },
        { cls: 1, label: "Platform", conf: 0.8, box: [1, 2, 3, 4] },
      ],
    });
    store.ingest({
      ev: "detections",
      cam: "C",
      model: "ppe",
      w: 1280,
      h: 720,
      roi: null,
      items: [],
    });
    const { renderHook } = await import("@testing-library/react");
    const { result } = renderHook(() => store.useDetections("C"));
    expect(result.current.frames.idss?.roi).toEqual([100, 50, 900, 700]);
    expect(result.current.frames.idss?.items.map((i) => i.inRoi)).toEqual([false, true]);
    expect(result.current.frames.ppe?.roi).toBeNull();
  });

  it("maps the roi with the same letterbox mapping", () => {
    const video = containRect(800, 600, 1280, 720);
    const r = mapBox([0, 0, 640, 360], 1280, 720, video);
    expect(r).toEqual({ x: 0, y: 75, w: 400, h: 225 });
  });

  it("recognises the derrickman class case-insensitively", async () => {
    const { isDerrickman } = await import("@/lib/detections");
    expect(isDerrickman("Derrickman")).toBe(true);
    expect(isDerrickman("DERRICK_MAN")).toBe(true);
    expect(isDerrickman("helmet")).toBe(false);
  });

  it("tracks alarm state and clears on None", async () => {
    const store = await import("@/lib/detections");
    const { alarmText } = await import("@/components/rig/DetectionControls");
    const { renderHook, act } = await import("@testing-library/react");
    const { result } = renderHook(() => store.useDetections("C"));
    act(() =>
      store.ingest({
        ev: "alarm",
        cam: "C",
        state: "Yellow",
        condition: "Near edge",
        place: "Derrick board",
      }),
    );
    expect(result.current.alarm?.state).toBe("Yellow");
    act(() =>
      store.ingest({
        ev: "alarm",
        cam: "C",
        state: "Red",
        previous: "Yellow",
        severity: "Critical",
        condition: "Derrickman out of safe zone",
        place: "Derrick board",
      }),
    );
    expect(alarmText(result.current.alarm!)).toBe(
      "Red · Derrickman out of safe zone · Derrick board",
    );
    expect(alarmText({ state: "Yellow", condition: "Near edge", place: "Derrick board" })).toBe("Monitoring active");
    act(() => store.ingest({ ev: "alarm", cam: "C", state: "None", previous: "Red" }));
    expect(result.current.alarm).toBeUndefined();
    act(() => store.ingest({ ev: "alarm", cam: "C", state: "bogus" }));
    expect(result.current.alarm).toBeUndefined();
  });
});

describe("platform", () => {
  const video = containRect(800, 600, 1280, 720); // x 0, y 75, scale 0.625
  const frame = (platform: unknown) => ({
    model: "idss" as const,
    w: 1280,
    h: 720,
    ms: 1,
    ts: 1,
    receivedAt: 1,
    roi: null,
    items: [],
    platform: platform as never,
  });
  const recorder = () => {
    const calls: string[] = [];
    const ctx = new Proxy({} as Record<string, unknown>, {
      get: (t, k: string) =>
        typeof k === "string" && !(k in t)
          ? (...a: unknown[]) => calls.push(`${k}(${a.join(",")})`)
          : t[k],
      set: (t, k: string, v) => {
        t[k] = v;
        if (k !== "font") calls.push(`${k}=${String(v)}`);
        return true;
      },
    });
    return { ctx: ctx as unknown as CanvasRenderingContext2D, calls };
  };

  it("parses platform: null, empty outline, contours", async () => {
    const { parsePlatform } = await import("@/lib/detections");
    expect(parsePlatform(null)).toBeNull();
    expect(parsePlatform({ outline: [] })).toBeNull();
    expect(parsePlatform({ box: [1, 2, 3, 4], outline: [] })).toEqual({
      box: [1, 2, 3, 4],
      outline: [],
    });
    expect(
      parsePlatform({
        box: [1, 2, 3, 4],
        outline: [
          [
            [0, 0],
            [10, 0],
            [10, 10],
          ],
          [[1, 1]],
        ],
      })?.outline,
    ).toEqual([
      [
        [0, 0],
        [10, 0],
        [10, 10],
      ],
    ]);
  });

  it("maps contour points with the letterbox mapping", () => {
    expect(mapPoint([640, 360], 1280, 720, video)).toEqual([400, 300]);
    expect(mapPoint([0, 0], 1280, 720, video)).toEqual([0, 75]);
  });

  it("draws nothing for null platform and box only for empty outline", async () => {
    const { drawPlatform } = await import("@/components/rig/LiveDetections");
    const a = recorder();
    drawPlatform(a.ctx, frame(null), video);
    expect(a.calls).toEqual([]);
    const b = recorder();
    drawPlatform(b.ctx, frame({ box: [0, 0, 1280, 720], outline: [] }), video);
    expect(b.calls.some((c) => c.startsWith("strokeRect(0,75,800,450)"))).toBe(true);
    expect(b.calls.some((c) => c.startsWith("beginPath"))).toBe(false);
  });

  it("draws the box first, then closed contours, green 2px", async () => {
    const { drawPlatform } = await import("@/components/rig/LiveDetections");
    const { ctx, calls } = recorder();
    drawPlatform(
      ctx,
      frame({
        box: [0, 0, 1280, 720],
        outline: [
          [
            [0, 0],
            [1280, 0],
            [640, 360],
          ],
        ],
      }),
      video,
    );
    expect(calls).toContain("strokeStyle=#00ff00");
    expect(calls).toContain("lineWidth=2");
    const iRect = calls.findIndex((c) => c.startsWith("strokeRect"));
    const iBegin = calls.findIndex((c) => c.startsWith("beginPath"));
    const iClose = calls.findIndex((c) => c.startsWith("closePath"));
    const iStroke = calls.findIndex((c, i) => i > iClose && c.startsWith("stroke("));
    expect(iRect).toBeGreaterThanOrEqual(0);
    expect(iRect).toBeLessThan(iBegin);
    expect(iBegin).toBeLessThan(iClose);
    expect(iClose).toBeLessThan(iStroke);
    expect(calls).toContain("moveTo(0,75)");
    expect(calls).toContain("lineTo(800,75)");
    expect(calls).toContain("lineTo(400,300)");
  });
});

describe("no text on the frame", () => {
  it("the canvas draws no text at all: no labels, no confidence, no iou", async () => {
    const { drawFrame } = await import("@/components/rig/LiveDetections");
    const texts: string[] = [];
    const ctx = new Proxy({} as Record<string, unknown>, {
      get: (t, k: string) =>
        k === "measureText"
          ? () => ({ width: 10 })
          : k === "fillText"
            ? (s: string) => texts.push(s)
            : k in t
              ? t[k]
              : () => {},
      set: (t, k: string, v) => ((t[k] = v), true),
    }) as unknown as CanvasRenderingContext2D;
    drawFrame(
      ctx,
      {
        model: "ppe",
        w: 1280,
        h: 720,
        ms: 1,
        ts: 1,
        receivedAt: 1,
        roi: null,
        platform: null,
        items: [
          { cls: 0, label: "helmet", conf: 0.87, box: [10, 10, 100, 100], inRoi: true },
          { cls: 1, label: "Derrickman", conf: 0.5, box: [10, 10, 100, 100], inRoi: false },
        ],
      },
      { x: 0, y: 0, w: 1280, h: 720 },
    );
    expect(texts).toEqual([]);
  });
});

describe("segmentation masks", () => {
  const video = { x: 0, y: 75, w: 800, h: 450 };
  const run = async (items: unknown[]) => {
    const { drawFrame } = await import("@/components/rig/LiveDetections");
    const calls: string[] = [];
    const ctx = new Proxy({} as Record<string, unknown>, {
      get: (t, k: string) =>
        k === "measureText"
          ? () => ({ width: 10 })
          : k in t
            ? t[k]
            : (...a: unknown[]) => calls.push(`${k}(${a.join(",")})`),
      set: (t, k: string, v) => {
        t[k] = v;
        if (k !== "font") calls.push(`${k}=${String(v)}`);
        return true;
      },
    }) as unknown as CanvasRenderingContext2D;
    drawFrame(
      ctx,
      {
        model: "idss",
        w: 1280,
        h: 720,
        ms: 1,
        ts: 1,
        receivedAt: 1,
        roi: null,
        platform: null,
        items: items as never,
      },
      video,
    );
    return calls;
  };
  const poly: [number, number][] = [
    [0, 0],
    [1280, 0],
    [640, 360],
  ];

  it("parses polygons and drops degenerate ones", async () => {
    const { parsePolygon } = await import("@/lib/detections");
    expect(parsePolygon(poly)).toEqual(poly);
    expect(
      parsePolygon([
        [1, 2],
        [3, 4],
      ]),
    ).toBeUndefined();
    expect(parsePolygon(undefined)).toBeUndefined();
  });

  it("maps polygon points with the letterbox mapping and fills before the box", async () => {
    const calls = await run([
      { cls: 1, label: "Derrickman", conf: 1, box: [0, 0, 640, 360], inRoi: true, polygon: poly },
    ]);
    expect(calls).toContain("moveTo(0,75)");
    expect(calls).toContain("lineTo(800,75)");
    expect(calls).toContain("lineTo(400,300)");
    expect(calls).toContain("globalAlpha=0.25");
    const iFill = calls.findIndex((c) => c.startsWith("fill("));
    const iBox = calls.findIndex((c) => c.startsWith("strokeRect"));
    expect(iFill).toBeGreaterThanOrEqual(0);
    expect(iFill).toBeLessThan(iBox);
    expect(calls.some((c) => c.startsWith("fillText"))).toBe(false);
  });

  it("does not fill the platform polygon and keeps outside items dashed", async () => {
    const plat = await run([
      { cls: 0, label: "Platform", conf: 1, box: [0, 0, 100, 100], inRoi: true, polygon: poly },
    ]);
    expect(plat.some((c) => c.startsWith("fill("))).toBe(false);
    expect(plat).toContain("strokeStyle=#00ff00");
    const out = await run([
      { cls: 1, label: "Derrickman", conf: 1, box: [0, 0, 100, 100], inRoi: false, polygon: poly },
    ]);
    expect(out).toContain("setLineDash(7,4)");
    expect(out).toContain("strokeStyle=#ff1f2d");
  });

  it("draws items without a polygon as before", async () => {
    const calls = await run([
      { cls: 1, label: "helmet", conf: 1, box: [0, 0, 100, 100], inRoi: true },
    ]);
    expect(calls.some((c) => c.startsWith("fill("))).toBe(false);
    expect(calls.some((c) => c.startsWith("strokeRect"))).toBe(true);
  });

  it("alarm chip colours: yellow, orange and red are distinct", async () => {
    const { alarmStyle } = await import("@/components/rig/DetectionControls");
    const bg = ["Yellow", "Orange", "Red"].map((s) => alarmStyle(s).background);
    expect(new Set(bg).size).toBe(3);
    expect(alarmStyle("Red").background).toBe("#dc2626");
  });
});
