import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { toast } from "sonner";
import { readDevice, writeDevice } from "@/lib/device-db";
import { alerts, type Alert } from "@/data/mock";
import { isNative, nativeCall } from "@/lib/native-bridge";
import { installUiAudit } from "@/lib/audit";
import { demoDataEnabled } from "@/lib/demo-mode";
import { currentOperator } from "@/lib/auth";
import type { SummaryFields } from "@/lib/alert-text";

// Sample alerts only when the installer option was ticked; otherwise the list starts empty and fills from the host.
const seedAlerts = (): Alert[] => (demoDataEnabled() ? alerts : []);
const operatorName = () => (demoDataEnabled() ? "Faisal A." : currentOperator().name);
const FEED_INTERVAL_MS = 1000;

// One row of the backend alerts.feed: an alert the C++ backend raised, with its current status.
type RaisedRow = { id: string; severity: Alert["severity"]; condition: string; place: string; status: Alert["status"]; owner: string; raisedMs: number; updatedMs?: number; phase?: string } & SummaryFields;

// Facts metadata and the SLM sentence of a host row (null until written). Sample rows never have these keys.
const hostFields = (row: RaisedRow) => ({
  place: row.place,
  metadata: row.metadata ?? null,
  summary: row.summary ?? null,
  summarySource: row.summarySource ?? null,
  summaryAt: row.summaryAt ?? null,
  updatedMs: row.updatedMs ?? 0,
});

// "Demo rig 01 · Derrick board" -> rig and area, as the alert table shows them.
function toAlert(row: RaisedRow, now: number): Alert {
  const [rig = "", area = ""] = row.place.split("·").map((part) => part.trim());
  const seconds = Math.max(0, Math.round((now - row.raisedMs) / 1000));
  const age = seconds < 60 ? `${seconds} s` : `${Math.floor(seconds / 60)} m ${String(seconds % 60).padStart(2, "0")} s`;
  return { id: row.id, rig, area, severity: row.severity, condition: row.condition, age, owner: row.owner, status: row.status, ...hostFields(row) };
}

// Adds new raised alerts on top, and updates status/owner/age of known ones. Returns the same array when nothing changed.
function mergeFeed(old: Alert[], rows: RaisedRow[]): Alert[] {
  const now = Date.now();
  const byId = new Map(rows.map((r) => [r.id, r]));
  let changed = false;
  const updated = old.map((a) => {
    const r = byId.get(a.id);
    if (!r) return a;
    const next = { ...a, status: r.status, owner: r.owner, severity: r.severity, condition: r.condition, ...hostFields(r) };
    // metadata changes only together with updatedMs (the host writes both in one step), so the cheap comparison is enough
    if (
      next.status !== a.status || next.owner !== a.owner || next.severity !== a.severity || next.condition !== a.condition ||
      next.summary !== a.summary || next.summarySource !== a.summarySource || next.updatedMs !== a.updatedMs
    )
      changed = true;
    return next;
  });
  const known = new Set(old.map((a) => a.id));
  const fresh = rows.filter((r) => !known.has(r.id)).map((r) => toAlert(r, now));
  if (fresh.length) changed = true;
  return changed ? [...fresh, ...updated] : old;
}

const Context = createContext<{
  dark: boolean;
  toggleTheme: () => void;
  items: Alert[];
  respond: (id: string, status: Alert["status"]) => void;
  reset: () => void;
}>({ dark: true, toggleTheme: () => {}, items: seedAlerts(), respond: () => {}, reset: () => {} });
export function AppState({ children }: { children: ReactNode }) {
  // Dark is the default theme; a saved choice (preferences) overrides it, and "Reset settings" returns to it.
  const [dark, setDark] = useState(true),
    [items, setItems] = useState<Alert[]>(seedAlerts);
  useEffect(() => {
    readDevice<{ dark: boolean; items: Alert[] }>("preferences")
      .then((saved) => {
        if (saved) {
          setDark(saved.dark);
          // Saved rows are the sample set (or a copy of host rows); real alerts always come from the host feed.
          if (demoDataEnabled()) setItems(saved.items);
        }
      })
      .catch(() => toast.error("Device storage could not be opened"));
  }, []);
  useEffect(() => {
    if (!isNative()) return;
    // Live feed: the C++ backend owns raised alerts and their status. Polled once a second; the list
    // only re-renders when a row changes.
    let stopped = false;
    const poll = () =>
      nativeCall<RaisedRow[]>("alerts.feed")
        .then((rows) => {
          if (stopped) return;
          setItems((old) => mergeFeed(old, rows));
        })
        .catch((e) => console.warn("alerts.feed failed", e));
    poll();
    const timer = window.setInterval(poll, FEED_INTERVAL_MS);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, []);

  useEffect(() => {
    installUiAudit();
  }, []);

  const firstTheme = useRef(true);
  useEffect(() => {
    const root = document.documentElement;
    let glide: ReturnType<typeof setTimeout> | undefined;
    if (!firstTheme.current) {
      // not on the first paint: only when the person switches the theme, so the colours glide instead of snapping
      root.classList.add("rm-theme-switching");
      glide = setTimeout(() => root.classList.remove("rm-theme-switching"), 360);
    }
    firstTheme.current = false;
    root.classList.toggle("dark", dark);
    try {
      localStorage.setItem("rm-theme", dark ? "dark" : "light"); // read by the head script so the next start paints the right theme (and boot splash) at once
    } catch {
      /* storage blocked: the saved preference still applies once loaded */
    }
    return () => clearTimeout(glide);
  }, [dark]);
  return (
    <Context.Provider
      value={{
        dark,
        toggleTheme: () =>
          setDark((v) => {
            writeDevice("preferences", { dark: !v, items }).catch(() =>
              toast.error("Theme could not be saved"),
            );
            return !v;
          }),
        items,
        respond: (id, status) => {
          if (isNative())
            nativeCall("alert.respond", { id, status, owner: operatorName() }).catch((e) =>
              console.warn("alert.respond failed", e),
            );
          setItems((old) => {
            const next = old.map((a) => (a.id === id ? { ...a, status, owner: operatorName() } : a));
            writeDevice("preferences", { dark, items: next }).catch(() =>
              toast.error("Response could not be saved"),
            );
            return next;
          });
          toast.success(`${id} ${status.toLowerCase()}`);
        },
        reset: () => setItems(seedAlerts()),
      }}
    >
      {children}
    </Context.Provider>
  );
}
export const useAppState = () => useContext(Context);
