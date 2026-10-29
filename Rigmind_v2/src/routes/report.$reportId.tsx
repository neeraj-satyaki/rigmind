import { createFileRoute, useRouter } from "@tanstack/react-router";
import { ArrowLeft, Download, Printer } from "lucide-react";
import { reportTemplates, reportTitles, type ReportSearch } from "@/reports/templates";
import "@/reports/reports.css";

export const Route = createFileRoute("/report/$reportId")({
  validateSearch: (s: Record<string, unknown>): ReportSearch => ({
    eventId: typeof s["eventId"] === "string" ? s["eventId"] : undefined,
    actionId: typeof s["actionId"] === "string" ? s["actionId"] : undefined,
    type: typeof s["type"] === "string" ? s["type"] : undefined,
    week: typeof s["week"] === "string" && /^\d{4}-\d{2}-\d{2}$/.test(s["week"]) ? s["week"] : undefined,
  }),
  head: (ctx) => {
    const title = `${reportTitles[ctx.params.reportId] ?? "Report"} · RigMind`;
    return {
      meta: [
        { title },
        { name: "description", content: "Printable RigMind report." },
        { property: "og:title", content: title },
        { property: "og:description", content: "Printable RigMind report." },
        { property: "og:type", content: "website" },
        { name: "twitter:card", content: "summary" },
      ],
      links: [
        { rel: "preconnect", href: "https://fonts.googleapis.com" },
        { rel: "preconnect", href: "https://fonts.gstatic.com", crossOrigin: "anonymous" },
        {
          rel: "stylesheet",
          href: "https://fonts.googleapis.com/css2?family=Poppins:wght@400;500;600&display=swap",
        },
      ],
    };
  },
  component: ReportViewer,
});

function ReportViewer() {
  const { reportId } = Route.useParams();
  const search = Route.useSearch();
  const router = useRouter();
  const entry = reportTemplates[reportId] ?? reportTemplates["pack"];
  if (!entry) return null;
  const { Component, name, csv } = entry;
  return (
    <div className="rm-report">
      <div className="rm-toolbar">
        <button type="button" onClick={() => router.history.back()} aria-label="Back">
          <ArrowLeft size={15} /> Back
        </button>
        <span className="rm-toolbar__title">{name}</span>
        {csv && (
          <button type="button" onClick={csv}>
            <Download size={15} /> Download CSV
          </button>
        )}
        <button type="button" className="rm-toolbar__primary" onClick={() => window.print()}>
          <Printer size={15} /> Print / Save as PDF
        </button>
      </div>
      <Component search={search} />
    </div>
  );
}
