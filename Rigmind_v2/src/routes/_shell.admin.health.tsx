import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { SystemHealth } from "@/components/rig/GovernanceScreens";

export const Route = createFileRoute("/_shell/admin/health")({
  head: pageHead("System health", "System health in RigMind safety operations."),
  component: SystemHealth,
});
