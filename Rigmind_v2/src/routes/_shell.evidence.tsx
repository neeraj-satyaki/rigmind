import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { EvidenceLibrary } from "@/components/rig/ReviewScreens";
import { EvidenceChat } from "@/components/rig/EvidenceChat";

export const Route = createFileRoute("/_shell/evidence")({
  head: pageHead("Evidence library", "Evidence library in RigMind safety operations."),
  component: EvidencePage,
});

function EvidencePage() {
  return (
    <>
      <EvidenceLibrary />
      <EvidenceChat />
    </>
  );
}
