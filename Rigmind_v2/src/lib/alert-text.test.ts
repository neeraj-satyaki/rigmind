import { describe, expect, it } from "vitest";
import { alertDescription, alertDescriptionLine, factLabels, metadataFacts, peakState, summaryStatus, type AlertMeta } from "./alert-text";
import { alertToastText } from "./alert-colors";

const meta: AlertMeta = {
  kind: "alarm",
  camera: "CAM-01",
  peak: "Orange",
  cycle: "Y>O>Y",
  closed: true,
  states: [
    { state: "Yellow", tsMs: 1, facts: { machine_present: true, link_tilt_present: true, link_tilt_angled: false, angle_deg: 7.4 } },
    { state: "Orange", tsMs: 2, facts: { machine_present: true, link_tilt_present: true, link_tilt_angled: true, elevator_present: true, elevator_near_platform: true, angle_deg: 28.4 } },
    { state: "Yellow", tsMs: 3, facts: { machine_present: true } },
  ],
};

describe("fact labels (a list of labelled facts, never a sentence)", () => {
  it("names the facts that are true, with the angle", () => {
    expect(factLabels(meta.states![1]!.facts)).toEqual(["machine present", "link tilt angled 28°", "elevator near platform"]);
    expect(factLabels({ machine_present: true, link_tilt_present: true, link_tilt_angled: false, angle_deg: 7.4 })).toEqual(["machine present", "link tilt present 7°"]);
    expect(factLabels({ link_tilt_angled: true, angle_deg: null })).toEqual(["link tilt angled"]);
    expect(factLabels({ elevator_present: true })).toEqual(["elevator present"]);
    expect(factLabels(null)).toEqual([]);
    expect(factLabels({})).toEqual([]);
  });
  it("takes the facts of the highest level reached", () => {
    expect(peakState(meta)?.state).toBe("Orange");
    expect(metadataFacts(meta)).toBe("machine present, link tilt angled 28°, elevator near platform");
    expect(metadataFacts({ kind: "alarm", states: [{ state: "Yellow", tsMs: 1, facts: null }] })).toBe("");
    expect(metadataFacts(null)).toBe("");
  });
  it("PPE metadata: the items not detected and the people count", () => {
    expect(metadataFacts({ kind: "ppe", missing: ["vest", "gloves"], persons: 2 })).toBe("vest, gloves not detected · 2 people");
    expect(metadataFacts({ kind: "ppe", missing: ["helmet"], persons: 1 })).toBe("helmet not detected · 1 person");
  });
});

describe("alert description and status chip", () => {
  const base = { condition: "Orange alert", place: "CAM-01", metadata: meta, summary: null, summarySource: null };
  it("pending: label, camera and facts with 'Summary pending'", () => {
    const d = alertDescription(base);
    expect(d).toMatchObject({ text: "Orange alert · CAM-01 · machine present, link tilt angled 28°, elevator near platform", isSummary: false, status: "pending", chip: "Summary pending" });
    expect(alertDescriptionLine(base)).toBe(`${d.text} [Summary pending]`);
  });
  it("no model: 'BitNet not available'", () => {
    expect(alertDescription({ ...base, summarySource: "unavailable" })).toMatchObject({ status: "unavailable", chip: "BitNet not available" });
    expect(alertDescription({ ...base, summarySource: "template" }).chip).toBe("BitNet not available");
  });
  it("the SLM sentence replaces the facts and the chip", () => {
    const d = alertDescription({ ...base, summary: "Camera CAM-01 saw it. Check the area.", summarySource: "slm" });
    expect(d).toMatchObject({ text: "Camera CAM-01 saw it. Check the area.", isSummary: true, status: "ready", chip: null });
  });
  it("a Yellow-only alert is only watched: no chip", () => {
    const yellow = { condition: "Yellow monitoring", place: "CAM-01", metadata: { kind: "alarm", camera: "CAM-01", peak: "Yellow", states: [] }, summary: null, summarySource: null };
    expect(summaryStatus(yellow)).toBe("none");
    expect(alertDescription(yellow).chip).toBeNull();
  });
  it("sample rows (no host keys) show their condition unchanged", () => {
    expect(alertDescription({ condition: "Occupied movement zone", place: "Demo rig 01" })).toMatchObject({ text: "Occupied movement zone", chip: null, status: "none" });
  });
});

describe("toast text", () => {
  const a = { id: "ALM-CAM-01-b11", severity: "Warning", condition: "Orange alert", place: "CAM-01", metadata: meta, summary: null, summarySource: null };
  it("title is the level label and camera; description is the facts, then the sentence", () => {
    expect(alertToastText(a)).toEqual({ title: "Orange alert · CAM-01", description: "machine present, link tilt angled 28°, elevator near platform", chip: "Summary pending", isSummary: false });
    expect(alertToastText({ ...a, summary: "Camera CAM-01 saw it. Check the area.", summarySource: "slm" })).toEqual({
      title: "Orange alert · CAM-01",
      description: "Camera CAM-01 saw it. Check the area.",
      chip: null,
      isSummary: true,
    });
  });
  it("sample alerts keep the old toast text", () => {
    expect(alertToastText({ id: "EV-1043", severity: "Critical", condition: "Occupied movement zone", place: "Demo rig 01" })).toMatchObject({
      title: "Red · Critical safety event",
      description: "EV-1043 · Occupied movement zone · Demo rig 01",
    });
  });
});
