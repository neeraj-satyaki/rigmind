#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace rigmind {

// Alert metadata is facts only (no sentences). The only plain-English description of an alert is the SLM's.
//
// Alarm alert metadata (table alert_metadata, one JSON per alert id):
//   {"v":1,"kind":"alarm","camera":"CAM-01","peak":"Orange","cycle":"Y>O>Y","closed":true,
//    "states":[{"state":"Yellow","tsMs":..,"frame":..,"facts":{...}}, ...]}
// facts = the EWS facts event nearest before the transition (see trim_facts), or null when none had arrived yet.
// PPE alert metadata: {"v":1,"kind":"ppe","camera":"CAM-01","missing":["vest"],"persons":2,"since_ms":..}

// Keeps the fact fields of a pipeline {"ev":"facts"} event: booleans, angle, positions, frame size, zoom, ROI and the
// per-fact percentages. Drops the diagnostics (frame counters, no-ROI variants, angle series).
nlohmann::json trim_facts(const nlohmann::json& facts_event);

// Neutral level label for the alert row: "Yellow monitoring" (0), "Orange alert" (1), "Red alert" (2).
const char* level_label(int level);

// Short human names for the facts that are true in one snapshot, with their values, as plain list items such as
// "link tilt angled 28 deg" or "elevator near platform". Deterministic; no sentences.
std::string facts_list(const nlohmann::json& facts);

// The SLM prompt for one alert, built from the stored metadata only: camera, cycle path, the facts true at each state
// with their values (angle, positions as percent of the frame), times; for PPE the items not detected and the people
// count. `severity` is the alert's severity; `label` the row's neutral condition label (used when no metadata exists).
std::string describe_prompt(const nlohmann::json& metadata, const std::string& severity, const std::string& label,
                            const std::string& camera, std::int64_t ts_ms);

} // namespace rigmind
