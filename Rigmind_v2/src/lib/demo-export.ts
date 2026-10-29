export function exportCsv<T extends object>(name: string, rows: T[]) {
  const first = rows[0];
  if (!first) return;
  const headers = Object.keys(first) as (keyof T)[];
  const cell = (v: unknown) => `"${String(v ?? "").replaceAll('"', '""')}"`;
  const content = [
    headers.map(cell).join(","),
    ...rows.map((row) => headers.map((key) => cell(row[key])).join(",")),
  ].join("\r\n");
  const url = URL.createObjectURL(new Blob([content], { type: "text/csv;charset=utf-8" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = `${name}.csv`;
  link.click();
  URL.revokeObjectURL(url);
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
