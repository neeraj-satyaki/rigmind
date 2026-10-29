#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace rigmind {

// Acceptance rules for the BitNet plain-language sentence. Same rules as scripts/slm_guard.py, so the
// Linux and Windows hosts show the same text. A rejected sentence leaves the template text on the page.
std::string first_sentence(const std::string& text);

// Empty string when the sentence is acceptable for this event, else the rejection reason.
std::string reject_reason(const std::string& sentence, const nlohmann::json& event);

} // namespace rigmind
