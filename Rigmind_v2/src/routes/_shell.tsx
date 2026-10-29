import { createFileRoute, Outlet } from "@tanstack/react-router";
import { AppShell } from "@/components/rig/AppShell";

export const Route = createFileRoute("/_shell")({
  component: () => (
    <AppShell>
      <Outlet />
    </AppShell>
  ),
});
