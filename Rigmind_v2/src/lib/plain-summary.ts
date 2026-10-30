import { listDevice, writeDevice } from "./device-db";
import { nativeCall, onNativeEvent, type NativeEvent } from "./native-bridge";
import type { RaisedAlert } from "./alert-bus";

/** Plain-language text for one raised alert: the SLM sentence, or the template text when the model gave none. */
export type PlainSummary = {
  alertId: string;
  severity: string;
  condition: string;
  place: string;
  camId: string;
  text: string;
  source: "slm" | "template";
  at: string;
};

type DescribeReply = { text: string; slm: boolean };

/** Picks what to show for a host "description" event: its text from the SLM, else the template text. */
export function resolveDescription(
  msg: Record<string, unknown>,
  templateText: string,
): { text: string; source: "slm" | "template" } {
  const text = msg["text"];
  if (msg["source"] === "slm" && typeof text === "string" && text.trim()) return { text, source: "slm" };
  return { text: templateText, source: "template" };
}

const key = (alertId: string) => `plain:${alertId}`;
export const savePlainSummary = (s: PlainSummary) => writeDevice(key(s.alertId), s);
export async function loadPlainSummaries(): Promise<PlainSummary[]> {
  const rows = await listDevice<PlainSummary>("plain:");
  return rows.map((r) => r.value).sort((a, b) => a.at.localeCompare(b.at));
}

/** Report text for one summary, e.g. "EV-2001 · Critical · Platform-elevator collision". */
export const summaryLabel = (s: PlainSummary) => `${s.alertId} · ${s.severity} · ${s.condition}`;

/** Row shape of the CSV export. */
export const plainSummaryCsvRows = (list: PlainSummary[]) =>
  list.map((s) => ({
    "Alert ID": s.alertId,
    Time: s.at,
    Severity: s.severity,
    Condition: s.condition,
    "Camera · place": [s.camId, s.place].filter(Boolean).join(" · "),
    "Plain-language summary": s.text,
    Source: s.source === "slm" ? "BitNet" : "Template",
  }));

const SLM_WAIT_MS = 300_000; // summaries queue behind each other on the CPU

/**
 * Asks the host for plain words for one alert, then calls show(text) once: the template text at once when
 * no model runs, otherwise the model sentence (or the template text if it never answers). The result is
 * stored so reports can include it.
 *
 * The listener is registered before the call: the host may send the description event before its reply.
 */
export async function describeAlert(
  a: RaisedAlert,
  camId: string,
  place: string,
  show: (text: string, source: "slm" | "template") => void,
  waitMs = SLM_WAIT_MS,
  /** When the alert happened, for the scene shown to the model; 0 = no scene (an old alert). Default: now. */
  sceneAtMs: number = Date.now(),
) {
  let early: NativeEvent | undefined;
  let deliver: ((msg: NativeEvent) => void) | undefined;
  const off = onNativeEvent((msg) => {
    if (msg.ev !== "description" || msg["alertId"] !== a.id) return;
    if (deliver) deliver(msg);
    else early = msg;
  });
  try {
    const reply = await nativeCall<DescribeReply>("event.describe", {
      alertId: a.id,
      severity: a.severity,
      condition: a.condition,
      place,
      camId,
      tsMs: Date.now(),
      sceneTs: sceneAtMs,
    });
    const finish = (msg: Record<string, unknown>) => {
      off();
      const r = resolveDescription(msg, reply.text);
      show(r.text, r.source);
      savePlainSummary({
        alertId: a.id,
        severity: a.severity,
        condition: a.condition,
        place,
        camId,
        text: r.text,
        source: r.source,
        at: new Date().toISOString(),
      }).catch((e) => console.warn("plain summary not stored", e));
    };
    if (!reply.slm) return finish({ text: null, source: "template" });
    if (early) return finish(early);
    const timer = setTimeout(() => finish({ text: null, source: "template" }), waitMs);
    deliver = (msg) => {
      clearTimeout(timer);
      finish(msg);
    };
  } catch (e) {
    off();
    console.warn("event.describe failed", e);
  }
}
