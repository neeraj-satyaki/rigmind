import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { EventOverview } from "@/components/rig/AnalyticsScreens";

export const Route = createFileRoute("/_shell/analytics/overview")({
  head: pageHead("Event overview", "Event overview in RigMind safety operations."),
  component: EventOverview,
});
