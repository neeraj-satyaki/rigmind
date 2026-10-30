import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";

const h = vi.hoisted(() => ({
  calls: [] as { method: string; params: Record<string, any> }[],
  listeners: new Set<(e: any) => void>(),
  slm: false,
  saved: [] as { id: string; title: string; updatedMs: number; turns: number }[],
  stored: new Map<string, any[]>(),
}));
vi.mock("@/lib/native-bridge", () => ({
  isNative: () => true,
  onNativeEvent: (l: (e: any) => void) => (h.listeners.add(l), () => h.listeners.delete(l)),
  nativeCall: async (method: string, params: Record<string, any>) => {
    h.calls.push({ method, params });
    if (method === "evidence.ask")
      return {
        chatId: "chat-1",
        note: "Found 1 Orange image set for CAM-01 in today.",
        slm: h.slm,
        matches: [{ camId: "CAM-01", timestampMs: 1793349677358, clipCount: 3, path: "CAM-01/1793349677358", kind: "image" }],
      };
    if (method === "chat.save") {
      h.stored.set(params["session"].id, params["session"].turns);
      h.saved = [{ id: params["session"].id, title: params["session"].turns[0].text, updatedMs: 1, turns: params["session"].turns.length }];
      return {};
    }
    if (method === "chat.list") return h.saved;
    if (method === "chat.load") return { id: params["id"], turns: h.stored.get(params["id"]) };
    return {};
  },
}));

import { EVIDENCE_FOCUS_EVENT, EvidenceChat } from "@/components/rig/EvidenceChat";

const open = () => fireEvent.click(screen.getByRole("button", { name: "Ask about evidence" }));
const ask = async (q: string) => {
  fireEvent.change(screen.getByLabelText("Your question"), { target: { value: q } });
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: "Send question" }));
  });
};

describe("evidence chat", () => {
  beforeEach(() => {
    h.calls.length = 0;
    h.listeners.clear();
    h.slm = false;
    h.saved = [];
    h.stored.clear();
  });

  it("finds evidence without a model: shows the plain answer and a chip that focuses the set", async () => {
    render(<EvidenceChat />);
    open();
    await ask("Latest Orange evidence");
    await screen.findByText("Found 1 Orange image set for CAM-01 in today.");
    expect(h.calls.find((c) => c.method === "evidence.ask")?.params).toMatchObject({ question: "Latest Orange evidence", history: [] });
    const focused = vi.fn();
    window.addEventListener(EVIDENCE_FOCUS_EVENT, (e) => focused((e as CustomEvent).detail));
    fireEvent.click(screen.getByRole("button", { name: /Orange images · CAM-01/ }));
    expect(focused).toHaveBeenCalledWith({ path: "CAM-01/1793349677358" });
    expect(screen.queryByText(/BitNet is writing/)).toBeNull(); // no model: nothing to wait for
  });

  it("with BitNet: waits for the 'chat' event, adds the explanation and marks the source", async () => {
    h.slm = true;
    render(<EvidenceChat />);
    open();
    await ask("What happened in the last hour?");
    await screen.findByText(/BitNet is writing the explanation/);
    await act(async () => {
      h.listeners.forEach((l) => l({ ev: "chat", chatId: "chat-1", text: "One Orange image set was saved at 14:22." }));
    });
    expect(await screen.findByText(/One Orange image set was saved at 14:22\./)).toBeTruthy();
    expect(screen.queryByText(/BitNet is writing/)).toBeNull();
    expect(screen.getByText("BitNet")).toBeTruthy();
  });

  it("a follow-up question carries the earlier turns as history", async () => {
    render(<EvidenceChat />);
    open();
    await ask("Latest Orange evidence");
    await screen.findByText(/Found 1 Orange/);
    await ask("and the one before it?");
    const second = h.calls.filter((c) => c.method === "evidence.ask")[1]!;
    expect(second.params["history"]).toEqual([
      { role: "you", text: "Latest Orange evidence" },
      { role: "assistant", text: "Found 1 Orange image set for CAM-01 in today." },
    ]);
  });

  it("saves the session through the host once the turns settle, and reopens it from History", async () => {
    vi.useFakeTimers({ toFake: ["setTimeout", "clearTimeout"] });
    try {
      render(<EvidenceChat />);
      open();
      await ask("Latest Orange evidence");
      await act(async () => {
        await vi.advanceTimersByTimeAsync(1000);
      });
      const save = h.calls.find((c) => c.method === "chat.save")!;
      expect(save.params["session"].turns.map((t: any) => [t.role, t.text])).toEqual([
        ["you", "Latest Orange evidence"],
        ["assistant", "Found 1 Orange image set for CAM-01 in today."],
      ]);
      expect(save.params["session"].turns[1].ev).toEqual(["image:CAM-01/1793349677358"]);
    } finally {
      vi.useRealTimers();
    }
    // a new chat, then reopen the old one from History
    fireEvent.click(screen.getByRole("button", { name: "New chat" }));
    expect(screen.queryByText(/Found 1 Orange/)).toBeNull();
    fireEvent.click(screen.getByRole("button", { name: "Past chats" }));
    const entry = await screen.findByText("Latest Orange evidence");
    await act(async () => {
      fireEvent.click(entry);
    });
    await waitFor(() => expect(screen.getByText("Found 1 Orange image set for CAM-01 in today.")).toBeTruthy());
    expect(screen.getByRole("button", { name: /Orange images · CAM-01/ })).toBeTruthy(); // the chip came back from the saved evidence path
  });
});
