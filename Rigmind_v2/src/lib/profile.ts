import { useEffect, useState } from "react";
import { readDevice, writeDevice } from "./device-db";

export const DEFAULT_EMAIL = "faisal.demo@example.com";
const KEY = "profile:email";
const EVT = "rigmind:profile-email";

export const isValidEmail = (v: string) => /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(v.trim()) && v.length <= 254;

export async function saveProfileEmail(email: string) {
  await writeDevice(KEY, email.trim());
  window.dispatchEvent(new CustomEvent(EVT, { detail: email.trim() }));
}

export function useProfileEmail() {
  const [email, setEmail] = useState(DEFAULT_EMAIL);
  useEffect(() => {
    readDevice<string>(KEY).then((v) => v && setEmail(v)).catch(() => {});
    const on = (e: Event) => setEmail((e as CustomEvent<string>).detail);
    window.addEventListener(EVT, on);
    return () => window.removeEventListener(EVT, on);
  }, []);
  return email;
}
