import { useEffect, useState } from "react";
import { useRouter, useRouterState } from "@tanstack/react-router";

export const SIMULATED_PAGE_DELAY_MS: number = 700;

/** Minimum navigation duration; no timers or subscriptions run during SSR. */
export function usePageTransition() {
  const router = useRouter();
  const isLoading = useRouterState({ select: (state) => state.isLoading });
  const [delaying, setDelaying] = useState(false);
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const unsubscribe = router.subscribe("onBeforeNavigate", ({ fromLocation, toLocation }) => {
      if (fromLocation?.href === toLocation.href) return;
      clearTimeout(timer);
      if (SIMULATED_PAGE_DELAY_MS === 0) return;
      setDelaying(true);
      timer = setTimeout(() => setDelaying(false), SIMULATED_PAGE_DELAY_MS);
    });
    return () => {
      unsubscribe();
      clearTimeout(timer);
    };
  }, [router]);
  return isLoading || delaying;
}