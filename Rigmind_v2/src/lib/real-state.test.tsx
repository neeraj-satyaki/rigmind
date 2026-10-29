import { act, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({ transitions: [] as any[], feed: [] as any[] }));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  onNativeEvent: () => () => {},
  nativeCall: async (method: string) => {
    if (method === "events.recent") return h.transitions;
    if (method === "alerts.feed") return h.feed;
    return {};
  },
}));
vi.mock("@/lib/device-db", () => ({ readDevice: async () => undefined, writeDevice: async () => {}, listDevice: async () => [] }));
vi.mock("sonner", () => ({ toast: Object.assign(() => {}, { error() {}, success() {}, warning() {} }) }));
vi.mock("@tanstack/react-router", () => ({
  Link: ({ children }: any) => <a>{children}</a>,
  useNavigate: () => () => {},
}));

import { AppState, useAppState } from "@/components/rig/AppState";
import { RealEventHistory } from "@/components/rig/RealEvents";
import { demoDataEnabled } from "@/lib/demo-mode";
import { eventsFromTransitions } from "@/lib/real-state";

const row = (id: number, ts: number, from: string, to: string, extra: object = {}) => ({
  id, timestampMs: ts, from, to, camId: "CAM-01", alertId: "ALM-1", condition: "Person in zone", place: "Rig A", frame: 1, severity: "Critical", ...extra,
});

function Probe() {
  const { items } = useAppState();
  return <div data-testid="n">{items.length}</div>;
}

describe("sample data off", () => {
  beforeEach(() => {
    (globalThis as any).__RIGMIND_CONFIG__ = { demoData: false };
    h.transitions = [];
    h.feed = [];
  });
  afterEach(() => {
    delete (globalThis as any).__RIGMIND_CONFIG__;
  });

  it("turns the config flag into demoDataEnabled()", () => {
    expect(demoDataEnabled()).toBe(false);
  });

  it("groups host transitions into one event per alarm episode", () => {
    const ev = eventsFromTransitions([
      row(3, 3000, "Red", "None"),
      row(2, 2000, "Yellow", "Red"),
      row(1, 1000, "None", "Yellow", { severity: "Advisory" }),
      row(4, 9000, "None", "Yellow", { alertId: "ALM-2", severity: "Advisory", condition: "Other" }),
    ]);
    expect(ev.map((e) => e.id)).toEqual(["ALM-2", "ALM-1"]);
    const e1 = ev[1]!;
    expect(e1.severity).toBe("Critical");
    expect(e1.cleared).toBe(true);
    expect(e1.steps).toHaveLength(3);
    expect(e1.startMs).toBe(1000);
    expect(ev[0]!.cleared).toBe(false);
  });

  it("starts with no alerts instead of the sample alerts", async () => {
    await act(async () => {
      render(
        <AppState>
          <Probe />
        </AppState>,
      );
    });
    expect(screen.getByTestId("n").textContent).toBe("0");
  });

  it("shows an honest empty state in the event history", async () => {
    await act(async () => {
      render(
        <AppState>
          <RealEventHistory />
        </AppState>,
      );
    });
    expect(screen.getByText("No events recorded.")).toBeTruthy();
    expect(screen.queryByText("EV-1042")).toBeNull();
    expect(screen.queryByText(/illustrative/)).toBeNull();
  });

  it("lists events the host recorded", async () => {
    h.transitions = [row(2, 2000, "Yellow", "Red"), row(1, 1000, "None", "Yellow", { severity: "Advisory" })];
    await act(async () => {
      render(
        <AppState>
          <RealEventHistory />
        </AppState>,
      );
    });
    expect(screen.getByText("ALM-1")).toBeTruthy();
    expect(screen.getByText("Person in zone")).toBeTruthy();
    expect(screen.queryByText("EV-1042")).toBeNull();
  });
});
