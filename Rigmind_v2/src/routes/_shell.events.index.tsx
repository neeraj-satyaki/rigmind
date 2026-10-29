import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { EventHistory } from "@/components/rig/ReviewScreens";

export const Route = createFileRoute("/_shell/events/")({
  head: pageHead("Event history", "Event history in RigMind safety operations."),
  component: EventHistory,
});
