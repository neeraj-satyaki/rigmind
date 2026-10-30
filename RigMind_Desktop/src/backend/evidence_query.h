// Evidence chat, the finding part: turns a question such as "latest Orange images from CAM-01 today" into a filter over the saved
// evidence sets, builds the plain match summary that works without any model, and the prompt that lets BitNet explain from the
// matched data only. Header-only and free of I/O, so it is unit tested without a database.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rigmind {

struct EvidenceQuery {
    std::string kind;            // "image" (Orange), "video" (Red) or "" for both
    std::string cam;             // camera id, "" for all
    std::int64_t since_ms = 0;   // inclusive
    std::int64_t until_ms = 0;   // exclusive; 0 = now
    int limit = 6;
    std::string window;          // words for the answer: "today", "the last hour", "the last 7 days"
};

namespace evq {

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
inline bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }
// A whole word: "red" is not in "required" or "stored".
inline bool has_word(const std::string& text, const std::string& word) {
    for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        const bool left = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
        const bool right = at + word.size() >= text.size() || !std::isalpha(static_cast<unsigned char>(text[at + word.size()]));
        if (left && right) return true;
    }
    return false;
}

// "last 3 hours" / "past 45 minutes" / "last 2 days": the number and the unit (ms), 0 when none.
inline std::int64_t last_n_ms(const std::string& q, std::string* label) {
    for (const char* lead : {"last ", "past "}) {
        for (std::size_t at = q.find(lead); at != std::string::npos; at = q.find(lead, at + 1)) {
            std::size_t i = at + std::string(lead).size();
            int n = 0, digits = 0;
            while (i < q.size() && std::isdigit(static_cast<unsigned char>(q[i])) && digits < 4) { n = n * 10 + (q[i] - '0'); ++i; ++digits; }
            if (digits == 0) { // "last hour", "past day"
                n = 1;
            } else {
                while (i < q.size() && q[i] == ' ') ++i;
            }
            std::int64_t unit = 0;
            const char* name = "";
            if (q.compare(i, 4, "hour") == 0) { unit = 3600000LL; name = "hour"; }
            else if (q.compare(i, 3, "min") == 0) { unit = 60000LL; name = "minute"; }
            else if (q.compare(i, 3, "day") == 0) { unit = 86400000LL; name = "day"; }
            else if (q.compare(i, 4, "week") == 0) { unit = 7 * 86400000LL; name = "week"; }
            if (unit > 0 && n > 0) {
                if (label) *label = "the last " + (n == 1 ? std::string(name) : std::to_string(n) + " " + name + "s");
                return n * unit;
            }
        }
    }
    return 0;
}

} // namespace evq

// local_midnight_ms: the start of the current local day (the caller knows the time zone); known_cams: camera ids seen in the data.
inline EvidenceQuery parse_evidence_question(const std::string& question, std::int64_t now_ms, std::int64_t local_midnight_ms,
                                             const std::vector<std::string>& known_cams) {
    using namespace evq;
    const std::string q = lower(question);
    EvidenceQuery out;
    out.since_ms = now_ms - 7 * 86400000LL; // evidence is kept 7 days
    out.window = "the last 7 days";

    if (has(q, "orange") || has(q, "image") || has(q, "photo") || has(q, "picture") || has(q, "snapshot")) out.kind = "image";
    if (has_word(q, "red")) out.kind = "video";
    if (has(q, "video") || has(q, "clip") || has(q, "footage")) out.kind = "video";
    if ((has(q, "orange") || has(q, "image")) && (has(q, "video") || has(q, "clip"))) out.kind.clear(); // both asked: both kinds

    for (const std::string& cam : known_cams) {
        if (!cam.empty() && has(q, lower(cam).c_str())) out.cam = cam;
    }

    std::string label;
    if (const std::int64_t span = last_n_ms(q, &label)) {
        out.since_ms = now_ms - span;
        out.window = label;
    } else if (has(q, "yesterday")) {
        out.since_ms = local_midnight_ms - 86400000LL;
        out.until_ms = local_midnight_ms;
        out.window = "yesterday";
    } else if (has(q, "today") || has(q, "this morning") || has(q, "tonight")) {
        out.since_ms = local_midnight_ms;
        out.window = "today";
    }

    if (has(q, "latest") || has(q, "most recent") || has(q, "newest") || (has(q, "last ") && out.window == "the last 7 days")) out.limit = 1;
    if (has(q, "all ") || has(q, "every") || has(q, "how many") || has(q, "list")) out.limit = 12;
    return out;
}

// Evidence rows (footage.list: camId, timestampMs, clipCount, path, kind) that satisfy the query, newest first.
inline nlohmann::json select_evidence(const nlohmann::json& rows, const EvidenceQuery& query, std::int64_t now_ms) {
    std::vector<nlohmann::json> hit;
    for (const auto& row : rows) {
        const std::int64_t t = row.value("timestampMs", std::int64_t{0});
        const std::string kind = row.value("kind", std::string("video"));
        if (!query.kind.empty() && kind != query.kind) continue;
        if (!query.cam.empty() && row.value("camId", std::string()) != query.cam) continue;
        if (t < query.since_ms || t >= (query.until_ms ? query.until_ms : now_ms + 1)) continue;
        hit.push_back(row);
    }
    std::sort(hit.begin(), hit.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a.value("timestampMs", std::int64_t{0}) > b.value("timestampMs", std::int64_t{0}); });
    if (static_cast<int>(hit.size()) > query.limit) hit.resize(static_cast<std::size_t>(query.limit));
    return nlohmann::json(hit);
}

// The answer that needs no model: counts and the newest set, in plain words.
inline std::string describe_matches(const nlohmann::json& matches, const EvidenceQuery& query, int total_in_window) {
    const std::string what = query.kind == "image" ? "Orange image set" : query.kind == "video" ? "Red video set" : "evidence set";
    const std::string where = query.cam.empty() ? std::string() : " for " + query.cam;
    if (matches.empty())
        return "No " + what + "s found" + where + " in " + query.window + ". Orange alerts save images and Red alerts save video; evidence is kept for 7 days.";
    std::string out = "Found " + std::to_string(total_in_window) + " " + what + (total_in_window == 1 ? "" : "s") + where + " in " + query.window + ".";
    if (static_cast<int>(matches.size()) < total_in_window) out += " Showing the newest " + std::to_string(matches.size()) + ".";
    return out;
}

// The prompt that lets the model explain from the data and nothing else. `evidence_lines` and `alert_lines` are already formatted
// (one per line); the data block is capped so the prompt fits the model's context.
inline std::string evidence_chat_prompt(const std::string& question, const std::string& note, const std::string& evidence_lines,
                                        const std::string& alert_lines, std::size_t max_data_chars = 2200,
                                        const std::string& history_toon = std::string()) {
    std::string data = "Search result: " + note + "\n";
    data += "EVIDENCE SETS (newest first)\n" + (evidence_lines.empty() ? std::string("none\n") : evidence_lines);
    data += "ALERTS IN THE SAME PERIOD\n" + (alert_lines.empty() ? std::string("none\n") : alert_lines);
    if (data.size() > max_data_chars) {
        std::size_t cut = data.find_last_of('\n', max_data_chars);
        data.resize(cut == std::string::npos ? max_data_chars : cut + 1);
    }
    if (!history_toon.empty()) data += "EARLIER IN THIS CHAT (compact table)\n" + history_toon;
    return data + "\nQuestion: " + question +
           "\nAnswer in at most four sentences using only the data above. If the data does not answer the question, say that. Never invent anything.";
}

} // namespace rigmind
