import { describe, expect, it } from "vitest";
import { shiftPages } from "@/reports/templates/pack";
describe("pack index page ranges", () => {
  it("weekly range grows with summary sheets", () => expect(shiftPages("Analytics · p. 08–09", 3)).toBe("Analytics · p. 08–12"));
  it("later entries shift", () => expect(shiftPages("Audit trail · p. 13", 3)).toBe("Audit trail · p. 16"));
  it("earlier entries unchanged", () => expect(shiftPages("Handover · p. 07", 3)).toBe("Handover · p. 07"));
});
