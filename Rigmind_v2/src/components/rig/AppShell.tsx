import { ALERT_RAISED, LOGO_ALERT_MS } from "@/lib/alert-bus";
import { startAlarmAlerts } from "@/lib/alarm-alerts";
import { startPpeAlerts } from "@/lib/ppe-alerts";
import { Suspense, useEffect, useState, type ReactNode } from "react";
import { Link, useRouterState } from "@tanstack/react-router";
import {
  LayoutGrid,
  Gauge,
  MonitorPlay,
  Inbox,
  History,
  FolderOpen,
  BarChart3,
  ClipboardCheck,
  FileText,
  Settings,
  ChevronDown,
  ChevronRight,
  Bell,
  Search,
  Moon,
  Lock,
  Menu,
  X,
  ChevronsUpDown,
} from "lucide-react";
import { cn } from "@/lib/utils";
import { Avatar } from "./ui";
import { currentUser as sampleUser } from "@/data/mock";
import { demoOr } from "@/lib/demo-mode";
import { currentOperator } from "@/lib/auth";
import { useAppState } from "./AppState";
import RigMindMark from "@/components/loaders/RigMindMark";
import PageLoader, { TopProgress } from "@/components/loaders/PageLoader";
import { usePageTransition } from "@/hooks/usePageTransition";
import { ThemeToggle, NavSearch, Notifications, UserMenu } from "./NavTools";
import satyakiLogo from "@/assets/satyaki-logo.svg";

const sections = [
  {
    label: "WORKSPACE",
    items: [
      { to: "/fleet", label: "Fleet overview", icon: LayoutGrid },
      { to: "/rig", label: "Rig workspace", icon: Gauge },
      { to: "/live", label: "Live monitoring", icon: MonitorPlay },
      { to: "/alerts", label: "Alert inbox", icon: Inbox, badge: true },
    ],
  },
  {
    label: "REVIEW & INSIGHTS",
    items: [
      { to: "/events", label: "Event history", icon: History },
      { to: "/evidence", label: "Evidence library", icon: FolderOpen },
      { to: "/analytics", label: "Analytics", icon: BarChart3 },
      { to: "/actions", label: "Actions & handover", icon: ClipboardCheck },
    ],
  },
] as const;

const adminItems = [
  { to: "/admin/cameras/new", label: "Cameras" },
  { to: "/admin/rules", label: "Safety rules" },
  { to: "/admin/zones", label: "Zone editor" },
  { to: "/admin/health", label: "System health" },
  { to: "/admin/license", label: "License" },
  { to: "/admin/integrations", label: "Integrations" },
  { to: "/admin/users", label: "Users & access" },
  { to: "/admin/audit", label: "Audit trail" },
] as const;

const titles: Record<string, string> = {
  "/fleet": "Fleet overview",
  "/rig": "Rig workspace",
  "/live": "Live monitoring",
  "/alerts": "Alert inbox",
  "/events": "Event history",
  "/evidence": "Evidence library",
  "/analytics": "Analytics",
  "/analytics/overview": "Event overview",
  "/actions": "Actions & handover",
  "/reports": "Reports",
  ...Object.fromEntries(adminItems.map((a) => [a.to, a.label])),
};

const navCls = (active: boolean) =>
  cn(
    "flex items-center gap-2 rounded-md px-2 py-1.5 text-[13px]",
    active
      ? "bg-nav-active font-semibold text-primary"
      : "text-muted-foreground hover:bg-field hover:text-foreground",
  );

function AlertLogo() {
  const [active, setActive] = useState(false);
  useEffect(() => {
    let t: ReturnType<typeof setTimeout> | undefined;
    const on = () => {
      setActive(true);
      clearTimeout(t);
      t = setTimeout(() => setActive(false), LOGO_ALERT_MS);
    };
    window.addEventListener(ALERT_RAISED, on);
    return () => {
      window.removeEventListener(ALERT_RAISED, on);
      clearTimeout(t);
    };
  }, []);
  return <RigMindMark key={active ? "on" : "off"} size={40} animate={active} />;
}

function Sidebar({ onNav }: { onNav: () => void }) {
  const currentUser = demoOr(sampleUser, currentOperator());
  const path = useRouterState({ select: (s) => s.location.pathname });
  const [adminOpen, setAdminOpen] = useState(path.startsWith("/admin"));
  const is = (to: string) => path === to || (to !== "/" && path.startsWith(to + "/"));
  const { items: alertItems } = useAppState();
  // Alerts that still need a person: raised and not yet acknowledged or cleared.
  const openAlerts = alertItems.filter((a) => a.status === "Open" || a.status === "Escalated").length;
  return (
    <div className="flex h-full flex-col gap-4 overflow-y-auto p-3">
      <div className="flex flex-col items-start gap-1">
        <div className="flex items-center gap-2" aria-label="RigMind">
          <AlertLogo />
          <strong className="text-xl">RigMind<sup className="tm">™</sup></strong>
        </div>
        <div className="text-[11px] text-muted-foreground">Safety operations</div>
      </div>
      {sections.map((s) => (
        <div key={s.label}>
          <div className="mb-1 px-2 text-[10px] font-semibold tracking-widest text-muted-foreground">
            {s.label}
          </div>
          {s.items.map((i) => (
            <Link key={i.to} to={i.to} onClick={onNav} className={navCls(is(i.to))}>
              <i.icon className="size-4" /> <span className="flex-1">{i.label}</span>
              {"badge" in i && openAlerts > 0 && (
                <span className="rounded-full bg-critical px-1.5 text-[10px] font-semibold text-destructive-foreground">
                  {openAlerts}
                </span>
              )}
            </Link>
          ))}
        </div>
      ))}
      <div>
        <div className="mb-1 px-2 text-[10px] font-semibold tracking-widest text-muted-foreground">
          GOVERNANCE
        </div>
        <Link to="/reports" onClick={onNav} className={navCls(is("/reports"))}>
          <FileText className="size-4" /> Reports
        </Link>
        <button onClick={() => setAdminOpen((o) => !o)} className={cn(navCls(false), "w-full")}>
          <Settings className="size-4" /> <span className="flex-1 text-left">Administration</span>
          {adminOpen ? <ChevronDown className="size-3.5" /> : <ChevronRight className="size-3.5" />}
        </button>
        {adminOpen && (
          <div className="ml-4 border-l border-border pl-2">
            {adminItems.map((a) => (
              <Link key={a.to} to={a.to} onClick={onNav} className={navCls(is(a.to))}>
                {a.label}
              </Link>
            ))}
          </div>
        )}
      </div>
      <div className="mt-auto space-y-2 border-t border-border pt-3">
        <div className="inline-flex items-center gap-1.5 rounded-full border border-success/30 bg-success-bg px-2 py-0.5 text-[11px] font-medium text-success">
          <span className="size-1.5 rounded-full bg-success" /> Rig-local edge
        </div>
        <div className="flex items-center gap-2">
          <Avatar initials={currentUser.initials} />
          <div>
            <div className="text-[12px] font-semibold">{currentUser.name}</div>
            <div className="text-[11px] text-muted-foreground">{currentUser.role}</div>
          </div>
        </div>
      </div>
    </div>
  );
}

export function SatyakiLogo({ caption = false }: { caption?: boolean }) {
  const logo = (
    <div className="flex shrink-0 items-center gap-2">
      <img src={satyakiLogo} alt="Satyaki Solutions" width={78} height={85} className="h-9 w-auto object-contain" />
      {/* One line: the wordmark reads "SATYAKI SOLUTIONS" left to right. */}
      <strong className="font-brand whitespace-nowrap text-[12px] font-semibold leading-none text-brand-word">
        SATYAKI <span className="text-[10px] font-medium text-brand-sub">SOLUTIONS</span>
      </strong>

    </div>
  );
  if (!caption) return logo;
  return (
    <div className="flex flex-col items-end gap-1 text-right">
      {logo}
      <span className="text-[10px] text-muted-foreground">Powered by Satyaki Solutions © 2026</span>
    </div>
  );
}

export function AppShell({ children }: { children: ReactNode }) {
  const [open, setOpen] = useState(false);
  useEffect(() => startAlarmAlerts(), []); // EWS alarms -> app alerts on every page
  useEffect(() => startPpeAlerts(), []); // PPE compliance (not detected) -> Advisory alerts on every page
  const [utcClock, setUtcClock] = useState("");
  const navigating = usePageTransition();
  const path = useRouterState({ select: (s) => s.location.pathname });
  const title = titles[path] ?? (path.startsWith("/events/") ? "Event investigation" : "Workspace");
  useEffect(() => {
    const fmt = () =>
      setUtcClock(new Date().toLocaleTimeString("en-GB", { timeZone: "UTC", hour12: false }));
    fmt();
    const t = setInterval(fmt, 30_000);
    return () => clearInterval(t);
  }, []);

  return (
    <div className="flex min-h-screen w-full">
      <aside className="sticky top-0 hidden h-screen w-[190px] shrink-0 border-r border-border bg-sidebar lg:block">
        <Sidebar onNav={() => {}} />
      </aside>
      {open && (
        <div className="fixed inset-0 z-40 lg:hidden">
          <div className="absolute inset-0 bg-foreground/30" onClick={() => setOpen(false)} />
          <aside className="relative h-full w-[230px] border-r border-border bg-sidebar">
            <button
              className="absolute right-2 top-2 p-1"
              onClick={() => setOpen(false)}
              aria-label="Close menu"
            >
              <X className="size-4" />
            </button>
            <Sidebar onNav={() => setOpen(false)} />
          </aside>
        </div>
      )}
      <div className="flex min-w-0 flex-1 flex-col">
        <header className="sticky top-0 z-30 flex h-14 items-center gap-3 border-b border-border bg-card px-4">
          <button className="lg:hidden" onClick={() => setOpen(true)} aria-label="Open menu">
            <Menu className="size-5" />
          </button>
          <div className="min-w-0 truncate text-[13px]">
            <span className="text-muted-foreground">Workspace</span>{" "}
            <span className="text-muted-foreground">›</span>{" "}
            <span className="font-medium">{title}</span>
          </div>
          <div className="ml-auto flex items-center gap-2">
            <NavSearch />
            <Notifications />
            <ThemeToggle />
            <UserMenu />
            <div className="hidden h-6 w-px bg-border sm:block" />
            <div className="hidden sm:block">
              <SatyakiLogo />
            </div>
          </div>
          <TopProgress active={navigating} />
        </header>
        <main className="flex flex-1 flex-col p-4 md:p-6" aria-busy={navigating}>
          {navigating ? <PageLoader /> : <Suspense fallback={<PageLoader />}>{children}</Suspense>}
        </main>
        <footer className="flex flex-wrap items-center gap-x-4 gap-y-0.5 border-t border-border bg-card px-4 py-1 text-[11px] text-muted-foreground sm:flex-row sm:justify-between md:px-6">
          <span className="flex items-center gap-1.5">
            <Lock className="size-3" /> Powered by Satyaki Solutions © 2026
          </span>
          <span className="ml-auto font-mono">{utcClock} UTC</span>


        </footer>
      </div>
    </div>
  );
}
