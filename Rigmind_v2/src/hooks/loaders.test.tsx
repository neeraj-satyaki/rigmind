import { act, cleanup, renderHook } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { renderToString } from "react-dom/server";
import { useBootSequence } from "./useBootSequence";
import { usePageTransition } from "./usePageTransition";

const navigation = vi.hoisted(() => ({
  listener: undefined as undefined | ((event: { fromLocation: { href: string }; toLocation: { href: string } }) => void),
  loading: false,
  unsubscribe: vi.fn(),
}));
vi.mock("@tanstack/react-router", () => ({
  useRouter: () => router,
  useRouterState: () => navigation.loading,
}));
const router = {
  subscribe: vi.fn((_event: string, listener: typeof navigation.listener) => {
    navigation.listener = listener;
    return navigation.unsubscribe;
  }),
};

beforeEach(() => { vi.useFakeTimers(); navigation.loading = false; vi.clearAllMocks(); });
afterEach(() => { cleanup(); vi.useRealTimers(); });

describe("RigMind startup timing", () => {
  it("advances all five boot steps every 600ms and fades for 400ms after Ready", () => {
    const { result } = renderHook(useBootSequence);
    expect(result.current).toEqual({ visible: true, progress: 0, status: "Starting services", done: false });
    const steps = ["Connecting to rig-local edge", "Loading safety rules", "Syncing events", "Preparing workspace", "Ready"];
    steps.forEach((status, index) => {
      act(() => vi.advanceTimersByTime(600));
      expect(result.current.status).toBe(status);
      expect(result.current.progress).toBe((index + 1) / 5);
      expect(result.current.visible).toBe(true);
    });
    expect(result.current.done).toBe(true);
    act(() => vi.advanceTimersByTime(399));
    expect(result.current.visible).toBe(true);
    act(() => vi.advanceTimersByTime(1));
    expect(result.current.visible).toBe(false);
  });

  it("starts no timers while rendering on the server", () => {
    function Startup() { const boot = useBootSequence(); return <span>{boot.status}</span>; }
    renderToString(<Startup />);
    expect(vi.getTimerCount()).toBe(0);
  });

  it("cleans up startup timers on unmount", () => {
    const { unmount } = renderHook(useBootSequence);
    unmount();
    expect(vi.getTimerCount()).toBe(0);
  });
});

describe("RigMind page transitions", () => {
  it("keeps fast navigations loading for exactly 700ms and restarts for another navigation", () => {
    const { result, unmount } = renderHook(usePageTransition);
    expect(result.current).toBe(false);
    act(() => navigation.listener?.({ fromLocation: { href: "/fleet" }, toLocation: { href: "/rig" } }));
    expect(result.current).toBe(true);
    act(() => vi.advanceTimersByTime(699));
    expect(result.current).toBe(true);
    act(() => vi.advanceTimersByTime(1));
    expect(result.current).toBe(false);
    act(() => navigation.listener?.({ fromLocation: { href: "/rig" }, toLocation: { href: "/alerts" } }));
    expect(result.current).toBe(true);
    unmount();
    expect(navigation.unsubscribe).toHaveBeenCalled();
    expect(vi.getTimerCount()).toBe(0);
  });

  it("keeps loading after the 700ms minimum when the router is still loading", () => {
    const { result, rerender } = renderHook(usePageTransition);
    navigation.loading = true;
    act(() => navigation.listener?.({ fromLocation: { href: "/fleet" }, toLocation: { href: "/live" } }));
    act(() => vi.advanceTimersByTime(700));
    expect(result.current).toBe(true);
    navigation.loading = false;
    rerender();
    expect(result.current).toBe(false);
  });
});