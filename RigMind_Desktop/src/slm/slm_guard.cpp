#include "slm_guard.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace rigmind {

namespace {

constexpr std::size_t kMaxChars = 360;
constexpr std::size_t kMinChars = 20;
const std::vector<std::string> kLeakMarkers = {"event:", "sentence:", "answer:", "facts:", "write one", "rewrite", "##", "**"};

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Characters, not bytes: UTF-8 continuation bytes are skipped.
std::size_t code_points(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

} // namespace

std::string first_sentences(const std::string& text, int max_sentences) {
    // First non-empty line.
    std::string line;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        const std::string candidate = trim(text.substr(start, newline == std::string::npos ? std::string::npos : newline - start));
        if (!candidate.empty()) {
            line = candidate;
            break;
        }
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    if (line.empty()) return {};

    // Drop list markers the model adds: "1: ", "1. ", or a run of ":-#*" then spaces.
    std::size_t i = 0;
    std::size_t digits_end = i;
    while (digits_end < line.size() && std::isdigit(static_cast<unsigned char>(line[digits_end]))) ++digits_end;
    if (digits_end > 0 && digits_end < line.size() && (line[digits_end] == ':' || line[digits_end] == '.' || line[digits_end] == ')')) {
        i = digits_end + 1;
    } else {
        while (i < line.size() && std::string(":-#*").find(line[i]) != std::string::npos) ++i;
    }
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    const std::string head = line.substr(i);

    // Cut after the max_sentences-th full stop, question or exclamation mark that ends a word.
    int seen = 0;
    for (std::size_t k = 0; k < head.size(); ++k) {
        const char c = head[k];
        if ((c == '.' || c == '!' || c == '?') && (k + 1 == head.size() || std::isspace(static_cast<unsigned char>(head[k + 1])))) {
            if (++seen >= max_sentences) return head.substr(0, k + 1);
        }
    }
    return head;
}

std::string first_sentence(const std::string& text) { return first_sentences(text, 1); }

bool has_action(const std::string& sentence) {
    static const std::vector<std::string> kCues = {"check", "stop", "confirm", "inspect", "review", "ensure", "verify", "must", "should",
                                                   "please", "clear the", "evacuate", "call ", "report to", "acknowledge", "needs to", "need to",
                                                   "make sure", "wear "};
    const std::string text = lower(sentence);
    for (const std::string& cue : kCues) {
        if (text.find(cue) != std::string::npos) return true;
    }
    return false;
}

std::string with_action(const std::string& sentence, const std::string& severity, bool* added) {
    if (added) *added = false;
    if (sentence.empty() || has_action(sentence)) return sentence;
    const char* action = severity == "Critical" ? "Stop the work and check the area now."
                         : severity == "Warning" ? "Check the area and confirm the crew is clear."
                                                 : "Confirm that everyone wears the required PPE.";
    std::string out = trim(sentence);
    if (out.back() != '.' && out.back() != '!' && out.back() != '?') out += '.';
    if (added) *added = true;
    return out + " " + action;
}

std::string reject_reason(const std::string& sentence, const nlohmann::json& event) {
    if (sentence.empty()) return "empty";
    const std::size_t length = code_points(sentence);
    if (length > kMaxChars || length < kMinChars) return "length " + std::to_string(length);

    const std::string text = lower(sentence);
    for (const std::string& marker : kLeakMarkers) {
        if (text.find(marker) != std::string::npos) return "leaked '" + marker + "'";
    }

    const std::string camera = lower(event.value("camId", std::string()));
    if (!camera.empty() && text.find(camera) == std::string::npos) return "missing camera";

    // Place: any part before or after the middle dot is enough ("Demo rig 01 · Derrick board").
    const std::string place = event.value("place", std::string());
    const std::string dot = "\xC2\xB7"; // U+00B7
    std::vector<std::string> parts;
    std::size_t from = 0;
    while (true) {
        const auto at = place.find(dot, from);
        parts.push_back(trim(place.substr(from, at == std::string::npos ? std::string::npos : at - from)));
        if (at == std::string::npos) break;
        from = at + dot.size();
    }
    bool has_part = false;
    bool found = false;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        has_part = true;
        if (text.find(lower(part)) != std::string::npos) found = true;
    }
    if (has_part && !found) return "missing place";
    return {};
}

std::string with_context(const std::string& sentence, const nlohmann::json& event) {
    const std::string reason = reject_reason(sentence, event);
    if (reason != "missing camera" && reason != "missing place") return sentence;
    const std::string camera = event.value("camId", std::string());
    const std::string place = event.value("place", std::string());
    std::string head;
    const std::string text = lower(sentence);
    if (!camera.empty() && text.find(lower(camera)) == std::string::npos) head = "Camera " + camera;
    // Place: any part of "Rig · Area" already named counts, as in reject_reason; otherwise the whole place goes in.
    bool place_named = place.empty();
    std::size_t from = 0;
    const std::string dot = "\xC2\xB7"; // U+00B7
    while (!place_named) {
        const auto at = place.find(dot, from);
        const std::string part = trim(place.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (!part.empty() && text.find(lower(part)) != std::string::npos) place_named = true;
        if (at == std::string::npos) break;
        from = at + dot.size();
    }
    if (!place_named) head += (head.empty() ? "" : ", ") + place;
    if (head.empty()) return sentence;
    std::string body = trim(sentence);
    // "At 30 Oct ..." / "The ..." read fine after the colon in lower case; keep names and acronyms as they are.
    if (body.size() > 1 && std::isupper(static_cast<unsigned char>(body[0])) && std::islower(static_cast<unsigned char>(body[1]))) body[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(body[0])));
    return head + ": " + body;
}

} // namespace rigmind
