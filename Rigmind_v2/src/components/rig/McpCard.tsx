import { useEffect, useState } from "react";
import { toast } from "sonner";
import { Btn, Card, CardHead, Chip, Mono, Row } from "./ui";
import { isNative, nativeCall } from "@/lib/native-bridge";

type McpStatus = { running: boolean; url: string; tokenFile: string; enabled: boolean; tools: number };

const TOOLS = [
  "list_alerts",
  "get_alert",
  "get_live_scene",
  "list_evidence",
  "get_evidence",
  "list_summaries",
  "report_summary",
  "list_reports",
  "get_audit",
  "request_summary",
];

/**
 * The MCP module: a local, token-protected endpoint that lets an MCP client (or the summary model) read alerts, evidence,
 * reports, the audit trail and the live inference scene. The token stays in a file; the page only shows where it is.
 */
export function McpCard() {
  const [status, setStatus] = useState<McpStatus | null>(null);
  useEffect(() => {
    if (!isNative()) return;
    nativeCall<McpStatus>("mcp.status", {})
      .then(setStatus)
      .catch(() => setStatus(null));
  }, []);
  if (!isNative() || !status) return null;

  const toggle = async () => {
    try {
      setStatus(await nativeCall<McpStatus>("mcp.set", { enabled: !status.enabled }));
      toast.success(status.enabled ? "MCP endpoint will stay off from the next start" : "MCP endpoint will start with RigMind next time");
    } catch (e) {
      toast.error("Could not change the MCP setting", { description: String(e) });
    }
  };
  const copy = () =>
    navigator.clipboard
      ?.writeText(status.url)
      .then(() => toast.success("Endpoint address copied"))
      .catch(() => toast.error("Could not copy the address"));

  return (
    <Card className="mt-4">
      <CardHead title="MCP module" sub="Alerts, evidence, reports and the live scene for the summary model and MCP clients" />
      <div className="p-4">
        <Row k="Endpoint" v={<Chip tone={status.running ? "success" : "neutral"}>{status.running ? "Running" : status.enabled ? "Not running" : "Disabled"}</Chip>} />
        {status.running && (
          <>
            <Row k="Address" v={<Mono>{status.url}</Mono>} />
            <Row k="Token file" v={<Mono className="break-all">{status.tokenFile}</Mono>} />
          </>
        )}
        <Row k="Access" v="This PC only (127.0.0.1), token required, read-only except request_summary" />
        <Row k="Tools" v={<span className="text-xs">{TOOLS.join(" · ")}</span>} />
        <p className="mt-3 text-xs text-muted-foreground">
          Add it to an MCP client with the address above and the header <Mono>Authorization: Bearer &lt;token from the file&gt;</Mono>.
          Every tool call is written to the audit trail.
        </p>
        <div className="mt-3 flex flex-wrap gap-2">
          {status.running && <Btn onClick={copy}>Copy address</Btn>}
          <Btn onClick={toggle}>{status.enabled ? "Turn off at next start" : "Turn on at next start"}</Btn>
        </div>
      </div>
    </Card>
  );
}
