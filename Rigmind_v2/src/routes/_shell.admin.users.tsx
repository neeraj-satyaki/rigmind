import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { UsersAccess } from "@/components/rig/CoordinationScreens";

export const Route = createFileRoute("/_shell/admin/users")({
  head: pageHead("Users & access", "Users & access in RigMind safety operations."),
  component: UsersAccess,
});
