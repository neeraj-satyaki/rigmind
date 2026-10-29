import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import type { ReactNode } from "react";

// In-memory stand-in for the host's records so the pages can save and reload what the user typed.
const store = new Map<string, unknown>();
vi.mock("@/lib/device-db", () => ({
  readDevice: async (k: string) => store.get(k),
  writeDevice: async (k: string, v: unknown) => void store.set(k, JSON.parse(JSON.stringify(v))),
  listDevice: async (prefix: string) => [...store.entries()].filter(([k]) => k.startsWith(prefix)).map(([key, value]) => ({ key, value })),
}));
vi.mock("@tanstack/react-router", async (orig) => ({
  ...(await orig<typeof import("@tanstack/react-router")>()),
  Link: ({ children }: { children?: ReactNode }) => <a>{children}</a>,
  useNavigate: () => () => {},
}));

import { SafetyAnalytics, EventOverview } from "@/components/rig/AnalyticsScreens";
import { ActionsHandover, Reports, UsersAccess } from "@/components/rig/CoordinationScreens";
import { AuditTrail, Integrations, LicenseCapacity, SafetyRules, SystemHealth } from "@/components/rig/GovernanceScreens";
import { ZoneEditor } from "@/components/rig/ZoneEditor";
import { ActionReport } from "@/reports/templates/action";
import { AuditReport } from "@/reports/templates/audit";
import { EventReport } from "@/reports/templates/event";
import { HandoverReport } from "@/reports/templates/handover";
import { HealthReport } from "@/reports/templates/health";
import { HseReport } from "@/reports/templates/hse";
import { PilotReport } from "@/reports/templates/pilot";
import { RegisterReport } from "@/reports/templates/register";
import { ShiftReport } from "@/reports/templates/shift";
import { WeeklyReport } from "@/reports/templates/weekly";
import { eventsFromAlerts, computeWeekly, availableWeeks } from "@/lib/weekly-events";
import { eventsByCondition, heatLevel, realHeatmap, realTrend, type LiveAlertRow } from "@/lib/live-analytics";

// Strings that only exist in the sample set. None of them may appear with sample data off.
const SAMPLE = [/EV-10\d\d/, /CA-028/, /Faisal/, /Maha S/, /Nasser/, /Omar S/, /Demo rig/, /RM-DEMO/, /DEMO-PLC/, /CAM-06/, /Derrick/i, /demonstration/i, /illustrative/i];
const noSample = () => {
  const text = document.body.textContent ?? "";
  for (const re of SAMPLE) expect(text, String(re)).not.toMatch(re);
};

beforeAll(() => {
  // recharts measures its container with ResizeObserver, which jsdom does not provide.
  (globalThis as unknown as { ResizeObserver: unknown }).ResizeObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  (globalThis as unknown as { __RIGMIND_CONFIG__: unknown }).__RIGMIND_CONFIG__ = { demoData: false };
});
afterAll(() => {
  delete (globalThis as unknown as { __RIGMIND_CONFIG__?: unknown }).__RIGMIND_CONFIG__;
});

const row = (id: string, severity: string, raisedMs: number, condition = "PPE review", status = "Open"): LiveAlertRow =>
  ({ id, severity, condition, place: "Cam 1", status, owner: "", raisedMs });

describe("real analytics helpers", () => {
  const now = new Date(2026, 9, 29, 15, 0, 0).getTime();
  const rows = [
    row("A", "Critical", new Date(2026, 9, 29, 9, 30).getTime()),
    row("B", "Warning", new Date(2026, 9, 28, 23, 0).getTime(), "Zone entry"),
    row("C", "Advisory", new Date(2026, 9, 29, 10, 0).getTime(), "PPE review", "Monitoring"),
    row("D", "Advisory", new Date(2026, 8, 1, 10, 0).getTime()),
  ];
  it("builds the last 7 days from raised alerts only, ignoring Monitoring", () => {
    const t = realTrend(rows, now);
    expect(t).toHaveLength(7);
    expect(t[6]).toMatchObject({ critical: 1, warning: 0, advisory: 0 });
    expect(t[5]).toMatchObject({ warning: 1 });
    expect(t.reduce((a, d) => a + d.critical + d.warning + d.advisory, 0)).toBe(2);
  });
  it("heatmap puts alerts in two-hour blocks; an empty history is all zeros", () => {
    expect(realHeatmap(rows, now)[6]!.values[4]).toBe(1);
    expect(realHeatmap([], now).flatMap((r) => r.values).every((v) => v === 0)).toBe(true);
  });
  it("groups events by condition and scales heat levels", () => {
    expect(eventsByCondition(rows, now - 7 * 86_400_000)).toEqual([{ type: "PPE review", count: 1 }, { type: "Zone entry", count: 1 }]);
    expect(heatLevel(0, 5)).toBe(0);
    expect(heatLevel(5, 5)).toBe(5);
    expect(heatLevel(1, 100)).toBe(1);
  });
  it("weekly report uses the rigs and types found in the stored events, never the fixtures", () => {
    const events = eventsFromAlerts(rows);
    expect(events.map((e) => e.id).sort()).toEqual(["A", "B", "D"]);
    const week = availableWeeks(events)[0]!;
    const stats = computeWeekly(events, week);
    expect(stats.rigRows.map((r) => r.rig)).toEqual(["This rig"]);
    expect(computeWeekly([], "2026-10-21").rigRows).toEqual([]);
  });
});

describe("pages with sample data off show nothing invented", () => {
  const pages: [string, ReactNode][] = [
    ["Safety analytics", <SafetyAnalytics />],
    ["Event overview", <EventOverview />],
    ["Actions & handover", <ActionsHandover />],
    ["Users & access", <UsersAccess />],
    ["Reports", <Reports />],
    ["Monitoring configuration", <SafetyRules />],
    ["System health", <SystemHealth />],
    ["License & capacity", <LicenseCapacity />],
    ["Integrations", <Integrations />],
    ["Audit trail", <AuditTrail />],
    ["Zone editor", <ZoneEditor />],
  ];
  for (const [name, el] of pages)
    it(name, () => {
      cleanup();
      render(el);
      noSample();
    });

  const reports: [string, ReactNode][] = [
    ["shift", <ShiftReport pages={["01 / 02", "02 / 02"]} />],
    ["event", <EventReport page="01 / 01" />],
    ["hse", <HseReport page="01 / 01" />],
    ["action", <ActionReport page="01 / 01" />],
    ["handover", <HandoverReport page="01 / 01" />],
    ["weekly", <WeeklyReport />],
    ["health", <HealthReport page="01 / 01" />],
    ["pilot", <PilotReport page="01 / 01" />],
    ["register", <RegisterReport page="01 / 01" />],
    ["audit", <AuditReport page="01 / 01" />],
  ];
  for (const [name, el] of reports)
    it(`${name} report says there is no data`, async () => {
      cleanup();
      render(el);
      await waitFor(() => expect(document.body.textContent).toMatch(/No data|No camera configured|No stored events|No alerts|No event selected|Pending|Not measured/));
      noSample();
    });

  it("audit trail shows only recorded rows and an empty state", () => {
    cleanup();
    render(<AuditTrail />);
    expect(screen.getByText(/No data yet/)).toBeTruthy();
  });
});

describe("what the user types is kept and reported", () => {
  it("creates a corrective action that the action report then shows", async () => {
    cleanup();
    store.clear();
    render(<ActionsHandover />);
    expect(screen.getByText("No corrective actions yet")).toBeTruthy();
    fireEvent.click(screen.getAllByText("New action")[0]!);
    fireEvent.change(await screen.findByLabelText("Action title"), { target: { value: "Fit a barrier at the gate" } });
    fireEvent.click(screen.getByText("Create action"));
    await waitFor(() => expect(screen.getByText(/CA-001 · Fit a barrier at the gate/)).toBeTruthy());
    expect((store.get("actions:v1") as { id: string }[])[0]!.id).toBe("CA-001");
    cleanup();
    render(<ActionReport page="01 / 01" actionId="CA-001" />);
    await waitFor(() => expect(screen.getByText("Fit a barrier at the gate")).toBeTruthy());
    cleanup();
    render(<ActionReport page="01 / 01" actionId="CA-404" />);
    await waitFor(() => expect(screen.getByText(/No data for CA-404/)).toBeTruthy());
  });
});
