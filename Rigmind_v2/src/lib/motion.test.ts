import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const css = readFileSync(resolve(__dirname, "../styles.css"), "utf8");
const motion = css.slice(css.indexOf("rm-motion"));

// the text of one @keyframes block, up to its closing brace at the start of a line
function keyframes(name: string): string {
  const start = motion.indexOf(`@keyframes ${name}`);
  const end = motion.indexOf("\n}", start);
  return motion.slice(start, end);
}

describe("subtle motion", () => {
  it("interactive elements ease colour, border and shadow in 150-250 ms", () => {
    expect(motion).toMatch(/--rm-fast:\s*1[5-9]\dms/);
    expect(motion).toMatch(/--rm-med:\s*2[0-5]\dms/);
    expect(motion).toMatch(/button, a, input[^{]*\{[^}]*transition:[^}]*background-color[^}]*border-color[^}]*box-shadow/s);
  });
  it("pages, panels and list items fade in with a small rise (6 px at most)", () => {
    for (const name of ["rm-page-in", "rm-pop-in", "rm-row-in"]) {
      const block = keyframes(name);
      expect(block).toContain("opacity: 0");
      const px = Number(/translateY\((\d+)px\)/.exec(block)?.[1]);
      expect(px).toBeGreaterThan(0);
      expect(px).toBeLessThanOrEqual(6);
    }
    expect(motion).toMatch(/main > \*\s*\{\s*animation: rm-page-in/);
  });
  it("is switched off for reduced motion", () => {
    const reduce = motion.slice(motion.indexOf("@media (prefers-reduced-motion: reduce)"));
    expect(reduce).toContain("animation: none !important");
    expect(reduce).toContain("transition: none !important");
  });
  it("video and canvas keep their own behaviour during the theme glide", () => {
    expect(motion).toContain(":not(video):not(canvas)");
  });
});
