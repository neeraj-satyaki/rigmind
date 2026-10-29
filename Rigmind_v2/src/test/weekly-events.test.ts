import { describe, expect, it } from "vitest";
import { availableWeeks, buildSeedEvents, computeWeekly, type LoggedEvent } from "@/lib/weekly-events";
import { reportableSummaries, type StoredSummary } from "@/lib/event-review-store";

const ev = (id: string, at: string, severity: LoggedEvent["severity"] = "Warning"): LoggedEvent =>
  ({ id, at, rig: "Demo rig 01", severity, type: "PPE review", ackSeconds: 10, status: "Closed" });

describe("weekly report calculations", () => {
  it("weeks start on Wednesday and counts come from stored events", () => {
    const s = computeWeekly([ev("A", "2026-09-30T10:00:00", "Critical"), ev("B", "2026-10-06T11:00:00"), ev("C", "2026-10-07T11:00:00")], "2026-09-30");
    expect(s.total).toBe(2);
    expect(s.critical).toBe(1);
  });
  it("marks comparison unavailable when prior week has no events", () => {
    const s = computeWeekly([ev("A", "2026-09-30T10:00:00")], "2026-09-30");
    expect(s.vsPrev.total.available).toBe(false);
  });
  it("compares with prior week when data exists", () => {
    const s = computeWeekly([ev("P1", "2026-09-24T10:00:00"), ev("P2", "2026-09-25T10:00:00"), ev("A", "2026-09-30T10:00:00")], "2026-09-30");
    expect(s.vsPrev.total).toEqual({ available: true, previous: 2, change: -1, pct: -50 });
  });
  it("seed contains named investigation events in the latest week", () => {
    const all = buildSeedEvents();
    const latest = availableWeeks(all)[0]!;
    expect(computeWeekly(all, latest).ids.has("EV-1042")).toBe(true);
  });
  it("only HSE-approved summaries are reportable", () => {
    const base = { draft: { summary: "", recordedFacts: [], missingInformation: [] }, sources: [], generatedAt: "" };
    const list: StoredSummary[] = [{ ...base, eventId: "A", approval: null }, { ...base, eventId: "B", approval: { by: "M", role: "HSE", at: "x" } }];
    expect(reportableSummaries(list).map((s) => s.eventId)).toEqual(["B"]);
  });
});
