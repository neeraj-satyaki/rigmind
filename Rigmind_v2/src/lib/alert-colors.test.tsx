import { readFileSync } from "node:fs";
import { resolve } from "node:path";
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
      expect(String(o.style.background)).toBe("var(--toast-bg)");
      expect(String(o.style.background)).toBe("var(--toast-bg)"); // the theme token (see styles.css), never a solid colour
      expect(String(o.style.backdropFilter)).toMatch(/blur\((1[2-9]|[2-9]\d)px\)/); // blurred enough to read over the page
      expect(String(o.style.background)).not.toContain("gradient");
      expect(String(o.style.backdropFilter)).toMatch(/blur\(\d+px\)/);
      expect(o.classNames.title).toContain("font-black");
      expect(o.classNames.description).toContain("font-bold");
    });
  }
  it("the border is the exact pure level colour, 2px solid, full strength", () => {
    expect(alertToastOptions("Advisory").style.border).toBe("2px solid #FFD60A");
    expect(alertToastOptions("Warning").style.border).toBe("2px solid #FF9500");
    expect(alertToastOptions("Critical").style.border).toBe("2px solid #FF3B30");
  });
  it("Red and Orange state names map like severities", () => {
    expect(alertToastOptions("Red").style.color).toBe("var(--toast-red)");
    expect(alertToastOptions("Orange").style.color).toBe("var(--toast-orange)");
  });
});

describe("toast background tokens (styles.css)", () => {
  const css = readFileSync(resolve(__dirname, "../styles.css"), "utf8");
  it("light theme: the theme colour at about a third", () => {
    expect(css).toMatch(/:root\s*\{[^}]*--toast-bg:\s*color-mix\(in srgb, var\(--popover\) (\d+)%, transparent\)/s);
    const pct = Number(/--toast-bg:\s*color-mix\(in srgb, var\(--popover\) (\d+)%/.exec(css)?.[1]);
    expect(pct).toBeGreaterThan(15);
    expect(pct).toBeLessThan(60); // clearly see-through
  });
  it("dark theme: the loading-screen navy (#16294A = 22,41,74), see-through", () => {
    const m = /\.dark\s*\{[^}]*--toast-bg:\s*rgba\(22,\s*41,\s*74,\s*([0-9.]+)\)/s.exec(css);
    expect(m).not.toBeNull();
    expect(Number(m![1])).toBeLessThan(0.8);
  });
});
