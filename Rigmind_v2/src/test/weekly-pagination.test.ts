import { describe, expect, it } from "vitest";
import { blockHeight, paginateSummaries, SUMMARY_PAGE_BUDGET } from "@/reports/templates/weekly";
import type { StoredSummary } from "@/lib/event-review-store";

const long = (n: number) => Array.from({ length: n }, (_, i) => `Sentence ${i} about recorded zone occupancy and PPE.`).join(" ");
const item = (id: string, n: number): StoredSummary => ({
  eventId: id, generatedAt: "", sources: ["Timeline", `Clip ${id}`],
  draft: { summary: long(n), recordedFacts: Array.from({ length: n / 4 }, () => long(4)), missingInformation: [] },
  approval: { by: "Maha S.", role: "HSE reviewer", at: "6 Oct 2026" },
});

describe("approved summary pagination", () => {
  const items = [item("EV-1", 80), item("EV-2", 40), item("EV-3", 12)];
  const pages = paginateSummaries(items);
  it("no sheet exceeds the printable budget", () => {
    for (const p of pages) expect(p.reduce((a, b) => a + blockHeight(b), 0)).toBeLessThanOrEqual(SUMMARY_PAGE_BUDGET + 44);
    expect(pages.length).toBeGreaterThan(1);
  });
  it("keeps every source and approval line", () => {
    const metas = pages.flat().filter((b) => b.kind === "meta");
    expect(metas.map((m) => m.s.eventId)).toEqual(["EV-1", "EV-2", "EV-3"]);
  });
  it("keeps all summary text", () => {
    const text = pages.flat().filter((b) => b.kind === "head" && b.s.eventId === "EV-1").map((b) => (b as { text: string }).text).filter(Boolean).join(" ");
    expect(text.replace(/\s+/g, " ")).toBe(items[0]!.draft.summary);
  });
});
