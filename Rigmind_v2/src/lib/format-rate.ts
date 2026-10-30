/**
 * Stream speed for display: megabits per second (one decimal) from 1 Mbps up, and only below that kilobits per second.
 * The pipeline reports kilobits per second.
 *   formatRate(4200) -> "4.2 Mbps"    formatRate(950) -> "950 kbps"    formatRate(0) -> "0 kbps"
 */
export function formatRate(kbps: number): string {
  const v = Number.isFinite(kbps) && kbps > 0 ? kbps : 0;
  return v >= 1000 ? `${(v / 1000).toFixed(1)} Mbps` : `${Math.round(v)} kbps`;
}
