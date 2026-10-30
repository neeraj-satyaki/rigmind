// Client for the C++ host (RigMind_Desktop). Inside WebView2 the host exposes
// window.chrome.webview; in a plain browser there is no host and callers keep their mock path.

type Reply = { id: number; ok: boolean; result?: unknown; error?: string };
/** Unsolicited host message, e.g. stream answers from the pipeline process. */
export type NativeEvent = { ev: string; cam?: string; [key: string]: unknown };
type WebView2 = {
  postMessage(message: string): void;
  addEventListener(type: "message", listener: (event: MessageEvent<Reply | NativeEvent>) => void): void;
};

const CALL_TIMEOUT_MS = 5000;
const pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();
const listeners = new Set<(event: NativeEvent) => void>();
let nextId = 1;
let wired = false;

function webview(): WebView2 | undefined {
  return (globalThis as unknown as { chrome?: { webview?: WebView2 } }).chrome?.webview;
}

export function isNative(): boolean {
  return webview() !== undefined;
}

/** The host refuses a call when no account is signed in (its session ended); the page then returns to the sign-in screen. */
export const SIGN_IN_REQUIRED = "Sign in required.";
export const SIGN_IN_REQUIRED_EVENT = "rigmind:sign-in-required";

function isReply(data: Reply | NativeEvent): data is Reply {
  return typeof (data as Reply).id === "number";
}

function wire(wv: WebView2) {
  if (wired) return;
  wired = true;
  wv.addEventListener("message", (event) => {
    const data = event.data;
    if (isReply(data)) {
      const call = pending.get(data.id);
      if (!call) return;
      pending.delete(data.id);
      if (data.ok) call.resolve(data.result);
      else {
        if (data.error === SIGN_IN_REQUIRED) window.dispatchEvent(new CustomEvent(SIGN_IN_REQUIRED_EVENT));
        call.reject(new Error(data.error ?? "native call failed"));
      }
      return;
    }
    listeners.forEach((listener) => listener(data));
  });
}

/** Subscribes to host events. Returns the unsubscribe function. */
export function onNativeEvent(listener: (event: NativeEvent) => void): () => void {
  const wv = webview();
  if (wv) wire(wv);
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/** Calls a C++ bridge method. Rejects when not inside the host, on host error, or on timeout. */
export function nativeCall<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = CALL_TIMEOUT_MS): Promise<T> {
  const wv = webview();
  if (!wv) return Promise.reject(new Error("native host not available"));
  wire(wv);
  const id = nextId++;
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`native call timed out: ${method}`));
    }, timeoutMs);
    pending.set(id, {
      resolve: (v) => {
        clearTimeout(timer);
        resolve(v as T);
      },
      reject: (e) => {
        clearTimeout(timer);
        reject(e);
      },
    });
    wv.postMessage(JSON.stringify({ id, method, params }));
  });
}
