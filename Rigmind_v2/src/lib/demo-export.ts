import { toast } from "sonner";

export function exportCsv<T extends object>(name: string, rows: T[]) {
  const first = rows[0];
  if (!first) {
    toast.info("Nothing to export yet", { description: "There are no rows for this report. Rows appear as alerts and events are recorded." });
    return;
  }
  const headers = Object.keys(first) as (keyof T)[];
  // A value that starts with = + - @ would run as a formula in a spreadsheet: keep it as text.
  const cell = (v: unknown) => {
    const text = String(v ?? "");
    return `"${(/^[=+\-@\t\r]/.test(text) ? `'${text}` : text).replaceAll('"', '""')}"`;
  };
  const content = [
    headers.map(cell).join(","),
    ...rows.map((row) => headers.map((key) => cell(row[key])).join(",")),
  ].join("\r\n");
  // The byte-order mark makes Excel read the file as UTF-8 (dots, dashes and accents stay readable).
  const url = URL.createObjectURL(new Blob(["﻿", content, "\r\n"], { type: "text/csv;charset=utf-8" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = `${name}.csv`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10_000); // not at once: the download may still be reading the blob
}
export async function exportPdf(title: string, lines: string[]) {
  const { jsPDF } = await import("jspdf");
  const pdf = new jsPDF();
  pdf.setFontSize(18);
  pdf.text(`RigMind - ${title}`, 20, 22);
  pdf.setFontSize(10);
  let y = 35;
  for (const line of ["DEMONSTRATION DATA - 06 October 2026 / Day shift", ...lines]) {
    const wrapped = pdf.splitTextToSize(line, 170);
    if (y + wrapped.length * 5 > 275) {
      pdf.addPage();
      y = 20;
    }
    pdf.text(wrapped, 20, y);
    y += wrapped.length * 5 + 5;
  }
  pdf.save(`rigmind-${title.toLowerCase().replaceAll(" ", "-")}.pdf`);
}
