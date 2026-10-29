import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { AuditTrail } from "@/components/rig/GovernanceScreens";

export const Route = createFileRoute("/_shell/admin/audit")({
  head: pageHead("Audit trail", "Audit trail in RigMind safety operations."),
  component: AuditTrail,
});
