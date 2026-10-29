import { listDevice, readDevice, writeDevice } from "./device-db";
import type { EventSummaryDraft } from "./event-summary.functions";

export type HseApproval = { by: string; role: string; at: string };
export type StoredSummary = {
  eventId: string;
  draft: EventSummaryDraft;
  sources: string[];
  generatedAt: string;
  approval: HseApproval | null;
};
export type StoredReview = { classification: string; notes: string; savedAt: string; by: string };

const sKey = (id: string) => `summary:${id}`;
const rKey = (id: string) => `review:${id}`;

export const loadSummary = (id: string) => readDevice<StoredSummary>(sKey(id));
export const saveSummary = (s: StoredSummary) => writeDevice(sKey(s.eventId), s);
export const loadReview = (id: string) => readDevice<StoredReview>(rKey(id));
export const saveReview = (id: string, r: StoredReview) => writeDevice(rKey(id), r);

/** Only summaries an HSE reviewer approved may appear in reports. */
export function reportableSummaries(all: StoredSummary[]) {
  return all.filter((s) => s.approval !== null);
}
export async function loadApprovedSummaries() {
  return reportableSummaries((await listDevice<StoredSummary>("summary:")).map((r) => r.value));
}
