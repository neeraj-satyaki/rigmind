import { useSyncExternalStore } from "react";
import { isNative, nativeCall } from "@/lib/native-bridge";

/**
 * Local sign-in. The desktop host owns the accounts (salted PBKDF2 hashes in its database); this module asks it to create the
 * first account, to check a username and password, and keeps who is signed in for this app run (sessionStorage: it ends when the
 * app closes). Outside the host (plain browser, tests) there are no accounts and nothing is enforced.
 */
export type Session = { username: string; displayName: string; role: string };

const KEY = "rm-session";
const listeners = new Set<() => void>();
let cached: Session | null | undefined;

function read(): Session | null {
  if (cached !== undefined) return cached;
  try {
    const raw = typeof sessionStorage !== "undefined" ? sessionStorage.getItem(KEY) : null;
    const v = raw ? (JSON.parse(raw) as Session) : null;
    cached = v && typeof v.username === "string" ? v : null;
  } catch {
    cached = null;
  }
  return cached;
}

export const getSession = (): Session | null => read();

function write(s: Session | null) {
  cached = s;
  try {
    if (s) sessionStorage.setItem(KEY, JSON.stringify(s));
    else sessionStorage.removeItem(KEY);
  } catch {
    /* storage blocked: the session lasts until the page reloads */
  }
  listeners.forEach((l) => l());
}

export function useSession(): Session | null {
  return useSyncExternalStore(
    (l) => (listeners.add(l), () => listeners.delete(l)),
    read,
    () => null,
  );
}

/** True when this run must sign in: inside the desktop host. */
export const authRequired = () => isNative();

export async function authStatus(): Promise<{ hasUsers: boolean }> {
  return nativeCall<{ hasUsers: boolean }>("auth.status", {});
}

/** Creates the first account (administrator) and signs it in. Rejects with the host's readable message. */
export async function createFirstAccount(username: string, password: string): Promise<Session> {
  const s = await nativeCall<Session>("auth.setup", { username, password });
  write(s);
  return s;
}

/** Checks the username and password with the host. Rejects with "Wrong username or password." or the lockout message. */
export async function signIn(username: string, password: string): Promise<Session> {
  const s = await nativeCall<Session>("auth.login", { username, password });
  write(s);
  return s;
}

/** Forgets the account in this page (the host already ended its session). */
export const clearSession = () => write(null);

export function signOut() {
  const who = read()?.username ?? "";
  write(null);
  if (isNative()) nativeCall("auth.logout", { username: who }).catch(() => {});
}

/** The person using this device for labels and "owner" fields: the signed-in account, else a neutral "Operator". */
export function currentOperator(): { name: string; role: string; initials: string } {
  const s = read();
  if (!s) return { name: "Operator", role: "This device", initials: "OP" };
  const initials = s.displayName
    .split(/[.@_\-\s]+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((p) => p[0]!.toUpperCase())
    .join("");
  return { name: s.displayName, role: s.role, initials: initials || "OP" };
}
