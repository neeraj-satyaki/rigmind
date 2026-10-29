import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { LicenseCapacity } from "@/components/rig/GovernanceScreens";

export const Route = createFileRoute("/_shell/admin/license")({
  head: pageHead("License & capacity", "License & capacity in RigMind safety operations."),
  component: LicenseCapacity,
});
