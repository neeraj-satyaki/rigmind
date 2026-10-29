import { DataTable, SeverityPill } from "./components";
import { plainSummaryCsvRows, type PlainSummary } from "@/lib/plain-summary";

/** Rows per printed sheet. */
export const PLAIN_ROWS_PER_SHEET = 8;

export function chunkPlainSummaries(list: PlainSummary[], size = PLAIN_ROWS_PER_SHEET): PlainSummary[][] {
  const out: PlainSummary[][] = [];
  for (let i = 0; i < list.length; i += size) out.push(list.slice(i, i + size));
  return out;
}

/** Plain-language sentence per alert, marked BitNet or template. */
export function PlainSummaryTable({ rows }: { rows: PlainSummary[] }) {
  const csv = plainSummaryCsvRows(rows);
  return (
    <DataTable
      head={[
        <th key="i">Alert</th>,
        <th key="s">Severity</th>,
        <th key="t">Plain-language summary</th>,
        <th key="o">Source</th>,
      ]}
    >
      {rows.map((r, i) => (
        <tr key={r.alertId}>
          <td>{r.alertId}</td>
          <td><SeverityPill sev={r.severity as Parameters<typeof SeverityPill>[0]["sev"]} /></td>
          <td>{r.text}</td>
          <td>{csv[i]?.Source}</td>
        </tr>
      ))}
    </DataTable>
  );
}
