import { useEffect, useRef, useState, type FormEvent } from "react";
import { History, MessageCircle, Plus, Send, Trash2, X } from "lucide-react";
import { Btn, Chip } from "./ui";
import { isNative, nativeCall, onNativeEvent } from "@/lib/native-bridge";

/** One evidence set the host found for a question (a row of footage.list). */
export type EvidenceMatch = { camId: string; timestampMs: number; clipCount: number; path: string; kind?: "video" | "image" };
type Ask = { chatId: string; matches: EvidenceMatch[]; note: string; slm: boolean };
type Msg =
  | { role: "you"; t: number; text: string }
  | { role: "assistant"; t: number; id: string; text: string; source?: "slm" | "matches"; matches: EvidenceMatch[]; waiting: boolean };
type SavedTurn = { t: number; role: "you" | "assistant"; text: string; ev: string[] };
type SessionRow = { id: string; title: string; updatedMs: number; turns: number };

/** The Evidence page listens for this to scroll to and highlight one set. */
export const EVIDENCE_FOCUS_EVENT = "rigmind:evidence-focus";

const SUGGESTIONS = ["Latest Orange evidence", "Any Red video today?", "What happened in the last hour?"];
const WAIT_MS = 300_000; // BitNet runs on the CPU and can queue behind other summaries
const SAVE_DELAY_MS = 600;

const when = (ms: number) => new Date(ms).toLocaleString("en-GB", { day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit", hour12: false });
const newId = () => `chat-${Date.now()}-${Math.floor(Math.random() * 1000)}`;

// A saved turn keeps only the evidence paths ("image:CAM-01/1793349677358"), so a reloaded chat can still show its chips.
const toSaved = (m: Msg): SavedTurn => ({
  t: m.t,
  role: m.role,
  text: m.text,
  ev: m.role === "assistant" ? m.matches.map((x) => `${x.kind ?? "video"}:${x.path}`) : [],
});
const fromSaved = (s: SavedTurn, i: number): Msg =>
  s.role === "you"
    ? { role: "you", t: s.t, text: s.text }
    : {
        role: "assistant",
        t: s.t,
        id: `saved-${i}`,
        text: s.text,
        matches: s.ev.map((token) => {
          const [kind, path] = token.includes(":") ? [token.slice(0, token.indexOf(":")), token.slice(token.indexOf(":") + 1)] : ["video", token];
          const [camId = "", stamp = "0"] = path.split("/");
          return { camId, timestampMs: Number(stamp) || 0, clipCount: 0, path, kind: kind === "image" ? "image" : "video" } as EvidenceMatch;
        }),
        waiting: false,
      };

/**
 * A small chat for finding and understanding evidence. The host picks the evidence sets and alerts that match the question
 * (so "finding" works even without the model) and, when BitNet is installed, asks it to explain only from that data, with the
 * earlier turns as a compact table. The answer arrives as a host "chat" event; matching sets appear as chips that jump to the set.
 * Every finished turn is saved by the host as a compressed TOON blob, so past chats can be reopened from History.
 */
export function EvidenceChat() {
  const [open, setOpen] = useState(false);
  const [question, setQuestion] = useState("");
  const [msgs, setMsgs] = useState<Msg[]>([]);
  const [sessionId, setSessionId] = useState(newId);
  const [showHistory, setShowHistory] = useState(false);
  const [sessions, setSessions] = useState<SessionRow[]>([]);
  const end = useRef<HTMLDivElement>(null);
  const timers = useRef(new Map<string, ReturnType<typeof setTimeout>>());
  const saveTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const loadedRef = useRef(false); // a chat just opened from History must not be saved again as a change

  useEffect(() => {
    if (!isNative()) return;
    return onNativeEvent((e) => {
      if (e.ev !== "chat" || typeof e["chatId"] !== "string") return;
      const id = e["chatId"] as string;
      const text = typeof e["text"] === "string" && e["text"] ? (e["text"] as string) : "";
      clearTimeout(timers.current.get(id));
      setMsgs((old) =>
        old.map((m) => (m.role === "assistant" && m.id === id ? { ...m, waiting: false, source: text ? "slm" : "matches", text: text ? `${m.text}\n\n${text}` : m.text } : m)),
      );
    });
  }, []);
  useEffect(() => end.current?.scrollIntoView?.({ block: "end" }), [msgs, open]);
  useEffect(() => () => timers.current.forEach(clearTimeout), []);

  // Save the session once its turns have settled (no answer still being written).
  useEffect(() => {
    if (!isNative() || msgs.length === 0) return;
    if (loadedRef.current) {
      loadedRef.current = false;
      return;
    }
    if (msgs.some((m) => m.role === "assistant" && m.waiting)) return;
    clearTimeout(saveTimer.current);
    saveTimer.current = setTimeout(() => {
      nativeCall("chat.save", { session: { id: sessionId, turns: msgs.map(toSaved) } }).catch((e) => console.warn("chat not saved", e));
    }, SAVE_DELAY_MS);
    return () => clearTimeout(saveTimer.current);
  }, [msgs, sessionId]);

  if (!isNative()) return null;

  const refreshSessions = () =>
    nativeCall<SessionRow[]>("chat.list", {})
      .then(setSessions)
      .catch(() => setSessions([]));
  const openHistory = () => {
    setShowHistory((v) => !v);
    void refreshSessions();
  };
  const newChat = () => {
    setMsgs([]);
    setSessionId(newId());
    setShowHistory(false);
  };
  const openSession = async (id: string) => {
    try {
      const s = await nativeCall<{ id: string; turns: SavedTurn[] }>("chat.load", { id });
      loadedRef.current = true;
      setSessionId(s.id);
      setMsgs(s.turns.map(fromSaved));
      setShowHistory(false);
    } catch (e) {
      console.warn("chat not opened", e);
    }
  };
  const deleteSession = async (id: string) => {
    await nativeCall("chat.delete", { id }).catch(() => {});
    if (id === sessionId) newChat();
    await refreshSessions();
  };

  const ask = async (text: string) => {
    const q = text.trim();
    if (!q) return;
    setQuestion("");
    const history = msgs.slice(-6).map((m) => ({ role: m.role, text: m.text }));
    setMsgs((old) => [...old, { role: "you", t: Date.now(), text: q }]);
    try {
      const r = await nativeCall<Ask>("evidence.ask", { question: q, history });
      setMsgs((old) => [...old, { role: "assistant", t: Date.now(), id: r.chatId, text: r.note, source: "matches", matches: r.matches, waiting: r.slm }]);
      if (r.slm)
        timers.current.set(
          r.chatId,
          setTimeout(() => setMsgs((old) => old.map((m) => (m.role === "assistant" && m.id === r.chatId ? { ...m, waiting: false } : m))), WAIT_MS),
        );
    } catch (e) {
      setMsgs((old) => [
        ...old,
        { role: "assistant", t: Date.now(), id: "err", text: `Could not ask: ${String(e instanceof Error ? e.message : e)}`, matches: [], waiting: false },
      ]);
    }
  };
  const focus = (path: string) => window.dispatchEvent(new CustomEvent(EVIDENCE_FOCUS_EVENT, { detail: { path } }));
  const onSubmit = (e: FormEvent) => {
    e.preventDefault();
    void ask(question);
  };

  if (!open)
    return (
      <Btn variant="primary" className="fixed bottom-14 right-5 z-30 shadow-lg" onClick={() => setOpen(true)} aria-label="Ask about evidence">
        <MessageCircle className="size-4" /> Ask about evidence
      </Btn>
    );

  return (
    <section
      aria-label="Ask about evidence"
      className="fixed bottom-14 right-5 z-30 flex h-[min(30rem,70vh)] w-[min(24rem,calc(100vw-2rem))] flex-col overflow-hidden rounded-lg border border-border bg-card shadow-2xl"
    >
      <header className="flex items-center gap-1 border-b border-border px-3 py-2">
        <MessageCircle className="size-4" />
        <strong className="mr-auto text-sm">Ask about evidence</strong>
        <button type="button" onClick={openHistory} aria-label="Past chats" aria-pressed={showHistory} title="Past chats" className="rounded p-1 text-muted-foreground hover:text-foreground">
          <History className="size-4" />
        </button>
        <button type="button" onClick={newChat} aria-label="New chat" title="New chat" className="rounded p-1 text-muted-foreground hover:text-foreground">
          <Plus className="size-4" />
        </button>
        <button type="button" onClick={() => setOpen(false)} aria-label="Close chat" className="rounded p-1 text-muted-foreground hover:text-foreground">
          <X className="size-4" />
        </button>
      </header>
      {showHistory ? (
        <div className="flex-1 overflow-y-auto p-2 text-sm" role="list" aria-label="Past chats">
          {sessions.length === 0 && <p className="p-2 text-muted-foreground">No saved chats yet. Each chat is saved on this PC after your first question.</p>}
          {sessions.map((s) => (
            <div key={s.id} role="listitem" className="flex items-center gap-1 rounded-md hover:bg-field">
              <button type="button" onClick={() => void openSession(s.id)} className="min-w-0 flex-1 px-2 py-1.5 text-left">
                <span className="block truncate">{s.title || "(no question)"}</span>
                <span className="text-[11px] text-muted-foreground">
                  {when(s.updatedMs)} · {s.turns} turn{s.turns === 1 ? "" : "s"}
                </span>
              </button>
              <button type="button" onClick={() => void deleteSession(s.id)} aria-label={`Delete chat ${s.title}`} className="p-1.5 text-muted-foreground hover:text-critical">
                <Trash2 className="size-4" />
              </button>
            </div>
          ))}
        </div>
      ) : (
        <div className="flex-1 space-y-3 overflow-y-auto p-3 text-sm" role="log" aria-live="polite">
          {msgs.length === 0 && (
            <div className="space-y-2 text-muted-foreground">
              <p>Find evidence or ask what happened. Answers use only the evidence, alerts and summaries saved on this PC.</p>
              <div className="flex flex-wrap gap-1.5">
                {SUGGESTIONS.map((s) => (
                  <button key={s} type="button" onClick={() => void ask(s)} className="rounded-full border border-border px-2.5 py-1 text-xs hover:bg-field">
                    {s}
                  </button>
                ))}
              </div>
            </div>
          )}
          {msgs.map((m, i) =>
            m.role === "you" ? (
              <div key={i} className="ml-8 rounded-lg bg-primary px-3 py-2 text-primary-foreground">
                {m.text}
              </div>
            ) : (
              <div key={i} className="mr-4 space-y-2 rounded-lg bg-field px-3 py-2">
                <p className="whitespace-pre-wrap">{m.text}</p>
                {m.waiting && <p className="text-xs text-muted-foreground">BitNet is writing the explanation. It can take up to a minute…</p>}
                {m.source === "slm" && <Chip tone="neutral">BitNet</Chip>}
                {m.matches.length > 0 && (
                  <ul className="space-y-1">
                    {m.matches.slice(0, 6).map((x) => (
                      <li key={x.path}>
                        <button type="button" onClick={() => focus(x.path)} className="w-full rounded-md border border-border px-2 py-1 text-left text-xs hover:bg-card">
                          {x.kind === "image" ? "Orange images" : "Red video"} · {x.camId} · {x.timestampMs ? when(x.timestampMs) : ""}
                          {x.clipCount ? ` · ${x.clipCount} file${x.clipCount === 1 ? "" : "s"}` : ""}
                        </button>
                      </li>
                    ))}
                  </ul>
                )}
              </div>
            ),
          )}
          <div ref={end} />
        </div>
      )}
      <form onSubmit={onSubmit} className="flex gap-2 border-t border-border p-2">
        <input
          className="h-9 min-w-0 flex-1 rounded-md border border-border bg-field px-2 text-sm"
          value={question}
          onChange={(e) => setQuestion(e.target.value)}
          placeholder="e.g. Orange images from CAM-01 today"
          aria-label="Your question"
          maxLength={300}
        />
        <Btn type="submit" variant="primary" className="h-9 w-9 px-0" aria-label="Send question" disabled={!question.trim()}>
          <Send className="size-4" />
        </Btn>
      </form>
    </section>
  );
}
