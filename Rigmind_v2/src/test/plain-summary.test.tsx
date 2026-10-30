import { beforeEach, describe, expect, it, vi } from "vitest";
import { render, screen } from "@testing-library/react";
import type { NativeEvent } from "@/lib/native-bridge";

const host = vi.hoisted(() => ({
  listeners: new Set<(e: NativeEvent) => void>(),
  call: vi.fn(),
  feed: [] as unknown[],
}));

vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  nativeCall: (method: string, params: unknown) => {
    host.call(method, params);
    if (method === "alerts.feed") return Promise.resolve(host.feed);
    return Promise.resolve({});
  },
  onNativeEvent: (l: (e: NativeEvent) => void) => {
    host.listeners.add(l);
    return () => host.listeners.delete(l);
  },
}));
vi.mock("@/lib/device-db", () => ({
  writeDevice: async () => {},
  readDevice: async () => undefined,
  listDevice: async () => [],
}));

import { loadPlainSummaries, onSummaryReady, plainSummaryCsvRows, plainSummaryFromRow, requestSummary, summaryReportText, type FeedRow } from "@/lib/plain-summary";
import { PlainSummaryTable, chunkPlainSummaries } from "@/reports/PlainSummaryTable";
import { RegisterReport, registerCsvRows } from "@/reports/templates/register";

const SENTENCE = "Camera CAM-01 saw the elevator near the platform with the link tilt angled 28 degrees. Check the area and confirm the crew is clear.";

const meta = {
  v: 1,
  kind: "alarm",
  camera: "CAM-01",
  peak: "Orange",
  cycle: "Y>O>Y",
  closed: true,
  states: [
    { state: "Yellow", tsMs: 1000, frame: 1, facts: { machine_present: true, link_tilt_present: true, link_tilt_angled: true, angle_deg: 28.2 } },
    { state: "Orange", tsMs: 2000, frame: 2, facts: { machine_present: true, link_tilt_present: true, link_tilt_angled: true, elevator_present: true, elevator_near_platform: true, angle_deg: 28.4 } },
  ],
};
const row = (id: string, extra: Partial<FeedRow> = {}): FeedRow => ({
  id,
  severity: "Warning",
  condition: "Orange alert",
  place: "CAM-01",
  phase: "Cleared",
  raisedMs: Date.parse("2026-10-01T10:00:00Z"),
  metadata: meta,
  summary: null,
  summarySource: null,
  summaryAt: null,
  ...extra,
});

beforeEach(() => {
  host.listeners.clear();
  host.call.mockReset();
  host.feed = [];
});

describe("summaries are read from the host rows", () => {
  it("loads the alerts that have or await a sentence, oldest first; a Yellow-only row has none", async () => {
    host.feed = [
      row("ALM-CAM-01-b2", { raisedMs: 3_000_000, summary: SENTENCE, summarySource: "slm", summaryAt: 3_500_000 }),
      row("ALM-CAM-01-b1", { raisedMs: 1_000_000 }),
      row("ALM-CAM-01-b0", { raisedMs: 500_000, condition: "Yellow monitoring", severity: "Advisory", metadata: { ...meta, peak: "Yellow", states: [meta.states[0]!] } }),
    ];
    const list = await loadPlainSummaries();
    expect(list.map((s) => s.alertId)).toEqual(["ALM-CAM-01-b1", "ALM-CAM-01-b2"]);
    expect(list[0]).toMatchObject({ text: "", source: "none", status: "pending", chip: "Summary pending", camId: "CAM-01" });
    expect(list[0]!.facts).toBe("machine present, link tilt angled 28°, elevator near platform");
    expect(list[1]).toMatchObject({ text: SENTENCE, source: "slm", status: "ready", chip: null });
    expect(host.call).toHaveBeenCalledWith("alerts.feed", { limit: 500 });
  });

  it("exports the sentence, the facts and the source to CSV rows", () => {
    const list = [plainSummaryFromRow(row("A1", { summary: SENTENCE, summarySource: "slm", summaryAt: 1 }))!, plainSummaryFromRow(row("A2", { summarySource: "unavailable" }))!];
    const rows = plainSummaryCsvRows(list);
    expect(rows[0]).toMatchObject({ "Alert ID": "A1", "Plain-language summary": SENTENCE, Source: "BitNet", "Camera · place": "CAM-01", "Level label": "Orange alert" });
    expect(rows[1]).toMatchObject({ "Plain-language summary": "", Source: "BitNet not available" });
    expect(rows[1]!.Facts).toContain("elevator near platform");
  });

  it("the report table shows the sentence once it exists, else label, camera and facts with the status", () => {
    const done = plainSummaryFromRow(row("A1", { summary: SENTENCE, summarySource: "slm", summaryAt: 1 }))!;
    const waiting = plainSummaryFromRow(row("A2"))!;
    const noModel = plainSummaryFromRow(row("A3", { summarySource: "unavailable" }))!;
    expect(summaryReportText(waiting)).toBe("Orange alert · CAM-01 · machine present, link tilt angled 28°, elevator near platform");
    render(<PlainSummaryTable rows={[done, waiting, noModel]} />);
    expect(screen.getByText(SENTENCE)).toBeInTheDocument();
    expect(screen.getByText("BitNet")).toBeInTheDocument();
    expect(screen.getAllByText("Orange alert · CAM-01 · machine present, link tilt angled 28°, elevator near platform")).toHaveLength(2);
    expect(screen.getByText("Summary pending")).toBeInTheDocument();
    expect(screen.getByText("BitNet not available")).toBeInTheDocument();
    expect(chunkPlainSummaries(Array.from({ length: 17 }, () => done)).map((c) => c.length)).toEqual([8, 8, 1]);
  });

  it("the event register report lists the host summaries and adds a CSV column", async () => {
    host.feed = [row("ALM-CAM-01-b1", { summary: SENTENCE, summarySource: "slm", summaryAt: 1 })];
    render(<RegisterReport page="01 / 01" />);
    expect(await screen.findAllByText(new RegExp("Camera CAM-01 saw the elevator"))).not.toHaveLength(0);
    expect(registerCsvRows([plainSummaryFromRow(row("EV-1042", { summary: SENTENCE, summarySource: "slm" }))!]).find((r) => r["Event ID"] === "EV-1042")?.["Plain-language summary"]).toBe(SENTENCE);
    expect(registerCsvRows([])[0]).toHaveProperty("Plain-language summary", "");
  });
});

describe("manual re-run and the ready signal", () => {
  it("'Plain words' sends only the alert id (the host builds the prompt from its stored metadata)", async () => {
    await requestSummary("ALM-CAM-01-b1", 5);
    const [method, params] = host.call.mock.calls[0]!;
    expect(method).toBe("event.describe");
    expect(params).toMatchObject({ alertId: "ALM-CAM-01-b1", sceneTs: 5 });
    expect(params).not.toHaveProperty("condition");
    expect(params).not.toHaveProperty("place");
  });

  it("onSummaryReady reports description events by alert id", () => {
    const seen: string[] = [];
    const off = onSummaryReady((id) => seen.push(id));
    host.listeners.forEach((l) => l({ ev: "description", alertId: "A1", text: SENTENCE, source: "slm" } as NativeEvent));
    host.listeners.forEach((l) => l({ ev: "status", cam: "CAM-01" } as NativeEvent));
    expect(seen).toEqual(["A1"]);
    off();
    expect(host.listeners.size).toBe(0);
  });
});
