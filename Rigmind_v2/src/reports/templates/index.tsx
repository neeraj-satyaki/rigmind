import type { ComponentType } from "react";
import { ActionReport } from "./action";
import { AuditReport, downloadAuditCsv } from "./audit";
import { EventReport } from "./event";
import { HandoverReport } from "./handover";
import { HealthReport } from "./health";
import { HseReport } from "./hse";
import { PackIndex } from "./pack";
import { PilotReport } from "./pilot";
import { RegisterReport, downloadRegisterCsv } from "./register";
import { ShiftReport } from "./shift";
import { WeeklyReport } from "./weekly";

export type ReportSearch = {
  eventId?: string | undefined;
  actionId?: string | undefined;
  type?: string | undefined;
  week?: string | undefined;
};

type ReportEntry = {
  name: string;
  Component: ComponentType<{ search: ReportSearch }>;
  csv?: () => void;
};

export const reportTemplates: Record<string, ReportEntry> = {
  pack: { name: "RigMind report pack", Component: PackIndex },
  shift: {
    name: "Shift safety report",
    Component: ({ search }) => <ShiftReport pages={["01 / 02", "02 / 02"]} key={search.type} />,
  },
  event: {
    name: "Event investigation report",
    Component: ({ search }) => <EventReport page="01 / 01" eventId={search.eventId} />,
  },
  hse: { name: "HSE review report", Component: () => <HseReport page="01 / 01" /> },
  action: {
    name: "Corrective action report",
    Component: ({ search }) => <ActionReport page="01 / 01" actionId={search.actionId} />,
  },
  handover: { name: "Shift handover report", Component: () => <HandoverReport page="01 / 01" /> },
  weekly: {
    name: "Weekly fleet safety report",
    Component: ({ search }) => <WeeklyReport week={search.week} />,
  },
  health: { name: "Monitoring coverage report", Component: () => <HealthReport page="01 / 01" /> },
  pilot: { name: "Pilot results report", Component: () => <PilotReport page="01 / 01" /> },
  register: {
    name: "Event register",
    Component: () => <RegisterReport page="01 / 01" />,
    csv: downloadRegisterCsv,
  },
  audit: { name: "Audit trail extract", Component: () => <AuditReport page="01 / 01" />, csv: downloadAuditCsv },
};

export const reportTitles: Record<string, string> = Object.fromEntries(
  Object.entries(reportTemplates).map(([id, e]) => [id, e.name]),
);
