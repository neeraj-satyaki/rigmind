// What the page shows for an alert. The host stores facts only (alert metadata) and a neutral level label; the one plain-English
// description of an alert is the SLM's sentence (`summary`). Until it exists - and when no model runs - the page shows the level
// label, the camera and a plain list of the facts that were true. That list is derived here, deterministically, from the
// metadata: it is a list of labelled facts, never a sentence.

export type AlertFacts = {
  machine_present?: boolean;
  link_tilt_present?: boolean;
  link_tilt_angled?: boolean;
  elevator_present?: boolean;
  elevator_near_platform?: boolean;
  angle_deg?: number | null;
  derrickman_cx?: number | null;
  machine_cx?: number | null;
  w?: number;
  h?: number;
};
export type AlertState = { state: string; tsMs: number; frame?: number; facts: AlertFacts | null };
export type AlertMeta = {
  kind?: "alarm" | "ppe" | string;
  camera?: string;
  peak?: string;
  cycle?: string;
  closed?: boolean;
  states?: AlertState[];
  missing?: string[];
  persons?: number;
  since_ms?: number;
};

/** Fields the host adds to every alerts.feed row. */
export type SummaryFields = {
  metadata?: AlertMeta | null | undefined;
  /** The SLM sentence; null until the model has written it. */
  summary?: string | null | undefined;
  /** 'slm' | 'template' (the model gave nothing) | 'unavailable' (no model runs); null while none was requested. */
  summarySource?: string | null | undefined;
  summaryAt?: number | null | undefined;
};

export type SummaryStatus = "ready" | "pending" | "unavailable" | "none";

export const CHIP_PENDING = "Summary pending";
export const CHIP_UNAVAILABLE = "BitNet not available";

/** Human labels for the facts that are true in one snapshot, e.g. ["machine present", "link tilt angled 28°"]. */
export function factLabels(f: AlertFacts | null | undefined): string[] {
  if (!f || typeof f !== "object") return [];
  const out: string[] = [];
  const angle = typeof f.angle_deg === "number" && Number.isFinite(f.angle_deg) ? `${Math.round(f.angle_deg)}°` : "";
  if (f.machine_present) out.push("machine present");
  if (f.link_tilt_angled) out.push(angle ? `link tilt angled ${angle}` : "link tilt angled");
  else if (f.link_tilt_present) out.push(angle ? `link tilt present ${angle}` : "link tilt present");
  if (f.elevator_near_platform) out.push("elevator near platform");
  else if (f.elevator_present) out.push("elevator present");
  return out;
}

const LEVEL: Record<string, number> = { None: -1, Yellow: 0, Orange: 1, Red: 2 };

/** The state change with the highest level reached that carries facts (the latest one at that level). */
export function peakState(meta: AlertMeta | null | undefined): AlertState | undefined {
  const states = Array.isArray(meta?.states) ? meta!.states! : [];
  let best: AlertState | undefined;
  for (const s of states) {
    if (!s || !s.facts) continue;
    if (!best || (LEVEL[s.state] ?? -1) >= (LEVEL[best.state] ?? -1)) best = s;
  }
  return best;
}

/** "elevator near platform, link tilt angled 28°" (alarm) or "vest, gloves not detected · 2 people" (PPE). Empty when unknown. */
export function metadataFacts(meta: AlertMeta | null | undefined): string {
  if (!meta || typeof meta !== "object") return "";
  if (meta.kind === "ppe") {
    const missing = Array.isArray(meta.missing) ? meta.missing.filter((m) => typeof m === "string") : [];
    const parts: string[] = [];
    if (missing.length) parts.push(`${missing.join(", ")} not detected`);
    if (typeof meta.persons === "number" && meta.persons > 0) parts.push(`${meta.persons} ${meta.persons === 1 ? "person" : "people"}`);
    return parts.join(" · ");
  }
  return factLabels(peakState(meta)?.facts).join(", ");
}

/** True for a row that came from the host (it always carries metadata / summary keys, null when empty). */
export const isHostRow = (a: object) => "metadata" in a || "summary" in a || "summarySource" in a;

export function summaryStatus(a: SummaryFields): SummaryStatus {
  if (typeof a.summary === "string" && a.summary.trim()) return "ready";
  if (a.summarySource === "unavailable" || a.summarySource === "template") return "unavailable";
  // A Yellow-only alert is only being watched: no summary is written for it.
  if (a.metadata?.kind === "alarm" && (a.metadata.peak ?? "Yellow") === "Yellow") return "none";
  return "pending";
}

export const chipFor = (status: SummaryStatus): string | null =>
  status === "pending" ? CHIP_PENDING : status === "unavailable" ? CHIP_UNAVAILABLE : null;

export type AlertDescription = {
  /** The SLM sentence when it exists, else "<level label> · <camera> · <facts>". */
  text: string;
  /** True when `text` is the SLM sentence. */
  isSummary: boolean;
  status: SummaryStatus;
  /** "Summary pending" | "BitNet not available" | null */
  chip: string | null;
  /** The facts list alone (may be empty). */
  facts: string;
};

/** The text to show for one alert row. Rows that did not come from the host (sample data) show their condition unchanged. */
export function alertDescription(a: { condition?: string | undefined; place?: string | undefined } & SummaryFields): AlertDescription {
  if (!isHostRow(a)) return { text: a.condition ?? "", isSummary: false, status: "none", chip: null, facts: "" };
  const status = summaryStatus(a);
  const facts = metadataFacts(a.metadata);
  if (status === "ready") return { text: a.summary!, isSummary: true, status, chip: null, facts };
  const camera = a.metadata?.camera || a.place || "";
  const text = [a.condition, camera, facts].filter((p) => p && p.length).join(" · ");
  return { text, isSummary: false, status, chip: chipFor(status), facts };
}

/** One line for printed reports and CSV: the description, then the status in brackets while the sentence is missing. */
export function alertDescriptionLine(a: { condition?: string | undefined; place?: string | undefined } & SummaryFields): string {
  const d = alertDescription(a);
  return d.chip ? `${d.text} [${d.chip}]` : d.text;
}
