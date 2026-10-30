import { act, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({
  listeners: new Set<(e: any) => void>(),
  calls: [] as { method: string; params: any }[],
  toasts: [] as { title: string; opts: any }[],
  tones: [] as string[],
}));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  onNativeEvent: (l: (e: any) => void) => (h.listeners.add(l), () => h.listeners.delete(l)),
  nativeCall: async (method: string, params: any) => {
    h.calls.push({ method, params });
    return {};
  },
}));
// The real tone map has no sound for Advisory; the mock keeps that rule so a tone for a PPE alert would show up here.
vi.mock("@/lib/alert-sound", () => ({ playAlertSound: (s: string) => s !== "Advisory" && h.tones.push(s), setAlertLoop: () => {} }));
vi.mock("@/lib/device-db", () => ({ listDevice: async () => [], writeDevice: async () => {} }));
vi.mock("sonner", () => {
  const t: any = (title: string, opts: any) => h.toasts.push({ title, opts });
  t.error = t.warning = t.success = () => {};
  return { toast: t };
});
vi.mock("@tanstack/react-router", () => ({
  Link: ({ children }: any) => <a>{children}</a>,
  useNavigate: () => () => {},
}));

import { Notifications } from "@/components/rig/NavTools";
import { handlePpeEvent, resetPpeAlerts, startPpeAlerts } from "@/lib/ppe-alerts";
import { LOGO_ALERT_MS } from "@/lib/alert-bus";
import { alertToastOptions } from "@/lib/alert-colors";
import * as store from "@/lib/detections";

const send = (state: string, missing: string[], extra: Record<string, unknown> = {}) =>
  h.listeners.forEach((l) =>
    l({ ev: "ppe", cam: "CAM-01", state, missing, persons: 1, since_ms: 1, alertId: "PPE-CAM-01-b11", phase: state === "ok" ? "Cleared" : "Open", ...extra }),
  );
const step = async (state: string, missing: string[], extra?: Record<string, unknown>) => {
  await act(async () => {
    send(state, missing, extra);
    await vi.advanceTimersByTimeAsync(LOGO_ALERT_MS + 50);
  });
};
const text = () => document.body.textContent ?? "";

describe("PPE compliance events -> alerts", () => {
  let stop: () => void;
  beforeEach(() => {
    vi.useFakeTimers();
    h.listeners.clear();
    h.calls.length = h.toasts.length = h.tones.length = 0;
    resetPpeAlerts();
    stop = startPpeAlerts();
  });
  afterEach(() => {
    stop();
    vi.useRealTimers();
  });

  it("raises one yellow Advisory toast per episode, no tone, a bell row; updates in place; clears", async () => {
    render(<Notifications />);
    fireEvent.click(screen.getByLabelText("Notifications"));
    await step("missing", ["vest"]);
    expect(h.toasts).toHaveLength(1);
    expect(h.toasts[0]!.title).toContain("Advisory");
    expect(h.toasts[0]!.opts.description).toContain("PPE not detected: vest");
    expect(h.toasts[0]!.opts.description).not.toMatch(/not worn/i);
    // theme-coloured, slightly blurred toast with thick yellow text
    const glass = alertToastOptions("Advisory").style;
    expect(h.toasts[0]!.opts.style).toMatchObject({ color: glass.color, backdropFilter: expect.stringContaining("blur") });
    expect(String((h.toasts[0]!.opts.style as { background: string }).background)).toBe("var(--toast-bg)");
    expect(glass.color).toBe("var(--toast-yellow)");
    expect(h.tones).toHaveLength(0); // quiet
    expect(h.calls.filter((c) => c.method === "alert.raise")).toHaveLength(0); // the host stored the row
    expect(text()).toContain("PPE-CAM-01-b11");
    expect(text()).toContain("PPE not detected: vest");

    // repeat and set change: no new toast, the same bell row changes
    await step("missing", ["vest"]);
    await step("missing", ["vest", "gloves"], { condition: "PPE not detected: vest, gloves" });
    expect(h.toasts).toHaveLength(1);
    expect(text()).toContain("PPE not detected: vest, gloves");
    expect(screen.queryAllByText(/^PPE-/)).toHaveLength(1);

    await step("ok", []);
    expect(h.toasts).toHaveLength(1);
    expect(text()).toContain("Cleared");
    await step("ok", []); // a repeat does nothing
    expect(screen.queryAllByText(/^PPE-/)).toHaveLength(1);

    // the next episode (new host id) toasts again
    await step("missing", ["helmet"], { alertId: "PPE-CAM-01-b12" });
    expect(h.toasts).toHaveLength(2);
    expect(h.tones).toHaveLength(0);
  });

  it("an ok with no episode, a bad state and an empty missing list do nothing", () => {
    expect(handlePpeEvent({ ev: "ppe", cam: "CAM-01", state: "ok", missing: [] } as any)).toBeUndefined();
    expect(handlePpeEvent({ ev: "ppe", cam: "CAM-01", state: "weird" } as any)).toBeUndefined();
    expect(handlePpeEvent({ ev: "ppe", cam: "CAM-01", state: "missing", missing: [] } as any)).toBeUndefined();
    expect(handlePpeEvent({ ev: "alarm", cam: "CAM-01", state: "Yellow" } as any)).toBeUndefined();
  });

  it("works without host fields (plain fallback id)", async () => {
    const a = handlePpeEvent({ ev: "ppe", cam: "CAM-01", state: "missing", missing: ["vest", "gloves"] } as any);
    expect(a).toMatchObject({ severity: "Advisory", condition: "PPE not detected: vest, gloves", place: "CAM-01", phase: "Open" });
    expect(a!.id.length).toBeLessThanOrEqual(32);
  });

  it("the Live store keeps the latest ppe state for the chip", () => {
    store.resetDetections();
    store.ingest({ ev: "ppe", cam: "CAM-01", state: "missing", missing: ["vest"], persons: 2 } as any, 1000);
    expect(store.__peek("CAM-01")?.ppe).toMatchObject({ state: "missing", missing: ["vest"], persons: 2 });
    store.ingest({ ev: "ppe", cam: "CAM-01", state: "ok", missing: [], persons: 1 } as any, 2000);
    expect(store.__peek("CAM-01")?.ppe?.state).toBe("ok");
  });
});
