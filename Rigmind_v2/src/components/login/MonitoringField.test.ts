import { describe, expect, it } from "vitest";
import { insideZone } from "./MonitoringField";

describe("monitoring field zone", () => {
  it("detects a person on the drill floor inside the polygon", () => {
    expect(insideZone(.66, .77)).toBe(true);
  });
  it("does not detect a person at the camera or outside the perimeter", () => {
    expect(insideZone(.17, .20)).toBe(false);
    expect(insideZone(.9, .8)).toBe(false);
    expect(insideZone(.66, .6)).toBe(false);
  });
});