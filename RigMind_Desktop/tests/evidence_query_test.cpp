// Evidence chat retrieval: question parsing, selection and prompt building (backend/evidence_query.h).
#include <iostream>

#include "backend/evidence_query.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
using json = nlohmann::json;
constexpr std::int64_t H = 3600000LL, D = 24 * H;
} // namespace

int main() {
    const std::int64_t now = 100 * D + 15 * H;      // 15:00 on day 100
    const std::int64_t midnight = 100 * D;           // local midnight of day 100
    const std::vector<std::string> cams = {"CAM-01", "CAM-02"};
    auto parse = [&](const std::string& q) { return rigmind::parse_evidence_question(q, now, midnight, cams); };

    // kinds
    check(parse("show me the Orange evidence").kind == "image", "orange means images");
    check(parse("any photos from the platform?").kind == "image", "photo means images");
    check(parse("Is there Red video?").kind == "video", "red means video");
    check(parse("find clips").kind == "video", "clips are video");
    check(parse("what happened").kind.empty(), "no kind words: both kinds");
    check(parse("Orange images and video").kind.empty(), "both asked: both kinds");
    check(parse("the required report").kind.empty(), "'required' is not red");

    // cameras
    check(parse("evidence from cam-02 please").cam == "CAM-02", "camera matched case-insensitively");
    check(parse("evidence").cam.empty(), "no camera named");

    // time windows
    const auto today = parse("Orange images today");
    check(today.since_ms == midnight && today.window == "today", "today starts at local midnight");
    const auto yday = parse("what was saved yesterday");
    check(yday.since_ms == midnight - D && yday.until_ms == midnight && yday.window == "yesterday", "yesterday is the day before");
    const auto hour = parse("anything in the last hour?");
    check(hour.since_ms == now - H && hour.window == "the last hour", "last hour");
    const auto three = parse("evidence from the last 3 hours");
    check(three.since_ms == now - 3 * H && three.window == "the last 3 hours", "last 3 hours");
    const auto mins = parse("past 45 minutes");
    check(mins.since_ms == now - 45 * 60000LL && mins.window == "the last 45 minutes", "past 45 minutes");
    check(parse("last 2 days").since_ms == now - 2 * D, "last 2 days");
    check(parse("something").since_ms == now - 7 * D && parse("something").window == "the last 7 days", "default is the 7-day retention");

    // limits
    check(parse("latest Orange evidence").limit == 1, "latest means one");
    check(parse("list all Red video").limit == 12, "all means many");
    check(parse("evidence").limit == 6, "default limit");

    // selection
    const json rows = json::array({
        {{"camId", "CAM-01"}, {"timestampMs", now - 30 * 60000LL}, {"clipCount", 3}, {"path", "CAM-01/a"}, {"kind", "image"}},
        {{"camId", "CAM-01"}, {"timestampMs", now - 2 * H}, {"clipCount", 3}, {"path", "CAM-01/b"}, {"kind", "image"}},
        {{"camId", "CAM-02"}, {"timestampMs", now - 10 * 60000LL}, {"clipCount", 2}, {"path", "CAM-02/c"}},   // old row without kind = video
        {{"camId", "CAM-01"}, {"timestampMs", midnight - H}, {"clipCount", 1}, {"path", "CAM-01/d"}, {"kind", "image"}},
    });
    auto sel = [&](const std::string& q) { return rigmind::select_evidence(rows, parse(q), now); };
    check(sel("Orange images").size() == 3, "all images in 7 days");
    check(sel("Orange images today").size() == 2, "today excludes the set from before midnight");
    check(sel("Orange images today")[0]["path"] == "CAM-01/a", "newest first");
    check(sel("latest Orange").size() == 1 && sel("latest Orange")[0]["path"] == "CAM-01/a", "latest = the newest one");
    check(sel("video").size() == 1 && sel("video")[0]["path"] == "CAM-02/c", "rows without a kind count as video");
    check(sel("evidence from CAM-02").size() == 1, "camera filter");
    check(sel("images in the last hour").size() == 1, "time window filter");
    check(sel("images yesterday").size() == 1 && sel("images yesterday")[0]["path"] == "CAM-01/d", "yesterday window has an end");
    check(sel("red video from CAM-01").empty(), "no match is an empty list");

    // plain answer
    const auto q = parse("Orange images today");
    check(rigmind::describe_matches(rigmind::select_evidence(rows, q, now), q, 2) == "Found 2 Orange image sets in today.", "plain answer with counts");
    const auto none = parse("red video from CAM-01");
    const std::string miss = rigmind::describe_matches(json::array(), none, 0);
    check(miss.find("No Red video sets found for CAM-01") == 0 && miss.find("7 days") != std::string::npos, "plain answer when nothing matches");

    // prompt
    std::string lines;
    for (int i = 0; i < 100; ++i) lines += "Orange image set, CAM-01, 30 Oct 14:" + std::to_string(10 + i % 50) + ", 3 files\n";
    const std::string prompt = rigmind::evidence_chat_prompt("What happened?", "Found 100 sets.", lines, "", 800);
    check(prompt.find("Question: What happened?") != std::string::npos && prompt.find("never invent") == std::string::npos && prompt.find("Never invent") != std::string::npos, "prompt carries the question and the rule");
    check(prompt.size() < 800 + 300, "data block is capped");
    check(rigmind::evidence_chat_prompt("q", "n", "", "", 800).find("none") != std::string::npos, "empty data says none");

    if (failures) return 1;
    std::cout << "evidence_query tests passed\n";
    return 0;
}
