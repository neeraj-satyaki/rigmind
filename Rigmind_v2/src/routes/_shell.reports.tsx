import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { Reports } from "@/components/rig/CoordinationScreens";

export const Route = createFileRoute("/_shell/reports")({
  head: pageHead("Reports & pilot readiness", "Reports & pilot readiness in RigMind safety operations."),
  component: Reports,
});
