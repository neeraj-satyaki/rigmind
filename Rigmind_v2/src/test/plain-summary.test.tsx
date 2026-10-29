import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";
import type { NativeEvent } from "@/lib/native-bridge";

const host = vi.hoisted(() => ({
  listeners: new Set<(e: NativeEvent) => void>(),
  call: vi.fn(),
  store: new Map<string, unknown>(),
}));

vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  nativeCall: (...a: unknown[]) => host.call(...a),
  onNativeEvent: (l: (e: NativeEvent) => void) => {
    host.listeners.add(l);
    return () => host.listeners.delete(l);
  },
}));
vi.mock("@/lib/device-db", () => ({
  writeDevice: async (k: string, v: unknown) => void host.store.set(k, v),
  readDevice: async (k: string) => host.store.get(k),
  listDevice: async (prefix: string) =>
    [...host.store].filter(([k]) => k.startsWith(prefix)).map(([key, value]) => ({ key, value })),
}));

import {
  describeAlert,
  loadPlainSummaries,
  plainSummaryCsvRows,
  resolveDescription,
  type PlainSummary,
} from "@/lib/plain-summary";
import { PlainSummaryTable, chunkPlainSummaries } from "@/reports/PlainSummaryTable";
import { RegisterReport, registerCsvRows } from "@/reports/templates/register";

const alert = { id: "EV-2001", severity: "Critical", condition: "Platform-elevator collision", place: "Rig 1", };
const TEMPLATE = "At 01 Oct, camera CAM-01 at Rig 1 recorded: Platform-elevator collision. This Critical alert needs immediate action.";
const SENTENCE = "Camera CAM-01 at Rig 1 saw a platform-elevator collision, so this critical alert needs immediate action.";
const emit = (e: NativeEvent) => host.listeners.forEach((l) => l(e));

beforeEach(() => {
  host.listeners.clear();
  host.store.clear();
  host.call.mockReset();
});
afterEach(() => vi.useRealTimers());

describe("resolveDescription", () => {
  it("shows the SLM sentence when source is slm", () => {
    expect(resolveDescription({ text: SENTENCE, source: "slm" }, TEMPLATE)).toEqual({ text: SENTENCE, source: "slm" });
  });
  it("falls back to the template when source is template or text is null/empty", () => {
    for (const msg of [{ text: null, source: "template" }, { text: null, source: "slm" }, { text: "  ", source: "slm" }, { text: SENTENCE, source: "template" }]) {
      expect(resolveDescription(msg, TEMPLATE)).toEqual({ text: TEMPLATE, source: "template" });
    }
  });
});

describe("describeAlert (toast text)", () => {
  it("shows the BitNet sentence when the host sends it after the reply", async () => {
    host.call.mockResolvedValue({ text: TEMPLATE, slm: true });
    const show = vi.fn();
    await describeAlert(alert, "CAM-01", "Rig 1", show);
    expect(show).not.toHaveBeenCalled();
    emit({ ev: "description", alertId: "EV-2000", text: "other", source: "slm" }); // other alert: ignored
    emit({ ev: "description", alertId: "EV-2001", text: SENTENCE, source: "slm" });
    expect(show).toHaveBeenCalledExactlyOnceWith(SENTENCE, "slm");
    expect(host.listeners.size).toBe(0);
    await waitFor(() => expect(host.store.get("plain:EV-2001")).toMatchObject({ text: SENTENCE, source: "slm" }));
  });

  it("still shows the sentence when the host sends it before the reply", async () => {
    host.call.mockImplementation(async () => {
      emit({ ev: "description", alertId: "EV-2001", text: SENTENCE, source: "slm" });
      return { text: TEMPLATE, slm: true };
    });
    const show = vi.fn();
    await describeAlert(alert, "CAM-01", "Rig 1", show);
    expect(show).toHaveBeenCalledExactlyOnceWith(SENTENCE, "slm");
  });

  it("shows the template at once when no model runs", async () => {
    host.call.mockResolvedValue({ text: TEMPLATE, slm: false });
    const show = vi.fn();
    await describeAlert(alert, "CAM-01", "Rig 1", show);
    expect(show).toHaveBeenCalledExactlyOnceWith(TEMPLATE, "template");
    expect(host.listeners.size).toBe(0);
  });

  it("falls back to the template on a null description and on timeout", async () => {
    host.call.mockResolvedValue({ text: TEMPLATE, slm: true });
    const a = vi.fn();
    await describeAlert(alert, "CAM-01", "Rig 1", a);
    emit({ ev: "description", alertId: "EV-2001", text: null, source: "template" });
    expect(a).toHaveBeenCalledExactlyOnceWith(TEMPLATE, "template");

    vi.useFakeTimers();
    const b = vi.fn();
    await describeAlert({ ...alert, id: "EV-2002" }, "CAM-01", "Rig 1", b, 1000);
    vi.advanceTimersByTime(1001);
    expect(b).toHaveBeenCalledExactlyOnceWith(TEMPLATE, "template");
    expect(host.listeners.size).toBe(0);
  });
});

const summary = (id: string, text: string, source: PlainSummary["source"]): PlainSummary => ({
  alertId: id, severity: "Critical", condition: "Platform-elevator collision", place: "Rig 1", camId: "CAM-01", text, source, at: `2026-10-01T10:0${id.slice(-1)}:00Z`,
});

describe("reports include plain-language summaries", () => {
  it("stored summaries are loaded oldest first and exported to CSV rows", async () => {
    host.store.set("plain:EV-2002", summary("EV-2002", TEMPLATE, "template"));
    host.store.set("plain:EV-2001", summary("EV-2001", SENTENCE, "slm"));
    const list = await loadPlainSummaries();
    expect(list.map((s) => s.alertId)).toEqual(["EV-2001", "EV-2002"]);
    const rows = plainSummaryCsvRows(list);
    expect(rows[0]).toMatchObject({ "Alert ID": "EV-2001", "Plain-language summary": SENTENCE, Source: "BitNet", "Camera · place": "CAM-01 · Rig 1" });
    expect(rows[1]).toMatchObject({ Source: "Template" });
  });

  it("the table shows each sentence and its source; long lists split over sheets", () => {
    render(<PlainSummaryTable rows={[summary("EV-2001", SENTENCE, "slm"), summary("EV-2002", TEMPLATE, "template")]} />);
    expect(screen.getByText(SENTENCE)).toBeInTheDocument();
    expect(screen.getByText("BitNet")).toBeInTheDocument();
    expect(screen.getByText("Template")).toBeInTheDocument();
    expect(chunkPlainSummaries(Array.from({ length: 17 }, (_, i) => summary(`EV-${i}`, "x", "slm"))).map((c) => c.length)).toEqual([8, 8, 1]);
  });

  it("the event register report lists stored summaries and adds a CSV column", async () => {
    host.store.set("plain:EV-2001", summary("EV-2001", SENTENCE, "slm"));
    render(<RegisterReport page="01 / 01" />);
    expect(await screen.findByText(SENTENCE)).toBeInTheDocument();
    expect(registerCsvRows([summary("EV-1042", SENTENCE, "slm")]).find((r) => r["Event ID"] === "EV-1042")?.["Plain-language summary"]).toBe(SENTENCE);
    expect(registerCsvRows([])[0]).toHaveProperty("Plain-language summary", "");
  });
});
