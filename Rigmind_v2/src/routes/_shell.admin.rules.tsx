import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { SafetyRules } from "@/components/rig/GovernanceScreens";

export const Route = createFileRoute("/_shell/admin/rules")({
  head: pageHead("Monitoring configuration", "Monitoring configuration in RigMind safety operations."),
  component: SafetyRules,
});
