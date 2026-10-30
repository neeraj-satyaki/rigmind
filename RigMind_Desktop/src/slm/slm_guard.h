#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace rigmind {

// Acceptance rules for the BitNet plain-language sentence. Same rules as scripts/slm_guard.py, so the
// Linux and Windows hosts show the same text. A rejected sentence leaves the template text on the page.
std::string first_sentence(const std::string& text);
// The first one or two sentences (max_sentences): normal cycles get a one or two line summary.
std::string first_sentences(const std::string& text, int max_sentences);

// True when the text already tells the reader what to do (check, stop, confirm, must, ...).
bool has_action(const std::string& sentence);

// The sentence with a fixed action added by severity when the model left one out: Critical "Stop the work and check the
// area now.", Warning "Check the area and confirm the crew is clear.", otherwise "Confirm that everyone wears the required
// PPE." A sentence that already has an action is returned unchanged. `added` (optional) reports whether text was appended.
std::string with_action(const std::string& sentence, const std::string& severity, bool* added = nullptr);

// Empty string when the sentence is acceptable for this event, else the rejection reason.
std::string reject_reason(const std::string& sentence, const nlohmann::json& event);

} // namespace rigmind
