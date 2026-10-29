import { alerts, eventHistory, eventLogSeed, rigs as rigFixtures } from "@/data/mock";
import type { Severity } from "@/data/mock";

export type LoggedEvent = {
  id: string;
  /** Local rig time, ISO without zone: 2026-10-06T09:42:18 */
  at: string;
  rig: string;
  severity: Severity;
  type: string;
  ackSeconds: number | null;
  status: "Open" | "Closed";
};

const DAY = 86_400_000;
const pad = (n: number) => String(n).padStart(2, "0");
const iso = (d: Date) => `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}`;
const parse = (s: string) => new Date(`${s.slice(0, 10)}T00:00:00Z`);
const WD = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const MO = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const fmt = (d: Date, o: { weekday?: string; day?: string; month?: string; year?: string }) =>
  [o.weekday && WD[d.getUTCDay()], o.day && pad(d.getUTCDate()), o.month && MO[d.getUTCMonth()], o.year && d.getUTCFullYear()].filter(Boolean).join(" ");

/** Deterministic demo log built from the seed counts. */
export function buildSeedEvents(): LoggedEvent[] {
  const out: LoggedEvent[] = [];
  let n = 0;
  for (const [start, days] of Object.entries(eventLogSeed.weeks)) {
    days.forEach((counts, d) => {
      const date = new Date(parse(start).getTime() + d * DAY);
      (["Advisory", "Warning", "Critical"] as Severity[]).forEach((severity, si) => {
        for (let i = 0; i < counts[si]!; i++) {
          n++;
          const hour = [10, 11, 13, 14, 15, 8, 19, 22, 3, 12, 16, 20][(n * 7) % 12]!;
          out.push({
            id: `EV-${String(n).padStart(4, "0")}`,
            at: `${iso(date)}T${pad(hour)}:${pad((n * 13) % 60)}:${pad((n * 29) % 60)}`,
            rig: eventLogSeed.rigs[[0, 0, 1, 0, 2, 1, 3, 0, 1, 2][n % 10]!]!,
            severity,
            type: eventLogSeed.types[(n * 3 + si) % eventLogSeed.types.length]!,
            ackSeconds: n % 6 === 0 ? null : 4 + ((n * 11) % 21),
            status: n % 17 === 0 ? "Open" : "Closed",
          });
        }
      });
    });
  }
  // Named investigation events from the fixtures, recorded on the last seeded day.
  const lastDay = iso(new Date(parse(Object.keys(eventLogSeed.weeks).sort().at(-1)!).getTime() + 6 * DAY));
  for (const h of eventHistory) {
    const a = alerts.find((x) => x.id === h.id);
    out.push({
      id: h.id, at: `${lastDay}T${h.occurred}`, rig: a?.rig ?? "Demo rig 01", severity: h.severity,
      type: h.type, ackSeconds: h.review === "In review" ? null : 8, status: h.review === "Closed" ? "Closed" : "Open",
    });
  }
  alerts.filter((a) => !eventHistory.some((h) => h.id === a.id)).forEach((a, i) => {
    out.push({ id: a.id, at: `${lastDay}T09:${pad(41 - i)}:00`, rig: a.rig, severity: a.severity, type: a.condition, ackSeconds: null, status: "Open" });
  });
  return out;
}

export function weekStartOf(at: string) {
  const d = parse(at);
  const back = (d.getUTCDay() - eventLogSeed.weekStartDay + 7) % 7;
  return iso(new Date(d.getTime() - back * DAY));
}

/** ISO week number of the Thursday inside the reporting week. */
export function weekLabel(start: string) {
  const d = parse(start);
  const thu = new Date(d.getTime() + ((4 - d.getUTCDay() + 7) % 7) * DAY);
  const jan1 = Date.UTC(thu.getUTCFullYear(), 0, 1);
  return Math.floor((thu.getTime() - jan1) / DAY / 7) + 1;
}

export function availableWeeks(events: LoggedEvent[]) {
  return [...new Set(events.map((e) => weekStartOf(e.at)))].sort().reverse();
}

const median = (v: number[]) => {
  if (!v.length) return null;
  const s = [...v].sort((a, b) => a - b);
  const m = Math.floor(s.length / 2);
  return s.length % 2 ? s[m]! : (s[m - 1]! + s[m]!) / 2;
};
const pct = (v: number[], p: number) => {
  if (!v.length) return null;
  const s = [...v].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.ceil((p / 100) * s.length) - 1)]!;
};

export type Comparison = { available: false } | { available: true; previous: number; change: number; pct: number | null };
const compare = (cur: number, prev: number | null): Comparison =>
  prev === null ? { available: false } : { available: true, previous: prev, change: cur - prev, pct: prev ? ((cur - prev) / prev) * 100 : null };

export function computeWeekly(all: LoggedEvent[], start: string) {
  const startD = parse(start);
  const prevStart = iso(new Date(startD.getTime() - 7 * DAY));
  const inWeek = (s: string) => all.filter((e) => weekStartOf(e.at) === s);
  const ev = inWeek(start);
  const prev = inWeek(prevStart);
  const hasPrev = prev.length > 0;
  const days = Array.from({ length: 7 }, (_, i) => iso(new Date(startD.getTime() + i * DAY)));
  const count = (list: LoggedEvent[], sev: Severity) => list.filter((e) => e.severity === sev).length;
  const byDay = days.map((d) => ev.filter((e) => e.at.startsWith(d)));
  const series: [number[], number[], number[]] = [
    byDay.map((l) => count(l, "Advisory")),
    byDay.map((l) => count(l, "Warning")),
    byDay.map((l) => count(l, "Critical")),
  ];
  const busiestIdx = byDay.reduce((b, l, i) => (l.length > byDay[b]!.length ? i : b), 0);
  const critPeakIdx = series[2].reduce((b, v, i) => (v > series[2][b]! ? i : b), 0);
  const acks = ev.map((e) => e.ackSeconds).filter((v): v is number => v !== null);
  const prevAcks = prev.map((e) => e.ackSeconds).filter((v): v is number => v !== null);
  const total = ev.length;
  const critical = count(ev, "Critical");
  const blocks = [0, 6, 12, 18].map((h) => ({
    label: `${pad(h)}:00–${pad((h + 6) % 24)}:00`,
    n: ev.filter((e) => Math.floor(Number(e.at.slice(11, 13)) / 6) * 6 === h).length,
  }));
  const peak = blocks.reduce((a, b) => (b.n > a.n ? b : a), blocks[0]!);
  const quiet = blocks.reduce((a, b) => (b.n < a.n ? b : a), blocks[0]!);
  const night = ev.filter((e) => { const h = Number(e.at.slice(11, 13)); return h >= 18 || h < 6; }).length;
  const rigRows = rigFixtures.map((r) => {
    const l = ev.filter((e) => e.rig === r.name);
    return { rig: r.name, region: r.region, readiness: r.status, critical: count(l, "Critical"), warning: count(l, "Warning"), advisory: count(l, "Advisory"), open: l.filter((e) => e.status === "Open").length, total: l.length };
  });
  const topRig = rigRows.reduce((a, b) => (b.total > a.total ? b : a), rigRows[0]!);
  const busiest = byDay[busiestIdx]!;
  const typeBars = eventLogSeed.types
    .map((t) => ({ label: t, count: busiest.filter((e) => e.type === t).length }))
    .sort((a, b) => b.count - a.count);
  const dayLabel = (i: number) => fmt(parse(days[i]!), { weekday: "short", day: "2-digit", month: "short" });
  return {
    start, end: days[6]!, week: weekLabel(start), hasPrev, ids: new Set(ev.map((e) => e.id)),
    period: `${fmt(startD, { weekday: "short", day: "2-digit", month: "short" })} – ${fmt(parse(days[6]!), { weekday: "short", day: "2-digit", month: "short", year: "numeric" })}`,
    labels: days.map((d) => fmt(parse(d), { weekday: "short", day: "2-digit" })),
    series, total, critical, warning: count(ev, "Warning"), advisory: count(ev, "Advisory"),
    avgPerDay: total / 7,
    criticalShare: total ? (critical / total) * 100 : 0,
    medianAck: median(acks), p95Ack: pct(acks, 95), acknowledged: acks.length,
    busiestDay: { label: dayLabel(busiestIdx), total: busiest.length, critical: count(busiest, "Critical") },
    criticalPeak: { label: dayLabel(critPeakIdx), n: series[2][critPeakIdx]! },
    rigRows, topRig: { ...topRig, share: total ? (topRig.total / total) * 100 : 0 },
    typeBars, peak, quiet, nightShare: total ? (night / total) * 100 : 0,
    vsPrev: {
      total: compare(total, hasPrev ? prev.length : null),
      critical: compare(critical, hasPrev ? count(prev, "Critical") : null),
      medianAck: hasPrev && prevAcks.length ? { available: true as const, previous: median(prevAcks)! } : { available: false as const },
    },
  };
}
export type WeeklyStats = ReturnType<typeof computeWeekly>;

const KEY = "eventlog:v1";
/** Events persisted in device SQLite; seeded once from the demo fixture. */
export async function loadEventLog(): Promise<LoggedEvent[]> {
  const { readDevice, writeDevice } = await import("./device-db");
  const existing = await readDevice<LoggedEvent[]>(KEY);
  if (existing) return existing;
  const seed = buildSeedEvents();
  await writeDevice(KEY, seed);
  return seed;
}
