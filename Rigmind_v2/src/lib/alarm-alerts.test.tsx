import { act, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({
  listeners: new Set<(e: any) => void>(),
  calls: [] as { method: string; params: any }[],
  toasts: [] as string[],
  tones: [] as string[],
  loops: [] as [string, string | null][],
  os: [] as string[],
  native: false, // the desktop host raises the Windows notification itself; the page does it only outside the host
}));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => h.native,
  onNativeEvent: (l: (e: any) => void) => (h.listeners.add(l), () => h.listeners.delete(l)),
  nativeCall: async (method: string, params: any) => {
    h.calls.push({ method, params });
    return method === "event.describe" ? { text: "template text", slm: false } : {};
  },
}));
vi.mock("@/lib/alert-sound", () => ({ playAlertSound: (s: string) => h.tones.push(s), setAlertLoop: (k: string, s: string | null) => h.loops.push([k, s]) }));
vi.mock("@/lib/device-db", () => ({ listDevice: async () => [], writeDevice: async () => {} }));
vi.mock("sonner", () => {
  const t: any = (m: string) => h.toasts.push(m);
  t.error = t.warning = t.success = (m: string) => h.toasts.push(m);
  return { toast: t };
});
vi.mock("@tanstack/react-router", () => ({
  Link: ({ children }: any) => <a>{children}</a>,
  useNavigate: () => () => {},
}));

import { Notifications } from "@/components/rig/NavTools";
import { resetAlarmAlerts, startAlarmAlerts } from "@/lib/alarm-alerts";
import { LOGO_ALERT_MS } from "@/lib/alert-bus";
import { alerts as base } from "@/data/mock";

type Host = { alertId: string; episode: number; phase: string; escalation: number };
// Host-enriched line (what main.cpp post_line forwards). `host` undefined = plain browser / older host.
const send = (state: string, cam = "CAM-01", host?: Host) =>
  h.listeners.forEach((l) =>
    l({ ev: "alarm", cam, state, previous: "x", severity: "s", condition: "Derrickman out of safe zone", place: "Demo rig 01 · Derrick board", frame: 1, ...host }),
  );
const step = async (state: string, cam?: string, host?: Host) => {
  await act(async () => {
    send(state, cam, host);
    await vi.advanceTimersByTimeAsync(LOGO_ALERT_MS + 50);
  });
};
const hostFor = (state: string, ep = 1, cam = "CAM-01"): Host => ({
  alertId: `ALM-${cam}-b${ep}`,
  episode: ep,
  phase: state === "None" ? "Cleared" : state === "Yellow" ? "Monitoring" : "Escalated",
  escalation: state === "Orange" ? 1 : state === "Red" ? 2 : 0,
});
const raises = () => h.calls.filter((c) => c.method === "alert.raise").map((c) => c.params);
const badge = () => screen.getByLabelText(/unread alerts/).textContent;
const rows = () => screen.queryAllByText(/^ALM-/); // one id label per bell row (the popover is open)
const mount = () => {
  const r = render(<Notifications />);
  fireEvent.click(screen.getByLabelText("Notifications"));
  return r;
};
const bellText = () => document.body.textContent ?? "";

describe("alarm episode -> alert bridge", () => {
  let stop: () => void;
  beforeEach(() => {
    vi.useFakeTimers();
    h.listeners.clear();
    h.calls.length = h.toasts.length = h.tones.length = h.os.length = 0;
    h.native = false;
    (globalThis as any).Notification = class {
      static permission = "granted";
      constructor(title: string) {
        h.os.push(title);
      }
    };
    resetAlarmAlerts();
    stop = startAlarmAlerts();
  });
  afterEach(() => {
    stop();
    delete (globalThis as any).Notification;
    vi.useRealTimers();
  });

  for (const mode of ["host fields", "local fallback"] as const) {
    const host = (s: string, ep = 1, cam = "CAM-01") => (mode === "host fields" ? hostFor(s, ep, cam) : undefined);

    describe(mode, () => {
      it("Yellow is quiet; Orange toasts on the same item; Red adds red tone, OS notification and description; None clears", async () => {
        mount();
        const n0 = base.length;
        await step("None", "CAM-01");
        await step("Yellow", "CAM-01", host("Yellow"));
        expect(h.toasts).toHaveLength(0); // no toast
        expect(h.tones).toHaveLength(0); // no tone
        expect(badge()).toBe(String(n0)); // no bell increment
        expect(h.os).toHaveLength(0);
        expect(rows()).toHaveLength(0);

        await step("Yellow", "CAM-01", host("Yellow")); // repeat
        expect(h.toasts).toHaveLength(0);

        await step("Orange", "CAM-01", host("Orange"));
        expect(h.toasts.filter((t) => t.endsWith("safety event"))).toHaveLength(1);
        expect(badge()).toBe(String(n0 + 1));
        expect(h.tones).toEqual(["Warning"]);
        expect(rows()).toHaveLength(1);
        expect(h.os).toHaveLength(0);
        expect(h.calls.filter((c) => c.method === "event.describe")).toHaveLength(0);

        await step("Red", "CAM-01", host("Red"));
        expect(badge()).toBe(String(n0 + 2));
        expect(rows()).toHaveLength(1); // same item updated in place, no second row
        expect(bellText()).toContain("Critical");
        expect(h.tones).toEqual(["Warning", "Critical"]);
        expect(h.os).toHaveLength(1);
        expect(h.calls.filter((c) => c.method === "event.describe")).toHaveLength(1);

        await step("Red", "CAM-01", host("Red")); // repeat changes nothing
        expect(badge()).toBe(String(n0 + 2));
        expect(h.tones).toHaveLength(2);

        await step("None", "CAM-01", host("None"));
        expect(rows()).toHaveLength(1);
        expect(bellText()).toContain("Cleared"); // stays in history, quietly
        expect(badge()).toBe(String(n0 + 2));
        expect(h.toasts.filter((t) => t.endsWith("safety event"))).toHaveLength(2);
        expect(raises()).toHaveLength(0); // the host already owns the episode row; the page writes none
        await act(async () => {
          await vi.advanceTimersByTimeAsync(10);
        });
        expect(h.toasts.filter((t) => t.includes("in plain words"))).toHaveLength(1);
      });

      it("a second episode opens a new item; Yellow->Red skip; cameras are independent", async () => {
        mount();
        await step("Yellow", "CAM-01", host("Yellow"));
        await step("Orange", "CAM-01", host("Orange"));
        await step("None", "CAM-01", host("None"));
        await step("Yellow", "CAM-01", host("Yellow", 2));
        await step("Orange", "CAM-01", host("Orange", 2));
        expect(rows()).toHaveLength(2);
        await step("Yellow", "CAM-02", host("Yellow", 1, "CAM-02"));
        await step("Red", "CAM-02", host("Red", 1, "CAM-02"));
        await step("Red", "CAM-02", host("Red", 1, "CAM-02"));
        expect(rows()).toHaveLength(3);
        expect(h.tones).toEqual(["Warning", "Warning", "Critical"]);
        expect(new Set(rows().map((r) => r.textContent)).size).toBe(3);
      });

      it("None->Red straight away presents Critical once", async () => {
        mount();
        await step("Red", "CAM-01", host("Red"));
        await step("Red", "CAM-01", host("Red"));
        expect(h.tones).toEqual(["Critical"]);
        expect(h.os).toHaveLength(1);
        expect(rows()).toHaveLength(1);
      });
    });
  }

  it("ignores non-alarm events, unknown states and a lone None", async () => {
    mount();
    h.listeners.forEach((l) => l({ ev: "clip", cam: "CAM-01" }));
    h.listeners.forEach((l) => l({ ev: "alarm", cam: "CAM-01", state: "Purple" }));
    h.listeners.forEach((l) => l({ ev: "alarm", state: "Red" }));
    await step("None", "CAM-09");
    expect(h.toasts).toHaveLength(0);
    expect(rows()).toHaveLength(0);
  });
});
describe("event cycles (host closed field)", () => {
  let stop: () => void;
  beforeEach(() => {
    vi.useFakeTimers();
    h.listeners.clear();
    h.calls.length = h.toasts.length = h.tones.length = h.os.length = 0;
    h.native = true;
    resetAlarmAlerts();
    stop = startAlarmAlerts();
  });
  afterEach(() => {
    stop();
    vi.useRealTimers();
  });

  const line = (state: string, alertId: string, closed?: object) =>
    h.listeners.forEach((l) =>
      l({ ev: "alarm", cam: "CAM-01", state, condition: "Elevator near platform", place: "Demo rig 01 · Derrick board", alertId, phase: "Monitoring", escalation: 0, closed }),
    );
  const run = async (fn: () => void) => {
    await act(async () => {
      fn();
      await vi.advanceTimersByTimeAsync(LOGO_ALERT_MS + 50);
    });
  };
  const describes = () => h.calls.filter((c) => c.method === "event.describe").map((c) => c.params);

  it("Yellow>Orange>Yellow closes a normal cycle and summarises it; the new Yellow is quiet", async () => {
    await run(() => line("Yellow", "ALM-CAM-01-a1"));
    await run(() => line("Orange", "ALM-CAM-01-a1"));
    expect(describes()).toHaveLength(0);
    await run(() => line("Yellow", "ALM-CAM-01-a2", { alertId: "ALM-CAM-01-a1", peak: 1, critical: false, cycle: "Y>O>Y", kind: "normal" }));
    expect(describes()).toHaveLength(1);
    expect(describes()[0]).toMatchObject({ alertId: "ALM-CAM-01-a1", severity: "Warning" });
    expect(describes()[0].condition).toContain("Normal event cycle (Yellow, Orange, Yellow)");
    await run(() => line("Yellow", "ALM-CAM-01-a2")); // repeat
    expect(describes()).toHaveLength(1);
  });

  it("Yellow>Orange>Red>Orange>Yellow closes a critical cycle; Red still describes at once", async () => {
    await run(() => line("Orange", "ALM-CAM-01-b1"));
    await run(() => line("Red", "ALM-CAM-01-b1"));
    expect(describes()).toHaveLength(1); // at the Red moment
    await run(() => line("Orange", "ALM-CAM-01-b1"));
    await run(() => line("Yellow", "ALM-CAM-01-b2", { alertId: "ALM-CAM-01-b1", peak: 2, critical: true, cycle: "Y>O>R>O>Y", kind: "critical" }));
    expect(describes()).toHaveLength(2);
    expect(describes()[1]).toMatchObject({ alertId: "ALM-CAM-01-b1", severity: "Critical" });
    expect(describes()[1].condition).toContain("Critical event cycle (Yellow, Orange, Red, Orange, Yellow)");
  });

  it("a Yellow-only stretch is cleared without a summary", async () => {
    await run(() => line("Yellow", "ALM-CAM-01-c1"));
    await run(() => line("None", "ALM-CAM-01-c1", { alertId: "ALM-CAM-01-c1", peak: 0, critical: false, cycle: "Y>N", kind: "normal" }));
    expect(describes()).toHaveLength(0);
  });
});

describe("looping alarm tone", () => {
  let stop: () => void;
  beforeEach(() => {
    vi.useFakeTimers();
    h.listeners.clear();
    h.loops.length = 0;
    h.native = true;
    resetAlarmAlerts();
    stop = startAlarmAlerts();
  });
  afterEach(() => {
    stop();
    vi.useRealTimers();
  });
  const line = (state: string, cam = "CAM-01") =>
    h.listeners.forEach((l) => l({ ev: "alarm", cam, state, condition: "x", place: "p", alertId: `ALM-${cam}-a1` }));

  it("loops on Orange, escalates to the Red tone, and stops when the alarm is back to Yellow", () => {
    line("Yellow");
    expect(h.loops.at(-1)).toEqual(["CAM-01", null]); // Yellow: silent
    line("Orange");
    expect(h.loops.at(-1)).toEqual(["CAM-01", "Warning"]);
    line("Red");
    expect(h.loops.at(-1)).toEqual(["CAM-01", "Critical"]);
    line("Orange");
    expect(h.loops.at(-1)).toEqual(["CAM-01", "Warning"]);
    line("Yellow");
    expect(h.loops.at(-1)).toEqual(["CAM-01", null]);
  });
  it("stops when the alarm clears to None, and when the camera goes offline", () => {
    line("Red");
    line("None");
    expect(h.loops.at(-1)).toEqual(["CAM-01", null]);
    line("Orange", "CAM-02");
    h.listeners.forEach((l) => l({ ev: "status", cam: "CAM-02", state: "offline" }));
    expect(h.loops.at(-1)).toEqual(["CAM-02", null]);
  });
});
