import { isNative, nativeCall, onNativeEvent } from "./native-bridge";
import { alertDescription, summaryStatus, type SummaryFields, type SummaryStatus } from "./alert-text";

/*
 * Plain-language text for alerts. The host builds the model prompt from the alert's stored metadata, asks the SLM (automatically for
 * every Orange/Red cycle close, every Red opening and every PPE opening) and stores the sentence on the alert row. The page only
 * reads it from there (alerts.feed / alert.get); the 'Plain words' button asks for a new one.
 */

/** One alert as the reports list it: the SLM sentence when it exists, else the label, camera and facts with a status. */
export type PlainSummary = {
  alertId: string;
  severity: string;
  condition: string;
  place: string;
  camId: string;
  /** The SLM sentence; empty until the model has written it. */
  text: string;
  /** Facts that were true (a plain list, not a sentence). */
  facts: string;
  source: "slm" | "none";
  status: SummaryStatus;
  /** "Summary pending" | "BitNet not available" | null */
  chip: string | null;
  at: string;
};

/** An alerts.feed row. */
export type FeedRow = {
  id: string;
  severity: string;
  condition: string;
  place: string;
  phase?: string;
  raisedMs: number;
} & SummaryFields;

/** Report line for one feed row; null for rows that get no summary (a Yellow-only stretch). */
export function plainSummaryFromRow(row: FeedRow): PlainSummary | null {
  const status = summaryStatus(row);
  if (status === "none") return null;
  const d = alertDescription(row);
  const at = typeof row.summaryAt === "number" ? row.summaryAt : row.raisedMs;
  return {
    alertId: row.id,
    severity: row.severity,
    condition: row.condition,
    place: row.place,
    camId: row.metadata?.camera || row.place,
    text: d.isSummary ? d.text : "",
    facts: d.facts,
    source: d.isSummary ? "slm" : "none",
    status,
    chip: d.chip,
    at: new Date(Number.isFinite(at) ? at : 0).toISOString(),
  };
}

/** The text a report shows for one alert: the sentence, or label · camera · facts. */
export const summaryReportText = (s: PlainSummary) => s.text || [s.condition, s.camId, s.facts].filter(Boolean).join(" · ");

/** Alerts that have (or await) a plain-language summary, oldest first, read from the host rows. Empty outside the desktop host. */
export async function loadPlainSummaries(): Promise<PlainSummary[]> {
  if (!isNative()) return [];
  const rows = await nativeCall<FeedRow[]>("alerts.feed", { limit: 500 });
  if (!Array.isArray(rows)) return [];
  return rows
    .map(plainSummaryFromRow)
    .filter((s): s is PlainSummary => s !== null)
    .sort((a, b) => a.at.localeCompare(b.at));
}

/** Report text for one summary, e.g. "EV-2001 · Critical · Red alert". */
export const summaryLabel = (s: PlainSummary) => `${s.alertId} · ${s.severity} · ${s.condition}`;

/** Row shape of the CSV export. */
export const plainSummaryCsvRows = (list: PlainSummary[]) =>
  list.map((s) => ({
    "Alert ID": s.alertId,
    Time: s.at,
    Severity: s.severity,
    "Level label": s.condition,
    "Camera · place": [s.camId, s.place].filter((v, i, a) => v && a.indexOf(v) === i).join(" · "),
    "Facts": s.facts,
    "Plain-language summary": s.text,
    Source: s.source === "slm" ? "BitNet" : (s.chip ?? ""),
  }));

/** Asks the host for a new sentence for one alert (the 'Plain words' button). The text arrives on the alert row. */
export function requestSummary(alertId: string, sceneAtMs = 0) {
  return nativeCall<{ alertId: string; slm: boolean }>("event.describe", { alertId, tsMs: Date.now(), sceneTs: sceneAtMs });
}

/** One alert row from the host (metadata and summary included); null when unknown. */
export async function fetchAlertRow(alertId: string): Promise<FeedRow | null> {
  const row = await nativeCall<FeedRow | null>("alert.get", { id: alertId });
  return row && typeof row === "object" ? row : null;
}

/** Calls onDescription(alertId) whenever the host reports a finished summary (any alert). Returns the unsubscribe function. */
export function onSummaryReady(onDescription: (alertId: string) => void): () => void {
  return onNativeEvent((msg) => {
    if (msg.ev === "description" && typeof msg["alertId"] === "string") onDescription(msg["alertId"] as string);
  });
}
