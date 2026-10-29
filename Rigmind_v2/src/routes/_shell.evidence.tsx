import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { EvidenceLibrary } from "@/components/rig/ReviewScreens";

export const Route = createFileRoute("/_shell/evidence")({
  head: pageHead("Evidence library", "Evidence library in RigMind safety operations."),
  component: EvidenceLibrary,
});
