import { useEffect, useState } from "react";
import { BOOT_STEPS } from "@/components/loaders/BootSplash";

export const BOOT_STEP_MS = 600;
export const BOOT_FADE_MS = 400;

export function useBootSequence() {
  const [step, setStep] = useState(0);
  const [visible, setVisible] = useState(true);
  useEffect(() => {
    const timers = BOOT_STEPS.map((_, index) =>
      setTimeout(() => setStep(index + 1), (index + 1) * BOOT_STEP_MS),
    );
    timers.push(setTimeout(() => setVisible(false), BOOT_STEPS.length * BOOT_STEP_MS + BOOT_FADE_MS));
    return () => timers.forEach(clearTimeout);
  }, []);
  return {
    visible,
    progress: step / BOOT_STEPS.length,
    status: BOOT_STEPS[step] ?? "Ready",
    done: step >= BOOT_STEPS.length,
  };
}