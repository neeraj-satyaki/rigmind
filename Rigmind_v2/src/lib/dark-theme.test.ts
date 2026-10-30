import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const css = readFileSync(resolve(__dirname, "../styles.css"), "utf8");
const dark = /\.dark\s*\{([^}]*)\}/s.exec(css)![1]!;
const token = (name: string) => new RegExp(`--${name}:\\s*(#[0-9a-fA-F]{6})`).exec(dark)?.[1];

function hsl(hex: string) {
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255) as [number, number, number];
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2, d = max - min;
  const s = d === 0 ? 0 : d / (1 - Math.abs(2 * l - 1));
  const h = d === 0 ? 0 : max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return { h: h * 60, s, l };
}

describe("dark theme is dark navy blue", () => {
  for (const name of ["background", "card", "popover", "field", "border", "sidebar"]) {
    it(`--${name} is a dark, blue-hued navy`, () => {
      const hex = token(name);
      expect(hex).toBeDefined();
      const c = hsl(hex!);
      expect(c.h).toBeGreaterThan(205);
      expect(c.h).toBeLessThan(235); // blue
      expect(c.s).toBeGreaterThan(0.3); // clearly coloured, not grey
      expect(c.l).toBeLessThan(0.3); // dark
    });
  }
  it("the page is darker than its cards", () => {
    expect(hsl(token("background")!).l).toBeLessThan(hsl(token("card")!).l);
  });
});
