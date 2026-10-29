import { describe, expect, it } from "vitest";
import { demoPolicy, severityTrend, eventHistory, zoneDefaults } from "@/data/mock";

describe("RigMind demonstration constraints", () => {
  it("retains rolling footage for 7 days", () => expect(demoPolicy.rollingDays).toBe(7));
  it("retains event clips for 30 days", () => expect(demoPolicy.eventDays).toBe(30));
  it("allocates 24 of 32 streams with 8 remaining", () => {
    expect(demoPolicy.allocatedStreams).toBe(24);
    expect(demoPolicy.streamCapacity).toBe(32);
    expect(demoPolicy.streamCapacity - demoPolicy.allocatedStreams).toBe(8);
  });
  it("keeps physical outputs disabled", () => expect(demoPolicy.physicalOutputs).toBe(false));
  it("keeps machinery stop disabled", () => expect(demoPolicy.machineryStop).toBe(false));
  it("keeps deployed v3 separate from draft v4", () => {
    expect(demoPolicy.deployedRuleVersion).toBe(3);
    expect(demoPolicy.draftRuleVersion).toBe(4);
  });
  it("has the specified 06 Oct severity breakdown", () => {
    const day = severityTrend.find((d) => d.day === "06 Oct");
    expect(day).toEqual({ day: "06 Oct", critical: 6, warning: 14, advisory: 22 });
  });
  it("includes all six illustrative history records", () =>
    expect(eventHistory.map((e) => e.id)).toEqual([
      "EV-1042",
      "EV-1039",
      "EV-1035",
      "EV-1028",
      "EV-1017",
      "EV-1012",
    ]));
  it("starts with four valid zone vertices", () => {
    expect(zoneDefaults).toHaveLength(4);
    expect(zoneDefaults.every(([x, y]) => x >= 0 && x <= 100 && y >= 0 && y <= 100)).toBe(true);
  });
});
