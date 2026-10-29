import { act, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({
  listeners: new Set<(e: any) => void>(),
  calls: [] as { method: string; params: any }[],
  toasts: [] as string[],
  tones: [] as string[],
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
vi.mock("@/lib/alert-sound", () => ({ playAlertSound: (s: string) => h.tones.push(s) }));
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