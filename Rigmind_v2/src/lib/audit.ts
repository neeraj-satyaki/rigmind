import { isNative, nativeCall } from "@/lib/native-bridge";

// UI audit trail. Clicks on controls and route changes are queued here and sent to the C++ audit log
// in batches by a background timer, so the UI never waits on storage. Typed input is never recorded.

type AuditEntry = { ts: number; action: string; target: string; detail: string };

const FLUSH_MS = 1000;
const MAX_BATCH = 50;
const queue: AuditEntry[] = [];
let installed = false;
let timer: ReturnType<typeof setInterval> | undefined;

function flush() {
  if (queue.length === 0) return;
  const entries = queue.splice(0, MAX_BATCH);
  nativeCall("audit.write", { entries }).catch((e) => console.warn("audit write failed", e));
}

/** Queues one audit entry. Outside the host (plain browser) it does nothing. */
export function audit(action: string, target = "", detail = "") {
  if (!isNative()) return;
  queue.push({ ts: Date.now(), action, target: target.slice(0, 128), detail: detail.slice(0, 512) });
  if (queue.length >= MAX_BATCH) flush();
}

// Label of a control: its accessible name, else its text. Never the value of an input.
function describe(el: Element): string {
  const label = el.getAttribute("aria-label") || el.getAttribute("title") || el.textContent || el.tagName;
  return label.replace(/\s+/g, " ").trim().slice(0, 80);
}

/** Starts capturing clicks and navigation once per page. Safe to call again. */
export function installUiAudit() {
  if (installed || !isNative()) return;
  installed = true;

  document.addEventListener(
    "click",
    (event) => {
      const el = (event.target as Element | null)?.closest?.("button, a, [role='button'], [role='tab'], [role='menuitem']");
      if (!el) return;
      const href = el instanceof HTMLAnchorElement ? el.getAttribute("href") ?? "" : "";
      audit("click", describe(el), href);
    },
    true,
  );

  const push = history.pushState.bind(history);
  const replace = history.replaceState.bind(history);
  history.pushState = (...args: Parameters<History["pushState"]>) => {
    push(...args);
    audit("navigate", location.pathname);
  };
  history.replaceState = (...args: Parameters<History["replaceState"]>) => {
    replace(...args);
    audit("navigate", location.pathname);
  };
  window.addEventListener("popstate", () => audit("navigate", location.pathname));

  timer = setInterval(flush, FLUSH_MS);
  window.addEventListener("pagehide", () => {
    if (timer) clearInterval(timer);
    flush();
  });
}
