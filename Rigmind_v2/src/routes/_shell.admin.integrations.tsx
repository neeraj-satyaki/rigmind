import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { Integrations } from "@/components/rig/GovernanceScreens";

export const Route = createFileRoute("/_shell/admin/integrations")({
  head: pageHead("SCADA & alarm integration", "SCADA & alarm integration in RigMind safety operations."),
  component: Integrations,
});
