import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { ZoneEditor } from "@/components/rig/ZoneEditor";

export const Route = createFileRoute("/_shell/admin/zones")({
  head: pageHead("Zone editor", "Zone editor in RigMind safety operations."),
  component: ZoneEditor,
});
