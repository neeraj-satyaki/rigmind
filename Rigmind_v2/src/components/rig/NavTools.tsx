import { ALERT_RAISED, LOGO_ALERT_MS, raiseAlert, type RaisedAlert } from "@/lib/alert-bus";
import { playAlertSound } from "@/lib/alert-sound";
import { isValidEmail, saveProfileEmail, useProfileEmail } from "@/lib/profile";
import { useEffect, useRef, useState } from "react";
import { Link, useNavigate } from "@tanstack/react-router";
import { Bell, Search, Moon, Sun, LogOut, UserRound, CheckCheck } from "lucide-react";
import { toast } from "sonner";
import { Popover, PopoverContent, PopoverTrigger } from "@/components/ui/popover";
import { Btn, Chip, Avatar, Row, Mono } from "./ui";
import { Prompt } from "./workflow";
import { useAppState } from "./AppState";
import { alerts as mockAlerts, cameras, currentUser } from "@/data/mock";
import { isNative, nativeCall, onNativeEvent } from "@/lib/native-bridge";

/** Runs a scripted detection through the C++ state machine; each transition comes back as an alert to raise. */
async function runNativeDetection() {
  const site = mockAlerts[0];
  const place = site ? `${site.rig} · ${site.area}` : "";
  try {
    const { alerts: raised } = await nativeCall<{ alerts: RaisedAlert[] }>("alarm.simulate", {
      scenario: "red",
      place,
      cam: cameras[0]?.id ?? "",
    });
    raised.forEach((a) => {
      raiseAlert(a);
      describeAlert(a, cameras[0]?.id ?? "", place);
    });
  } catch (e) {
    toast.error("Detection could not be run", { description: String(e) });
  }
}

/** Plain-language text for one alert: template at once, or the SLM sentence when the model answers. */
async function describeAlert(a: RaisedAlert, camId: string, place: string) {
  const show = (text: string) => toast(`${a.id} in plain words`, { description: text });
  try {
    const reply = await nativeCall<{ text: string; slm: boolean }>("event.describe", {
      alertId: a.id,
      severity: a.severity,
      condition: a.condition,
      place,
      camId,
      tsMs: Date.now(),
    });
    if (!reply.slm) {
      show(reply.text);
      return;
    }
    const off = onNativeEvent((msg) => {
      if (msg.ev !== "description" || msg["alertId"] !== a.id) return;
      off();
      const text = msg["text"];
      show(typeof text === "string" ? text : reply.text);
    });
  } catch (e) {
    console.warn("event.describe failed", e);
  }
}
export function ThemeToggle() {
  const { dark, toggleTheme } = useAppState();
  return (
    <Btn
      title={dark ? "Light theme" : "Dark theme"}
      aria-label={dark ? "Light theme" : "Dark theme"}
      onClick={toggleTheme}
      className="px-2"
    >
      {dark ? <Sun className="size-4" /> : <Moon className="size-4" />}
    </Btn>
  );
}

export function NavSearch() {
  const [q, setQ] = useState(""),
    [open, setOpen] = useState(false);
  const { items } = useAppState();
  const nav = useNavigate();
  const input = useRef<HTMLInputElement>(null);
  const results = items
    .filter((a) => `${a.id} ${a.condition} ${a.area}`.toLowerCase().includes(q.toLowerCase()))
    .slice(0, 5);
  const cams = cameras
    .filter((c) => `${c.id} ${c.area}`.toLowerCase().includes(q.toLowerCase()))
    .slice(0, 3);
  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger asChild>
        <Btn aria-label="Search events and cameras" className="px-2 md:hidden">
          <Search className="size-4" />
        </Btn>
      </PopoverTrigger>
      <div className="relative hidden md:block">
        <Search className="absolute left-2 top-2.5 size-3.5 text-muted-foreground" />
        <input
          ref={input}
          aria-label="Search events and cameras"
          placeholder="Search events, cameras…"
          className="h-8 w-44 rounded-md border bg-field pl-7 pr-2 text-xs xl:w-56"
          value={q}
          onFocus={() => setOpen(true)}
          onChange={(e) => {
            setQ(e.target.value);
            setOpen(true);
          }}
          onKeyDown={(e) => {
            if (e.key === "Escape") setOpen(false);
            if (e.key === "Enter") {
              const first = results[0];
              if (first) {
                nav({ to: first.id === "EV-1042" ? "/live" : "/alerts" });
                setOpen(false);
              }
            }
          }}
        />
      </div>
      <PopoverContent
        align="end"
        className="w-[min(360px,calc(100vw-24px))] p-2"
        onOpenAutoFocus={(e) => e.preventDefault()}
      >
        <input
          aria-label="Search suggestions"
          value={q}
          onChange={(e) => setQ(e.target.value)}
          className="mb-2 h-8 w-full rounded border bg-field px-2 text-xs md:hidden"
          placeholder="Search events, cameras…"
        />
        <p className="px-2 py-1 text-xs text-muted-foreground">
          {q ? "Matching results" : "Recent events"}
        </p>
        {results.map((a) => (
          <Link
            key={a.id}
            to={a.id === "EV-1042" ? "/live" : "/alerts"}
            onClick={() => setOpen(false)}
            className="block rounded p-2 hover:bg-field"
          >
            <Mono>{a.id}</Mono>
            <p className="text-xs">{a.condition}</p>
          </Link>
        ))}
        {q &&
          cams.map((c) => (
            <Link
              key={c.id}
              to="/live"
              onClick={() => setOpen(false)}
              className="block rounded p-2 hover:bg-field"
            >
              <Mono>{c.id}</Mono>
              <p className="text-xs">{c.area}</p>
            </Link>
          ))}
        {q && !results.length && !cams.length && (
          <p className="p-4 text-xs text-muted-foreground">No matching events or cameras.</p>
        )}
      </PopoverContent>
    </Popover>
  );
}
export function Notifications() {
  const [open, setOpen] = useState(false);
  const { items } = useAppState();
  const [extra, setExtra] = useState<RaisedAlert[]>([]),
    [unread, setUnread] = useState<number | null>(null);
  const count = unread ?? items.length;
  useEffect(() => {
    const timers: ReturnType<typeof setTimeout>[] = [];
    const on = (e: Event) => {
      const a = (e as CustomEvent<RaisedAlert>).detail;
      timers.push(
        setTimeout(() => {
          setExtra((x) => [a, ...x]);
          setUnread((u) => (u ?? items.length) + 1);
          const fn = a.severity === "Critical" ? toast.error : a.severity === "Warning" ? toast.warning : toast;
          fn(`${a.severity} safety event`, { description: `${a.id} · ${a.condition} · ${a.place}` });
          playAlertSound(a.severity);
        }, LOGO_ALERT_MS),
      );
    };
    window.addEventListener(ALERT_RAISED, on);
    return () => {
      window.removeEventListener(ALERT_RAISED, on);
      timers.forEach(clearTimeout);
    };
  }, [items.length]);
  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger asChild>
        <Btn aria-label="Notifications" title="Notifications" className="relative w-8 px-0">
          <Bell className="size-4" />
          {count > 0 && (
            <span
              aria-label={`${count} unread alerts`}
              className="absolute -right-1.5 -top-1.5 flex h-4 min-w-4 items-center justify-center rounded-full bg-critical px-1 text-[9px] font-semibold text-primary-foreground"
            >
              {count > 99 ? "99+" : count}
            </span>
          )}
        </Btn>
      </PopoverTrigger>
      <PopoverContent align="end" className="w-[min(380px,calc(100vw-24px))] p-0">
        <div className="flex items-center justify-between border-b p-3">
          <strong>Safety events</strong>
          <Btn
            title="Mark all as read"
            aria-label="Mark all as read"
            className="px-2"
            onClick={() => {
              setUnread(0);
              toast.success("Notifications marked as read");
            }}
          >
            <CheckCheck className="size-4" />
          </Btn>
        </div>
        <div className="max-h-80 overflow-y-auto">
          {extra.map((a, i) => (
            <Link key={`${a.id}-${i}`} to="/alerts" onClick={() => setOpen(false)} className="block border-b p-3 hover:bg-field">
              <div className="flex justify-between">
                <Chip tone={a.severity === "Critical" ? "critical" : a.severity === "Warning" ? "warning" : "advisory"}>{a.severity}</Chip>
                <Mono>{a.id}</Mono>
              </div>
              <p className="mt-1 text-sm font-medium">{a.condition}</p>
              <p className="text-xs text-muted-foreground">{a.place} · just now</p>
            </Link>
          ))}
          {items.map((a) => (
            <Link
              key={a.id}
              to={a.id === "EV-1042" ? "/live" : "/alerts"}
              onClick={() => setOpen(false)}
              className="block border-b p-3 hover:bg-field"
            >
              <div className="flex justify-between">
                <Chip
                  tone={
                    a.severity === "Critical"
                      ? "critical"
                      : a.severity === "Warning"
                        ? "warning"
                        : "advisory"
                  }
                >
                  {a.severity}
                </Chip>
                <Mono>{a.id}</Mono>
              </div>
              <p className="mt-1 text-sm font-medium">{a.condition}</p>
              <p className="text-xs text-muted-foreground">
                {a.rig} · {a.area} · {a.age}
              </p>
            </Link>
          ))}
        </div>
        <div className="flex flex-wrap gap-2 p-3">
          <Link to="/alerts" onClick={() => setOpen(false)}>
            <Btn>View all alerts</Btn>
          </Link>
          <Btn
            onClick={() =>
              toast("Background push is not connected", {
                description:
                  "Connect Firebase Cloud Messaging to receive events while RigMind is closed.",
              })
            }
          >
            Enable push
          </Btn>
          <Btn
            onClick={() =>
              isNative()
                ? runNativeDetection()
                : raiseAlert({
                    id: `EV-${1043 + extra.length}`,
                    severity: "Critical",
                    condition: "Occupied movement zone",
                    place: "Demo rig 01 · Derrick board",
                  })
            }
          >
            Test notification
          </Btn>
        </div>
      </PopoverContent>
    </Popover>
  );
}
export function UserMenu() {
  const [profile, setProfile] = useState(false);
  const email = useProfileEmail();
  const [editing, setEditing] = useState(false),
    [emailDraft, setEmailDraft] = useState("");
  const saveEmail = async () => {
    if (!isValidEmail(emailDraft)) {
      toast.error("Enter a valid work email");
      return;
    }
    try {
      await saveProfileEmail(emailDraft);
      setEditing(false);
      toast.success("Work email updated on this device");
    } catch {
      toast.error("Could not save the email on this device");
    }
  };
  const nav = useNavigate();
  const { reset } = useAppState();
  const logout = () => {
    reset();
    nav({ to: "/login" });
    toast.success("Signed out of the demonstration workspace");
  };
  return (
    <>
      <Popover>
        <PopoverTrigger asChild>
          <Btn aria-label="User account" title="User account" variant="ghost" className="w-9 px-0">
            <Avatar initials={currentUser.initials} />
          </Btn>
        </PopoverTrigger>
        <PopoverContent align="end">
          <strong>{currentUser.name}</strong>
          <p className="mb-3 text-xs text-muted-foreground">Operations · Operator / Admin</p>
          <Btn className="mb-2 w-full" onClick={() => setProfile(true)}>
            <UserRound className="size-4" />
            View profile
          </Btn>
          <Btn className="w-full" onClick={logout}>
            <LogOut className="size-4" />
            Log out
          </Btn>
        </PopoverContent>
      </Popover>
      <Prompt
        title="User profile"
        description="Current demonstration account"
        open={profile}
        onClose={() => setProfile(false)}
      >
        <div className="flex items-center gap-3">
          <Avatar initials={currentUser.initials} size={48} />
          <div>
            <strong>{currentUser.name}</strong>
            <p className="text-sm text-muted-foreground">Operations</p>
          </div>
        </div>
        {editing ? (
          <form
            className="flex items-center gap-2"
            onSubmit={(e) => {
              e.preventDefault();
              saveEmail();
            }}
          >
            <input
              aria-label="Work email"
              type="email"
              className="h-9 flex-1 rounded-md border border-border bg-field px-3 text-[13px]"
              value={emailDraft}
              onChange={(e) => setEmailDraft(e.target.value)}
              autoFocus
            />
            <Btn type="submit" variant="primary">Save</Btn>
            <Btn type="button" onClick={() => setEditing(false)}>Cancel</Btn>
          </form>
        ) : (
          <Row
            k="Work email"
            v={
              <span className="flex items-center gap-2">
                {email}
                <Btn
                  className="h-7 px-2 text-xs"
                  onClick={() => {
                    setEmailDraft(email);
                    setEditing(true);
                  }}
                >
                  Edit
                </Btn>
              </span>
            }
          />
        )}
        <Row k="Role" v="Operator · Admin" />
        <Row k="Assigned rig" v="Demo rig 01" />
        <Row k="Shift" v="Day shift · 06:00–18:00 AST" />
        <Row k="Access" v="Active" />
        <Row k="Last access review" v="01 Oct 2026" />
        <Btn onClick={logout}>
          <LogOut className="size-4" />
          Log out
        </Btn>
      </Prompt>
    </>
  );
}
