// Single typed mock-data module. Swap these exports for API calls later.
export type Severity = "Critical" | "Warning" | "Advisory";
export type Tone = "critical" | "warning" | "advisory" | "success" | "neutral";

export const currentUser = { name: "Faisal A.", role: "Operations", initials: "FA" };

export const kpis = {
  rigs: { value: "04", note: "3 healthy · 1 degraded" },
  cameras: { value: "23/24", note: "95.8% current availability" },
  alerts: { value: "07", note: "2 critical need attention" },
  verified: { value: "42", note: "6 critical · 14 warning · 22 advisory" },
};

export const eventActivity = [
  { day: "30 Sep", events: 30 }, { day: "01 Oct", events: 29 }, { day: "02 Oct", events: 29 },
  { day: "03 Oct", events: 34 }, { day: "04 Oct", events: 36 }, { day: "05 Oct", events: 33 },
  { day: "06 Oct", events: 42 },
];

export interface Rig { id: string; name: string; region: string; status: "Ready" | "Degraded"; openAlerts: number }
export const rigs: Rig[] = [
  { id: "RIG-01", name: "Demo rig 01", region: "Eastern Province", status: "Ready", openAlerts: 3 },
  { id: "RIG-02", name: "Demo rig 02", region: "Eastern Province", status: "Ready", openAlerts: 2 },
  { id: "RIG-03", name: "Demo rig 03", region: "Offshore", status: "Ready", openAlerts: 1 },
  { id: "RIG-04", name: "Demo rig 04", region: "Eastern Province", status: "Degraded", openAlerts: 1 },
];

export interface Camera { id: string; area: string; hazard?: boolean }
export const cameras: Camera[] = [
  { id: "CAM-01", area: "Derrick board", hazard: true }, { id: "CAM-02", area: "Derrick board" },
  { id: "CAM-03", area: "Drill floor" }, { id: "CAM-04", area: "Drill floor" },
  { id: "CAM-05", area: "Pipe racking" }, { id: "CAM-06", area: "Access corridor" },
];

export const monitoredAreas = [
  { name: "Derrick board", cams: "CAM-01 · CAM-02", status: "Attention", tone: "critical" as Tone, note: "1 critical alert" },
  { name: "Drill floor", cams: "CAM-03 · CAM-04", status: "Ready", tone: "success" as Tone, note: "2 events for review" },
  { name: "Pipe handling", cams: "CAM-05", status: "Ready", tone: "success" as Tone, note: "No active hazards" },
  { name: "Access corridor", cams: "CAM-06", status: "Ready", tone: "success" as Tone, note: "Monitoring active" },
];

export interface Alert { id: string; rig: string; severity: Severity; condition: string; area: string; age: string; owner: string; status: "Open" | "Acknowledged" | "Escalated" | "Monitoring" | "Cleared" }
export const alerts: Alert[] = [
  { id: "EV-1042", rig: "Demo rig 01", severity: "Critical", condition: "Occupied movement zone", area: "Derrick board", age: "8 s", owner: "Unassigned", status: "Open" },
  { id: "EV-1041", rig: "Demo rig 02", severity: "Critical", condition: "Restricted-zone entry", area: "Drill floor", age: "19 s", owner: "Nasser H.", status: "Open" },
  { id: "EV-1039", rig: "Demo rig 01", severity: "Warning", condition: "PPE visibility issue", area: "Derrick board", age: "1 m 24 s", owner: "Faisal A.", status: "Open" },
  { id: "EV-1038", rig: "Demo rig 02", severity: "Warning", condition: "Equipment proximity", area: "Pipe handling", age: "2 m 18 s", owner: "Nasser H.", status: "Open" },
  { id: "EV-1037", rig: "Demo rig 04", severity: "Warning", condition: "Restricted-zone entry", area: "Access corridor", age: "3 m 03 s", owner: "Omar S.", status: "Open" },
  { id: "EV-1036", rig: "Demo rig 03", severity: "Advisory", condition: "Posture observation", area: "Derrick board", age: "4 m 20 s", owner: "HSE desk", status: "Open" },
  { id: "EV-1035", rig: "Demo rig 01", severity: "Advisory", condition: "PPE review", area: "Drill floor", age: "4 m 48 s", owner: "Faisal A.", status: "Open" },
];

export interface EventRecord { id: string; occurred: string; severity: Severity; type: string; review: "In review" | "Acknowledged" | "Closed"; owner: string }
export const eventHistory: EventRecord[] = [
  { id: "EV-1042", occurred: "09:42:18", severity: "Critical", type: "Occupied movement zone", review: "In review", owner: "MS" },
  { id: "EV-1039", occurred: "09:40:54", severity: "Warning", type: "PPE visibility issue", review: "Acknowledged", owner: "FA" },
  { id: "EV-1035", occurred: "09:37:30", severity: "Advisory", type: "PPE review", review: "Acknowledged", owner: "FA" },
  { id: "EV-1028", occurred: "08:56:03", severity: "Warning", type: "Equipment proximity", review: "Closed", owner: "MS" },
  { id: "EV-1017", occurred: "07:31:22", severity: "Advisory", type: "Posture observation", review: "Closed", owner: "MS" },
  { id: "EV-1012", occurred: "07:18:40", severity: "Warning", type: "Restricted-zone entry", review: "Closed", owner: "MS" },
];

export const responseTimeline = [
  { time: "09:42:18", label: "Detected", note: "Edge service · Rule DZ-01" },
  { time: "09:42:26", label: "Acknowledged", note: "Faisal A. · Live monitoring" },
  { time: "09:43:02", label: "Hazard cleared", note: "Rule engine · Zone vacated" },
  { time: "09:45:10", label: "Review started", note: "Maha S. · HSE reviewer" },
];

export const clips = [
  { id: "EV-1042", cam: "CAM-01", area: "Derrick board", severity: "Critical" as Severity, time: "09:42:18" },
  { id: "EV-1039", cam: "CAM-02", area: "Derrick board", severity: "Warning" as Severity, time: "09:40:54" },
  { id: "EV-1028", cam: "CAM-03", area: "Drill floor", severity: "Warning" as Severity, time: "08:56:03" },
];

export const severityTrend = [
  { day: "30 Sep", critical: 3, warning: 9, advisory: 18 },
  { day: "01 Oct", critical: 4, warning: 8, advisory: 17 },
  { day: "02 Oct", critical: 2, warning: 11, advisory: 16 },
  { day: "03 Oct", critical: 5, warning: 10, advisory: 19 },
  { day: "04 Oct", critical: 4, warning: 12, advisory: 20 },
  { day: "05 Oct", critical: 3, warning: 9, advisory: 21 },
  { day: "06 Oct", critical: 6, warning: 14, advisory: 22 },
];

export const heatmap: { day: string; values: number[] }[] = [
  { day: "Wed", values: [1, 1, 0, 1, 3, 4, 5, 4, 5, 3, 2, 1] },
  { day: "Thu", values: [1, 0, 1, 1, 2, 4, 5, 4, 4, 3, 2, 2] },
  { day: "Fri", values: [0, 1, 0, 2, 3, 4, 4, 5, 3, 3, 2, 2] },
  { day: "Sat", values: [1, 1, 1, 2, 3, 5, 5, 4, 4, 3, 3, 2] },
  { day: "Sun", values: [1, 1, 1, 2, 4, 5, 5, 5, 4, 3, 3, 2] },
  { day: "Mon", values: [1, 1, 0, 2, 3, 4, 5, 5, 4, 3, 3, 2] },
  { day: "Tue", values: [2, 1, 1, 2, 4, 5, 5, 5, 5, 5, 4, 3] },
];

export const eventsByType = [
  { type: "Zone occupancy", count: 18 }, { type: "PPE review", count: 12 },
  { type: "Equipment proximity", count: 8 }, { type: "Posture observation", count: 4 },
];

export const services = [
  { name: "RIG-01 / CAM-01", status: "Online", tone: "success" as Tone, seen: "1 second ago" },
  { name: "RIG-04 / CAM-06", status: "Offline", tone: "critical" as Tone, seen: "4 minutes ago" },
  { name: "RIG-01 / Edge inference", status: "Healthy", tone: "success" as Tone, seen: "2 seconds ago" },
  { name: "Event evidence store", status: "Healthy", tone: "success" as Tone, seen: "5 seconds ago" },
  { name: "Beacon gateway", status: "Simulation", tone: "advisory" as Tone, seen: "3 seconds ago" },
];

export const users = [
  { name: "Faisal A.", initials: "FA", role: "Operator · Admin", scope: "Demo rig 01", access: "Active" },
  { name: "Maha S.", initials: "MS", role: "HSE reviewer", scope: "Demo fleet", access: "Active" },
  { name: "Omar S.", initials: "OS", role: "Maintenance", scope: "Demo rig 04", access: "Active" },
  { name: "Nasser H.", initials: "NH", role: "Rig manager", scope: "Demo rig 02", access: "Active" },
  { name: "Audit viewer", initials: "AV", role: "Auditor", scope: "Approved evidence", access: "Read only" },
];

export const auditLog = [
  { time: "09:42:18", actor: "Edge service", action: "Event created", entity: "EV-1042", result: "Recorded" },
  { time: "09:42:26", actor: "Faisal A.", action: "Acknowledged", entity: "EV-1042", result: "Recorded" },
  { time: "09:43:02", actor: "Rule engine", action: "Hazard cleared", entity: "EV-1042", result: "Recorded" },
  { time: "09:45:10", actor: "Maha S.", action: "Review started", entity: "EV-1042", result: "Recorded" },
  { time: "09:47:00", actor: "Maha S.", action: "Action created", entity: "CA-028", result: "Recorded" },
  { time: "09:50:31", actor: "Faisal A.", action: "Draft rule saved", entity: "DZ-01 v4", result: "Pending approval" },
];

export const signalMappings = [
  { name: "Equipment moving", signal: "MOVEMENT_ACTIVE", dir: "Read" },
  { name: "Critical event active", signal: "RIGMIND_CRITICAL", dir: "Write / sim" },
  { name: "Output confirmation", signal: "BEACON_FEEDBACK", dir: "Read" },
];

export const licenseLifecycle = [
  { state: "Renewal window", behavior: "Persistent notice before expiry", response: "Renewal process opened" },
  { state: "Expired", behavior: "Restricted administrative changes", response: "Site-approved continuity policy" },
  { state: "Capacity reached", behavior: "New allocation blocked", response: "Existing allocation remains visible" },
];

export const actionUpdates = [
  { time: "09:47", who: "Maha S.", text: "Action created from EV-1042 review." },
  { time: "10:05", who: "Faisal A.", text: "Owner accepted. Crew briefing scheduled." },
];

export const pilotReadiness = [
  { item: "Camera placement", status: "Review required", tone: "warning" as Tone },
  { item: "Rules & use cases", status: "Approval required", tone: "warning" as Tone },
  { item: "Detection quality", status: "Not measured", tone: "neutral" as Tone },
  { item: "Alert latency", status: "Not measured", tone: "neutral" as Tone },
  { item: "Alarm interface", status: "Simulated", tone: "advisory" as Tone },
  { item: "Site UAT", status: "Not started", tone: "neutral" as Tone },
];

export const severityTone = (s: Severity): Tone => s.toLowerCase() as Tone;

export const demoPolicy = { rollingDays: 7, eventDays: 30, allocatedStreams: 24, streamCapacity: 32, validThrough: "31 Dec 2026", licenseId: "RM-DEMO-2026-001", physicalOutputs: false, machineryStop: false, deployedRuleVersion: 3, draftRuleVersion: 4 };
export const ruleDefaults = { id: "DZ-01", severity: "Critical", scope: "CAM-01 / Derrick board", trigger: "Person in zone + movement active", confirmation: "Pending site approval", clearance: "Pending site approval", missing: "Degrade affected rule and notify operator" };
export const reviewDefaults = { classification: "Confirmed unsafe exposure", confidence: "Sufficient for review", notes: "Person occupied the configured zone while the simulated movement signal was active. Follow-up assigned to the shift supervisor." };
export const zoneDefaults: [number, number][] = [[32, 76], [38, 39], [62, 36], [70, 73]];
export const reportTypes = [{ id: "weekly", title: "Weekly safety events", note: "Week summary + approved events" }, { id: "shift", title: "Shift safety", note: "Events and response" }, { id: "hse", title: "HSE review", note: "Evidence and actions" }, { id: "pilot", title: "Pilot results", note: "Measured outcomes" }];
export const reportSections = ["Verified events and response times", "Coverage and unavailable periods", "Corrective actions and evidence references", "Definitions and data-quality notes"];
export const roles = [{ title: "Operators", note: "Monitor assigned rigs and acknowledge safety alerts." }, { title: "HSE reviewers", note: "Review evidence and accept or reopen corrective actions." }, { title: "Administrators", note: "Manage approved configurations and scoped access." }];
export const integrationStages = [{ title: "Rig signals", note: "Read / simulated" }, { title: "RigMind edge", note: "Context + rules" }, { title: "Output gateway", note: "Write / simulated" }, { title: "Field warning", note: "Feedback / simulated" }];

export const weeklyReportMeta = {
  week: 40,
  period: "Wed 30 Sep – Tue 06 Oct 2026",
  asset: "Demo rigs 01–04",
  cameras: "24 streams · 23 online",
  monitored: "Day + night shifts · simulated",
  previous: { total: 241, critical: 22, medianAckS: 9.1, falsePositivePct: 3.0 },
  current: { medianAckS: 8, falsePositivePct: 2.4 },
  confidentiality: "Confidential · prepared for rig operations and HSE",
  website: "satyaki.co.in",
};

// ===== Printable report data (values from the report reference) =====

export type PillTone = "g" | "b" | "n" | "o" | "r" | "y";

/** Shift safety report · two-hour blocks (06 Oct day shift) */
export const shiftReport = {
  buckets: ["06–08", "08–10", "10–12", "12–14", "14–16", "16–18"],
  series: {
    advisory: [1, 2, 3, 3, 3, 2],
    warning: [1, 1, 2, 1, 2, 2],
    critical: [0, 2, 0, 1, 0, 0],
  },
  ticks: [0, 1, 2, 3, 4],
};

export const shiftCriticalRows: {
  id: string; time: string; severity: Severity; condition: string; location: string;
  ack: string; status: string; tone: PillTone;
}[] = [
  { id: "EV-1053", time: "13:16:40", severity: "Critical", condition: "Restricted-zone entry", location: "Rig 04 · Access corridor", ack: "6 s", status: "Closed", tone: "g" },
  { id: "EV-1042", time: "09:42:18", severity: "Critical", condition: "Occupied movement zone", location: "Rig 01 · Derrick board", ack: "8 s", status: "In review", tone: "b" },
  { id: "EV-1041", time: "09:42:07", severity: "Critical", condition: "Restricted-zone entry", location: "Rig 02 · Drill floor", ack: "24 s", status: "Closed", tone: "g" },
  { id: "EV-1039", time: "09:40:54", severity: "Warning", condition: "PPE visibility issue", location: "Rig 01 · Derrick board", ack: "11 s", status: "Acknowledged", tone: "n" },
  { id: "EV-1038", time: "09:40:00", severity: "Warning", condition: "Equipment proximity", location: "Rig 02 · Pipe handling", ack: "9 s", status: "Acknowledged", tone: "n" },
  { id: "EV-1037", time: "09:39:15", severity: "Warning", condition: "Restricted-zone entry", location: "Rig 04 · Access corridor", ack: "14 s", status: "Closed", tone: "g" },
  { id: "EV-1028", time: "08:56:03", severity: "Warning", condition: "Equipment proximity", location: "Rig 01 · Pipe handling", ack: "7 s", status: "Closed", tone: "g" },
  { id: "EV-1012", time: "07:18:40", severity: "Warning", condition: "Restricted-zone entry", location: "Rig 03 · Drill floor", ack: "5 s", status: "Closed", tone: "g" },
];

export const shiftCoverageGaps = [
  { cam: "CAM-06", area: "Rig 04 · Access corridor", window: "09:42 – 11:56", duration: "2 h 14 m", rules: "Access-zone monitoring" },
  { cam: "CAM-03", area: "Rig 01 · Drill floor", window: "15:05 – 15:07", duration: "2 m", rules: "None (restart)" },
];

export const shiftFindings = [
  { title: "Zone discipline at 09:40", text: "Two zone entries on two rigs within 11 s points to a crew-change pattern, not a single person." },
  { title: "Response held up", text: "Median acknowledgement stayed at 8 s. The slowest critical, EV-1041, took 24 s with no owner assigned." },
  { title: "Coverage gap is a risk", text: "EV-1053 on rig 04 came 80 min after CAM-06 returned. Events during the outage cannot be counted." },
];

/** HSE review report decisions (06 Oct) */
export const hseDecisions: {
  event: string; severity: Severity; condition: string; classification: string;
  evidence: string; evidenceTone: PillTone; followUp: string;
}[] = [
  { event: "EV-1042", severity: "Critical", condition: "Occupied movement zone", classification: "Confirmed unsafe exposure", evidence: "Sufficient", evidenceTone: "g", followUp: "CA-028" },
  { event: "EV-1041", severity: "Critical", condition: "Restricted-zone entry", classification: "Confirmed unsafe exposure", evidence: "Sufficient", evidenceTone: "g", followUp: "CA-029" },
  { event: "EV-1053", severity: "Critical", condition: "Restricted-zone entry", classification: "Authorized activity", evidence: "Sufficient", evidenceTone: "g", followUp: "None" },
  { event: "EV-1039", severity: "Warning", condition: "PPE visibility issue", classification: "Inconclusive", evidence: "Partial view", evidenceTone: "y", followUp: "Re-review" },
  { event: "EV-1038", severity: "Warning", condition: "Equipment proximity", classification: "False positive", evidence: "Sufficient", evidenceTone: "g", followUp: "Model note" },
  { event: "EV-1028", severity: "Warning", condition: "Equipment proximity", classification: "Confirmed unsafe exposure", evidence: "Sufficient", evidenceTone: "g", followUp: "Toolbox talk" },
];

export const hseCorrectiveActions: { id: string; title: string; linked: string; owner: string; due: string; status: string; tone: PillTone }[] = [
  { id: "CA-028", title: "Review crew positioning procedure", linked: "EV-1042", owner: "Faisal A.", due: "07 Oct 12:00", status: "In progress", tone: "b" },
  { id: "CA-029", title: "Barrier at drill-floor access gate", linked: "EV-1041", owner: "Nasser H.", due: "09 Oct 18:00", status: "Not started", tone: "n" },
];

/** Corrective action report · CA-028 */
export const actionReport = {
  checklist: [
    { done: true, text: "Procedure step added for positioning during block movement", note: "Done" },
    { done: false, text: "Briefing held with day and night derrick crews", note: "Open" },
    { done: false, text: "Evidence attached: signed briefing record", note: "Open" },
    { done: false, text: "HSE reviewer accepts closure", note: "Open" },
  ],
  progressLog: [
    { time: "06 Oct 09:47", text: "Action created by Maha S. and assigned to Faisal A." },
    { time: "06 Oct 10:05", text: "Owner accepted. Procedure review scheduled for the current shift." },
    { time: "06 Oct 12:40", text: "Draft procedure step written: crew stays behind the yellow line while the block moves." },
  ],
};

/** Shift handover report · Day → Night · 06 Oct */
export const handoverReport = {
  hazards: [
    { id: "EV-1039", severity: "Warning" as Severity, detail: "PPE visibility, derrick board. Classified inconclusive.", next: "Re-review with second view" },
    { id: "EV-1035", severity: "Advisory" as Severity, detail: "PPE review, drill floor. Acknowledged, not reviewed.", next: "HSE desk to review" },
  ],
  gaps: [
    { cam: "CAM-06", area: "Rig 04 · Access corridor", state: "Online", tone: "g" as PillTone, watch: "Was offline 09:42–11:56; confirm feed on first round" },
    { cam: "Beacon", area: "Rig 01 · Derrick board", state: "Simulation", tone: "b" as PillTone, watch: "Field beacon not connected; rely on radio call-out" },
  ],
  pendingActions: [
    { id: "CA-028", title: "Crew positioning procedure", owner: "Faisal A.", due: "07 Oct 12:00", note: "Brief night derrick crew before first trip" },
  ],
  checklist: [
    { done: true, text: "Open hazards reviewed with receiving supervisor", note: "18:02" },
    { done: true, text: "Monitoring gaps confirmed on live view", note: "18:04" },
    { done: true, text: "Pending actions transferred with deadlines", note: "18:05" },
    { done: false, text: "Receiving supervisor accepts handover", note: "Pending" },
  ],
};

/** Weekly fleet safety report · 30 Sep – 06 Oct 2026 */
export const weeklyReport = {
  labels: ["Wed 30", "Thu 01", "Fri 02", "Sat 03", "Sun 04", "Mon 05", "Tue 06"],
  series: {
    advisory: [18, 17, 16, 19, 20, 21, 22],
    warning: [9, 8, 11, 10, 12, 9, 14],
    critical: [3, 4, 2, 5, 4, 3, 6],
  },
  ticks: [0, 5, 10, 15, 20, 25],
  rigRows: [
    { rig: "Demo rig 01", region: "Eastern Province", readiness: "Ready", tone: "g" as PillTone, critical: 11, warning: 27, advisory: 52, open: 3 },
    { rig: "Demo rig 02", region: "Eastern Province", readiness: "Ready", tone: "g" as PillTone, critical: 8, warning: 21, advisory: 38, open: 2 },
    { rig: "Demo rig 03", region: "Offshore", readiness: "Ready", tone: "g" as PillTone, critical: 3, warning: 12, advisory: 25, open: 1 },
    { rig: "Demo rig 04", region: "Eastern Province", readiness: "Degraded", tone: "o" as PillTone, critical: 5, warning: 13, advisory: 18, open: 1 },
  ],
  typeBars: [
    { label: "Zone occupancy", count: 18 },
    { label: "PPE review", count: 12 },
    { label: "Equipment proximity", count: 8 },
    { label: "Posture observation", count: 4 },
  ],
  busiest: { peak: "10:00–16:00", quietest: "02:00–06:00", inPeak: "15", nightShare: "38" },
  findings: [
    { title: "Rig 01 leads volume", text: "39% of the week's events came from rig 01, mostly zone occupancy during tubular handling." },
    { title: "Rig 04 is under-counted", text: "Degraded coverage on CAM-06 means rig 04's lower count should not be read as safer." },
    { title: "Midday is the risk window", text: "Events cluster between 10:00 and 16:00, when tripping and pipe handling overlap." },
  ],
};

/** Pilot results report · Demo rig 01 */
export const pilotCriteria: { criterion: string; target: string; measured: string; status: string; tone: PillTone }[] = [
  { criterion: "Camera placement", target: "All monitored zones in view", measured: "Review required", status: "Review required", tone: "o" },
  { criterion: "Rules and use cases", target: "Signed off by HSE and operations", measured: "Approval required", status: "Approval required", tone: "o" },
  { criterion: "Detection quality", target: "Precision ≥ [TARGET %]", measured: "Not measured", status: "Not measured", tone: "n" },
  { criterion: "Alert latency", target: "Detection to alert ≤ [TARGET s]", measured: "Not measured", status: "Not measured", tone: "n" },
  { criterion: "Alarm interface", target: "Field beacon confirmed", measured: "Simulated only", status: "Simulated", tone: "b" },
  { criterion: "Site UAT", target: "Signed acceptance", measured: "Not started", status: "Not started", tone: "n" },
];

/** Event register · 06 Oct 2026 (11 of 42 records shown) */
export const registerRows: {
  id: string; occurred: string; severity: Severity; type: string; location: string;
  review: string; tone: PillTone; owner: string;
}[] = [
  { id: "EV-1053", occurred: "13:16:40", severity: "Critical", type: "Restricted-zone entry", location: "Rig 04 · Access corridor", review: "Closed", tone: "g", owner: "MS" },
  { id: "EV-1042", occurred: "09:42:18", severity: "Critical", type: "Occupied movement zone", location: "Rig 01 · Derrick board", review: "In review", tone: "b", owner: "MS" },
  { id: "EV-1041", occurred: "09:42:07", severity: "Critical", type: "Restricted-zone entry", location: "Rig 02 · Drill floor", review: "Closed", tone: "g", owner: "NH" },
  { id: "EV-1039", occurred: "09:40:54", severity: "Warning", type: "PPE visibility issue", location: "Rig 01 · Derrick board", review: "Acknowledged", tone: "n", owner: "FA" },
  { id: "EV-1038", occurred: "09:40:00", severity: "Warning", type: "Equipment proximity", location: "Rig 02 · Pipe handling", review: "Acknowledged", tone: "n", owner: "NH" },
  { id: "EV-1037", occurred: "09:39:15", severity: "Warning", type: "Restricted-zone entry", location: "Rig 04 · Access corridor", review: "Closed", tone: "g", owner: "OS" },
  { id: "EV-1036", occurred: "09:37:58", severity: "Advisory", type: "Posture observation", location: "Rig 03 · Derrick board", review: "Acknowledged", tone: "n", owner: "HS" },
  { id: "EV-1035", occurred: "09:37:30", severity: "Advisory", type: "PPE review", location: "Rig 01 · Drill floor", review: "Acknowledged", tone: "n", owner: "FA" },
  { id: "EV-1028", occurred: "08:56:03", severity: "Warning", type: "Equipment proximity", location: "Rig 01 · Pipe handling", review: "Closed", tone: "g", owner: "MS" },
  { id: "EV-1017", occurred: "07:31:22", severity: "Advisory", type: "Posture observation", location: "Rig 03 · Derrick board", review: "Closed", tone: "g", owner: "MS" },
  { id: "EV-1012", occurred: "07:18:40", severity: "Warning", type: "Restricted-zone entry", location: "Rig 03 · Drill floor", review: "Closed", tone: "g", owner: "MS" },
];

/** Audit trail extract · EV-1042 (append-only, 8 entries) */
export const auditReportRows: {
  time: string; actor: string; action: string; entity: string; isTag: boolean;
  result: string; tone: PillTone;
}[] = [
  { time: "09:42:18", actor: "Edge service", action: "Event created", entity: "EV-1042", isTag: true, result: "Recorded", tone: "g" },
  { time: "09:42:26", actor: "Faisal A.", action: "Acknowledged", entity: "EV-1042", isTag: true, result: "Recorded", tone: "g" },
  { time: "09:43:02", actor: "Rule engine", action: "Hazard cleared", entity: "EV-1042", isTag: true, result: "Recorded", tone: "g" },
  { time: "09:45:10", actor: "Maha S.", action: "Review started", entity: "EV-1042", isTag: true, result: "Recorded", tone: "g" },
  { time: "09:47:00", actor: "Maha S.", action: "Action created", entity: "CA-028", isTag: true, result: "Recorded", tone: "g" },
  { time: "09:50:31", actor: "Faisal A.", action: "Draft rule saved", entity: "DZ-01 v4", isTag: true, result: "Pending approval", tone: "o" },
  { time: "09:52:04", actor: "Maha S.", action: "Review saved · confirmed unsafe exposure", entity: "EV-1042", isTag: true, result: "Recorded", tone: "g" },
  { time: "10:02:15", actor: "Faisal A.", action: "Event register exported", entity: "Register 06 Oct", isTag: true, result: "Recorded", tone: "g" },
];

export const auditReportFindings = [
  { title: "Detection to review", text: "Review began 2 min 52 s after detection, inside the 5 min target." },
  { title: "Human and system actions", text: "3 of 8 entries came from services. Each is attributed and timestamped." },
  { title: "Configuration change pending", text: "DZ-01 v4 was saved as a draft and is not live until HSE approves it." },
];

/** Seed for the device event log (SQLite). Weekly reports compute from these stored events. */
export const eventLogSeed = {
  /** Reporting weeks start on this weekday (0 = Sun … 3 = Wed). */
  weekStartDay: 3,
  rigs: ["Demo rig 01", "Demo rig 02", "Demo rig 03", "Demo rig 04"],
  types: ["Zone occupancy", "PPE review", "Equipment proximity", "Posture observation"],
  /** Per-day [advisory, warning, critical] counts, keyed by first day of each week. */
  weeks: {
    "2026-09-23": [[17, 9, 3], [16, 10, 3], [18, 8, 2], [19, 9, 4], [17, 11, 3], [18, 10, 4], [21, 12, 3]],
    "2026-09-30": [[18, 9, 3], [17, 8, 4], [16, 11, 2], [19, 10, 5], [20, 12, 4], [21, 9, 3], [22, 14, 6]],
  } as Record<string, [number, number, number][]>,
};
