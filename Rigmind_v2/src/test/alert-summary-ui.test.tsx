import { act, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({
  listeners: new Set<(e: any) => void>(),
  toasts: [] as { title: string; opts: any }[],
  row: null as any,
}));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  onNativeEvent: (l: (e: any) => void) => (h.listeners.add(l), () => h.listeners.delete(l)),
  nativeCall: async (method: string) => (method === "alert.get" ? h.row : method === "alerts.feed" ? [] : {}),
}));
vi.mock("@/lib/alert-sound", () => ({ playAlertSound: () => {}, setAlertLoop: () => {} }));
vi.mock("@/lib/device-db", () => ({ listDevice: async () => [], writeDevice: async () => {}, readDevice: async () => undefined }));
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
import { ALERT_RAISED, LOGO_ALERT_MS, type RaisedAlert } from "@/lib/alert-bus";

const SENTENCE = "Camera CAM-01 saw the elevator near the platform with the link tilt angled 28 degrees. Check the area.";
const meta = {
  kind: "alarm",
  camera: "CAM-01",
  peak: "Orange",
  cycle: "Y>O",
  closed: false,
  states: [{ state: "Orange", tsMs: 2, facts: { machine_present: true, link_tilt_angled: true, angle_deg: 28.4, elevator_near_platform: true } }],
};
const alert: RaisedAlert = {
  id: "ALM-CAM-01-b11",
  severity: "Warning",
  condition: "Orange alert",
  place: "CAM-01",
  phase: "Escalated",
  metadata: null,
  summary: null,
  summarySource: null,
};
const body = (i: number) => {
  const r = render(<>{h.toasts[i]!.opts.description}</>);
  const text = r.container.textContent ?? "";
  r.unmount();
  return text;
};

describe("alert toast shows the facts, then the SLM sentence in place", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    h.listeners.clear();
    h.toasts.length = 0;
    h.row = null;
  });
  afterEach(() => vi.useRealTimers());

  it("title 'Orange alert · CAM-01'; facts + 'Summary pending' first; the sentence updates the same toast id", async () => {
    render(<Notifications />);
    fireEvent.click(screen.getByLabelText("Notifications"));
    h.row = { ...alert, metadata: meta, summary: null, summarySource: null, summaryAt: null };
    await act(async () => {
      window.dispatchEvent(new CustomEvent(ALERT_RAISED, { detail: alert }));
      await vi.advanceTimersByTimeAsync(LOGO_ALERT_MS + 50);
    });
    // 1st: straight from the raise (nothing known yet beyond the label); 2nd: the facts read from the host row
    expect(h.toasts.length).toBeGreaterThanOrEqual(2);
    const facts = h.toasts.length - 1;
    expect(h.toasts[0]!.title).toBe("Orange alert · CAM-01");
    expect(h.toasts[facts]!.title).toBe("Orange alert · CAM-01");
    expect(body(facts)).toContain("machine present, link tilt angled 28°, elevator near platform");
    expect(body(facts)).toContain("Summary pending");
    expect(h.toasts[facts]!.opts.id).toBe("ALM-CAM-01-b11");

    // the host finishes the sentence: it is stored on the row, the host sends the description line
    h.row = { ...h.row, summary: SENTENCE, summarySource: "slm", summaryAt: 9 };
    const before = h.toasts.length;
    await act(async () => {
      h.listeners.forEach((l) => l({ ev: "description", alertId: "ALM-CAM-01-b11", text: SENTENCE, source: "slm" }));
      await vi.advanceTimersByTimeAsync(10);
    });
    expect(h.toasts.length).toBe(before + 1);
    const last = h.toasts.length - 1;
    expect(h.toasts[last]!.opts.id).toBe("ALM-CAM-01-b11"); // same sonner toast id: updated in place
    expect(body(last)).toBe(SENTENCE);
    expect(body(last)).not.toContain("Summary pending");
    // the bell row shows the sentence too
    expect(document.body.textContent).toContain(SENTENCE);
  });

  it("with no model the chip says 'BitNet not available'", async () => {
    render(<Notifications />);
    h.row = { ...alert, metadata: meta, summary: null, summarySource: "unavailable", summaryAt: 5 };
    await act(async () => {
      window.dispatchEvent(new CustomEvent(ALERT_RAISED, { detail: alert }));
      await vi.advanceTimersByTimeAsync(LOGO_ALERT_MS + 50);
    });
    expect(body(h.toasts.length - 1)).toContain("BitNet not available");
  });

  it("a description for an alert that was never toasted does not pop a toast", async () => {
    render(<Notifications />);
    h.row = { ...alert, summary: SENTENCE, summarySource: "slm" };
    await act(async () => {
      h.listeners.forEach((l) => l({ ev: "description", alertId: "ALM-CAM-09-z1", text: SENTENCE, source: "slm" }));
      await vi.advanceTimersByTimeAsync(10);
    });
    expect(h.toasts).toHaveLength(0);
  });
});
