import { describe, expect, it } from "vitest";
import { reportableSummaries, type StoredSummary } from "@/lib/event-review-store";
import { isValidEmail } from "@/lib/profile";

const base = (id: string, approved: boolean): StoredSummary => ({
  eventId: id,
  draft: { summary: "s", recordedFacts: [], missingInformation: [] },
  sources: [],
  generatedAt: "now",
  approval: approved ? { by: "Maha S.", role: "HSE reviewer", at: "now" } : null,
});

describe("weekly report rules", () => {
  it("includes only HSE-approved summaries", () => {
    const out = reportableSummaries([base("EV-1042", true), base("EV-1039", false)]);
    expect(out.map((s) => s.eventId)).toEqual(["EV-1042"]);
  });
  it("rejects invalid profile emails", () => {
    expect(isValidEmail("faisal@satyaki.co.in")).toBe(true);
    expect(isValidEmail("not-an-email")).toBe(false);
  });
});
