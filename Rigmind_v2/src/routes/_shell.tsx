import { useEffect } from "react";
import { createFileRoute, Outlet, redirect, useNavigate } from "@tanstack/react-router";
import { authRequired, clearSession, getSession } from "@/lib/auth";
import { SIGN_IN_REQUIRED_EVENT } from "@/lib/native-bridge";
import { AppShell } from "@/components/rig/AppShell";

export const Route = createFileRoute("/_shell")({
  // Inside the desktop app every workspace page needs a signed-in account; the sign-in page is the only way in.
  beforeLoad: () => {
    if (authRequired() && !getSession()) throw redirect({ to: "/login" });
  },
  component: ShellWithSignInGuard,
});

// The real check is in the host (it refuses every call without a session). This only returns the page to the sign-in screen
// when the host says the session has ended.
function ShellWithSignInGuard() {
  const nav = useNavigate();
  useEffect(() => {
    const back = () => {
      clearSession();
      void nav({ to: "/login" });
    };
    window.addEventListener(SIGN_IN_REQUIRED_EVENT, back);
    return () => window.removeEventListener(SIGN_IN_REQUIRED_EVENT, back);
  }, [nav]);
  return (
    <AppShell>
      <Outlet />
    </AppShell>
  );
}
