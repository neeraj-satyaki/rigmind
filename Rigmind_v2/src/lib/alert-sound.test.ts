import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

// A tiny Audio stand-in: tracks play/pause and lets the test end a tone.
class FakeAudio {
  static instances: FakeAudio[] = [];
  src = "";
  currentTime = 0;
  paused = true;
  ended = false;
  plays: string[] = [];
  private handlers: Record<string, (() => void)[]> = {};
  constructor() {
    FakeAudio.instances.push(this);
  }
  addEventListener(name: string, fn: () => void) {
    (this.handlers[name] ??= []).push(fn);
  }
  play() {
    this.paused = false;
    this.ended = false;
    this.plays.push(this.src);
    return Promise.resolve();
  }
  pause() {
    this.paused = true;
  }
  finish() {
    this.paused = true;
    this.ended = true;
    (this.handlers["ended"] ?? []).forEach((fn) => fn());
  }
}

describe("looping alarm tone", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.resetModules();
    FakeAudio.instances.length = 0;
    (globalThis as unknown as { Audio: typeof FakeAudio }).Audio = FakeAudio;
    try {
      localStorage.removeItem("rigmind.alerts.muted");
    } catch {
      /* no storage in this test environment */
    }
  });
  afterEach(() => {
    vi.useRealTimers();
  });
  const load = async () => import("./alert-sound");
  const plays = () => FakeAudio.instances.flatMap((a) => a.plays).length;
  const finishAll = () => FakeAudio.instances.forEach((a) => !a.paused && a.finish());
  const paused = () => FakeAudio.instances.every((a) => a.paused);

  it("waits for the toast's own tone, then repeats it after each gap until the alarm is back to Yellow", async () => {
    const s = await load();
    s.setAlertLoop("CAM-01", "Warning");
    await vi.advanceTimersByTimeAsync(2000);
    expect(plays()).toBe(0); // the first tone belongs to the toast (playAlertSound), the loop does not double it
    s.playAlertSound("Warning"); // what the toast does
    expect(plays()).toBe(1);
    await vi.advanceTimersByTimeAsync(3000);
    expect(plays()).toBe(1); // still playing: no overlap
    finishAll();
    await vi.advanceTimersByTimeAsync(2000);
    expect(plays()).toBe(1); // Warning gap is 2.5 s
    await vi.advanceTimersByTimeAsync(800);
    expect(plays()).toBe(2); // repeated
    finishAll();
    await vi.advanceTimersByTimeAsync(3200);
    expect(plays()).toBe(3); // and again: it keeps looping

    s.setAlertLoop("CAM-01", null); // back to Yellow
    expect(s.isAlertLooping()).toBe(false);
    expect(paused()).toBe(true);
    finishAll();
    await vi.advanceTimersByTimeAsync(20000);
    expect(plays()).toBe(3); // silent from now on
  });

  it("Red repeats faster than Orange and wins while both are active", async () => {
    const s = await load();
    s.setAlertLoop("CAM-01", "Warning");
    s.setAlertLoop("CAM-02", "Critical");
    s.playAlertSound("Critical");
    finishAll();
    await vi.advanceTimersByTimeAsync(2600); // first-tone window (2.4 s) passed, gap 1.2 s passed
    expect(plays()).toBe(2);
    expect(FakeAudio.instances.find((a) => a.plays.length > 1)?.src).toContain("redAlert"); // the Red tone, not the Orange one
    s.setAlertLoop("CAM-02", null); // Red camera back to Yellow: Orange keeps looping
    expect(s.isAlertLooping()).toBe(true);
    s.setAlertLoop("CAM-01", null);
    expect(s.isAlertLooping()).toBe(false);
  });

  it("is silent while muted and resumes on unmute if the alarm is still active", async () => {
    const s = await load();
    s.setAlertLoop("CAM-01", "Critical");
    s.setAlertMuted(true);
    await vi.advanceTimersByTimeAsync(10000);
    expect(plays()).toBe(0);
    expect(s.isAlertLooping()).toBe(true); // still an active alarm
    s.setAlertMuted(false);
    await vi.advanceTimersByTimeAsync(1000);
    expect(plays()).toBe(1);
    s.setAlertLoop("CAM-01", null);
  });

  it("an Advisory or Yellow level never loops", async () => {
    const s = await load();
    s.setAlertLoop("CAM-01", "Advisory");
    s.setAlertLoop("CAM-01", "Yellow");
    expect(s.isAlertLooping()).toBe(false);
    await vi.advanceTimersByTimeAsync(10000);
    expect(plays()).toBe(0);
  });
});
