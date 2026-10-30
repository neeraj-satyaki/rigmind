import { fireEvent, render, screen } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({ calls: [] as { method: string; params: Record<string, unknown> }[] }));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  onNativeEvent: () => () => {},
  nativeCall: async (method: string, params: Record<string, unknown>) => {
    h.calls.push({ method, params });
    return {};
  },
}));

import { DetectionControls } from "@/components/rig/DetectionControls";
import { currentOverlayPrefs, setOverlay } from "@/lib/detections";

describe("hide inference button", () => {
  beforeEach(() => {
    h.calls.length = 0;
    setOverlay("ppe", true);
    setOverlay("idss", true);
  });

  it("hides every detection and tells the pipeline, then shows them again", () => {
    render(<DetectionControls camId="CAM-01" />);
    const button = screen.getByTestId("raw-frame-toggle");
    expect(button.textContent).toBe("Hide inference");
    fireEvent.click(button);
    expect(currentOverlayPrefs()).toEqual({ ppe: false, idss: false });
    expect(h.calls.at(-1)).toEqual({ method: "stream.overlay", params: { cam: "CAM-01", ppe: false, idss: false } });
    expect(screen.getByTestId("raw-frame-toggle").textContent).toBe("Show inference");
    fireEvent.click(screen.getByTestId("raw-frame-toggle"));
    expect(currentOverlayPrefs()).toEqual({ ppe: true, idss: true });
    expect(h.calls.at(-1)).toEqual({ method: "stream.overlay", params: { cam: "CAM-01", ppe: true, idss: true } });
  });

  it("the single model buttons still work and send the other model's state", () => {
    render(<DetectionControls camId="CAM-01" />);
    fireEvent.click(screen.getByRole("button", { name: /PPE/ }));
    expect(h.calls.at(-1)?.params).toMatchObject({ cam: "CAM-01", ppe: false, idss: true });
  });
});
