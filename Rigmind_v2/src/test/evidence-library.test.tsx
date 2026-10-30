import { beforeEach, describe, expect, it, vi } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";

const host = vi.hoisted(() => ({ call: vi.fn() }));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  nativeCall: (...a: unknown[]) => host.call(...a),
  onNativeEvent: () => () => {},
}));

import { EvidenceLibrary } from "@/components/rig/ReviewScreens";

const ts = 1_800_000_000_000;
const orange = { id: 2, camId: "CAM-01", timestampMs: ts + 5000, clipCount: 3, path: `CAM-01/${ts + 5000}`, expiresMs: ts + 9e8, kind: "image" };
const red = { id: 1, camId: "CAM-01", timestampMs: ts, clipCount: 2, path: `CAM-01/${ts}`, expiresMs: ts + 9e8, kind: "video" };

function serve(rows: unknown[]) {
  host.call.mockImplementation(async (method: string, p: { path?: string }) => {
    if (method === "footage.list") return rows;
    if (method === "footage.files")
      return p.path === orange.path
        ? { files: [], images: ["orange-1.jpg", "orange-2.jpg", "orange-3.jpg"], kind: "image" }
        : { files: ["seg00001.mp4"], images: [], kind: "video" };
    return null; // other calls (alert store, audit) are not under test
  });
}

beforeEach(() => host.call.mockReset());

describe("Evidence library", () => {
  it("shows Orange sets as images and Red sets as video", async () => {
    serve([orange, red]);
    const { container } = render(<EvidenceLibrary />);
    await waitFor(() => expect(screen.getByText("Orange - image evidence")).toBeTruthy());
    expect(screen.getByText("Red - video evidence")).toBeTruthy();
    await waitFor(() => expect(container.querySelectorAll('[data-testid="image-evidence"] img').length).toBe(4)); // 1 large + 3 thumbs
    const srcs = [...container.querySelectorAll("img")].map((i) => i.getAttribute("src"));
    expect(srcs).toContain(`https://footage.rigmind.app/${orange.path}/orange-1.jpg`);
    expect(srcs).toContain(`https://footage.rigmind.app/${orange.path}/orange-3.jpg`);
    // The Red set has no player until it is opened, and the Orange set never gets one.
    expect(container.querySelectorAll("video").length).toBe(0);
    expect(screen.getAllByText("Play clip").length).toBe(1);
  });

  it("treats rows without a kind (older hosts) as video", async () => {
    const { kind: _k, ...old } = red;
    serve([old]);
    render(<EvidenceLibrary />);
    await waitFor(() => expect(screen.getByText("Red - video evidence")).toBeTruthy());
    expect(screen.queryByText("Orange - image evidence")).toBeNull();
  });

  it("says honestly when nothing is saved", async () => {
    serve([]);
    render(<EvidenceLibrary />);
    await waitFor(() => expect(screen.getByText(/No evidence saved yet/)).toBeTruthy());
    expect(screen.getByText(/Orange alarms save images; Red alarms save a video clip/)).toBeTruthy();
  });
});
