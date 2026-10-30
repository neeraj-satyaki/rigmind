import { describe, expect, it } from "vitest";
import { formatRate } from "./format-rate";

describe("formatRate", () => {
  it("shows Mbps from 1 Mbps up, one decimal", () => {
    expect(formatRate(1000)).toBe("1.0 Mbps");
    expect(formatRate(4200)).toBe("4.2 Mbps");
    expect(formatRate(12345)).toBe("12.3 Mbps");
  });
  it("shows kbps only below 1 Mbps", () => {
    expect(formatRate(999.6)).toBe("1000 kbps"); // below 1000 stays in kbps (rounded for display)
    expect(formatRate(950)).toBe("950 kbps");
    expect(formatRate(0)).toBe("0 kbps");
  });
  it("treats missing or bad values as zero", () => {
    expect(formatRate(NaN)).toBe("0 kbps");
    expect(formatRate(-5)).toBe("0 kbps");
  });
});
