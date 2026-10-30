import { describe, expect, it } from "vitest";
import { alertToastOptions } from "./alert-colors";

describe("alert toast per level", () => {
  const cases = [
    ["Advisory", "yellow"],
    ["Warning", "orange"],
    ["Critical", "red"],
  ] as const;
  for (const [severity, level] of cases) {
    it(`${severity}: thick ${level} text on the theme background, slightly transparent and blurred, no gradient`, () => {
      const o = alertToastOptions(severity);
      expect(o.style.color).toBe(`var(--toast-${level})`);
      expect(String(o.style.background)).toContain("var(--popover)"); // the theme's own colour
      expect(String(o.style.background)).toContain("transparent"); // slightly see-through
      expect(String(o.style.background)).not.toContain("gradient");
      expect(String(o.style.backdropFilter)).toMatch(/blur\(\d+px\)/);
      expect(o.classNames.title).toContain("font-black");
      expect(o.classNames.description).toContain("font-bold");
    });
  }
  it("Red and Orange state names map like severities", () => {
    expect(alertToastOptions("Red").style.color).toBe("var(--toast-red)");
    expect(alertToastOptions("Orange").style.color).toBe("var(--toast-orange)");
  });
});
