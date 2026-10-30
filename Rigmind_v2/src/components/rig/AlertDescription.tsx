import { alertDescription, type SummaryFields } from "@/lib/alert-text";

/** Small status chip next to an alert description: "Summary pending" or "BitNet not available". */
export function SummaryChip({ chip }: { chip: string | null }) {
  if (!chip) return null;
  return (
    <span
      data-testid="summary-chip"
      className="ml-1 inline-block rounded-full border px-1.5 py-px align-middle text-[10px] font-semibold uppercase tracking-wide opacity-80"
    >
      {chip}
    </span>
  );
}

/**
 * Description of one alert: the SLM sentence once it exists; before that (and with no model) the level label, the camera and
 * the plain list of facts that were true, with a status chip. Sample-data rows show their condition unchanged.
 */
export function AlertDescription({
  alert,
  className,
}: {
  alert: { condition?: string | undefined; place?: string | undefined } & SummaryFields;
  className?: string;
}) {
  const d = alertDescription(alert);
  return (
    <span className={className} data-summary={d.isSummary ? "slm" : "facts"}>
      {d.text}
      <SummaryChip chip={d.chip} />
    </span>
  );
}
