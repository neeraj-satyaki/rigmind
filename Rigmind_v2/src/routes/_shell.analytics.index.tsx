import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { SafetyAnalytics } from "@/components/rig/AnalyticsScreens";

export const Route = createFileRoute("/_shell/analytics/")({
  head: pageHead("Safety analytics", "Safety analytics in RigMind safety operations."),
  component: SafetyAnalytics,
});
