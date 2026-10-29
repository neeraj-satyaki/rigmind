import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { ActionsHandover } from "@/components/rig/CoordinationScreens";

export const Route = createFileRoute("/_shell/actions")({
  head: pageHead("Actions & handover", "Actions & handover in RigMind safety operations."),
  component: ActionsHandover,
});
