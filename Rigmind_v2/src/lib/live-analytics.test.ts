import { describe, expect, it } from "vitest";
import type { CameraStatus } from "@/lib/camera-status";
import {
  cameraHealthRows,
  countSeverity,
  mergeTrend,
  percentile,
  summarizeCameras,
  todaysAlerts,
  type LiveAlertRow,
} from "@/lib/live-analytics";

const NOW = new Date(2026, 9, 29, 12, 0, 0).getTime();
const row = (id: string, severity: string, raisedMs: number): LiveAlertRow => ({
  id,
  severity,
  condition: "c",
  place: "p",
  status: "Open",
  owner: "",
  raisedMs,
});
const cam = (id: string, over: Partial<CameraStatus> = {}): CameraStatus => ({
  id,
  state: "online",
  reason: "",
  width: 0,
  height: 0,
  streamFps: 25,
  fps: 20,
  kbps: 1000,
  reconnects: 1,
  attempt: 0,
  onlineSince: NOW - 90_000,
  changedAt: NOW - 30_000,
  onlineMs: 90_000,
  observedMs: 100_000,
  ...over,
});

describe("live alerts", () => {
  it("keeps only today's alerts and counts severities", () => {
    const rows = [
      row("a", "Critical", NOW - 1000),
      row("b", "Warning", NOW - 2000),
      row("c", "Advisory", NOW - 3 * 86_400_000),
    ];
    const today = todaysAlerts(rows, NOW);
    expect(today.map((r) => r.id)).toEqual(["a", "b"]);
    expect(countSeverity(today)).toEqual({ critical: 1, warning: 1, advisory: 0 });
  });
});

describe("mergeTrend", () => {
  const demo = [
    { day: "05 Oct", critical: 3, warning: 9, advisory: 21 },
    { day: "06 Oct", critical: 6, warning: 14, advisory: 22 },
  ];
  it("returns demo untouched with no live events", () => {
    expect(mergeTrend(demo, { critical: 0, warning: 0, advisory: 0 })).toBe(demo);
  });
  it("adds live counts to the last day only", () => {
    const out = mergeTrend(demo, { critical: 2, warning: 1, advisory: 0 });
    expect(out[0]).toEqual(demo[0]);
    expect(out[1]).toEqual({ day: "06 Oct", critical: 8, warning: 15, advisory: 22 });
    expect(demo[1]?.critical).toBe(6);
  });
});

describe("cameras", () => {
  it("summarizes online cameras", () => {
    const s = summarizeCameras(
      {
        "CAM-01": cam("CAM-01"),
        "CAM-02": cam("CAM-02", { state: "offline", fps: 0, kbps: 0, onlineSince: 0 }),
      },
      ["CAM-01", "CAM-02", "CAM-03"],
      NOW,
    );
    expect(s).toMatchObject({
      total: 3,
      online: 1,
      offline: 2,
      connecting: 0,
      avgFps: 20,
      totalKbps: 1000,
      reconnects: 2,
    });
    expect(s.longestOnlineSec).toBe(90);
  });
  it("is empty-safe", () => {
    expect(summarizeCameras({}).availability).toBeNull();
  });
  it("health rows mark unconfigured demo cameras offline", () => {
    const rows = cameraHealthRows(["CAM-01", "CAM-02"], { "CAM-01": cam("CAM-01") }, NOW);
    expect(rows[0]).toMatchObject({ name: "CAM-01", status: "Online", tone: "success" });
    expect(rows[1]).toMatchObject({ name: "CAM-02", status: "Offline", seen: "Not configured" });
  });
});

describe("percentile", () => {
  it("handles empty and ordinary input", () => {
    expect(percentile([], 50)).toBeNull();
    expect(percentile([10, 2, 8, 4], 50)).toBe(4);
  });
});

describe("alarm episodes in the counts", () => {
  it("a Monitoring item is not an event; an escalated item counts once at its current severity", () => {
    const monitoring = { ...row("ALM-1", "Advisory", NOW), status: "Monitoring", phase: "Monitoring" };
    const acknowledgedMonitoring = { ...row("ALM-2", "Advisory", NOW), status: "Acknowledged", phase: "Monitoring" };
    const escalated = { ...row("ALM-3", "Critical", NOW), status: "Escalated", phase: "Escalated" };
    const cleared = { ...row("ALM-4", "Warning", NOW), status: "Cleared", phase: "Cleared" };
    expect(countSeverity([monitoring, acknowledgedMonitoring, escalated, cleared])).toEqual({ critical: 1, warning: 1, advisory: 0 });
  });
});