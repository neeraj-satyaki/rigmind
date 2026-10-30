// Backend unit tests. Plain asserts so they build with MSVC and g++ alike, no WebView2 needed.
#include "backend/alert_metadata.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/alarm_recorder.h"
#include "backend/alarm_state.h"
#include "backend/audit_log.h"
#include "backend/bridge.h"
#include "slm/slm_guard.h"
#include "backend/event_store.h"
#include "pipeline/snapshot_plan.h"
#include "sqlite3.h"

using rigmind::AlarmState;
using rigmind::AlarmStateMachine;
using rigmind::AlarmThresholds;
using rigmind::FrameFacts;

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static FrameFacts yellow_frame() {
    FrameFacts f;
    f.machine_present = true;
    f.link_tilt_present = true;
    f.link_tilt_angled = true;
    return f;
}

static void test_zoom_thresholds() {
    CHECK(AlarmThresholds::for_zoom(false).yellow == 4);
    CHECK(AlarmThresholds::for_zoom(false).orange == 20);
    CHECK(AlarmThresholds::for_zoom(true).yellow == 30);
    CHECK(AlarmThresholds::for_zoom(true).orange == 100);
}

static void test_yellow_after_stable_frames() {
    AlarmStateMachine sm;
    const FrameFacts f = yellow_frame();
    for (int i = 0; i < 4; ++i) CHECK(sm.step(f) == AlarmState::None);
    CHECK(sm.step(f) == AlarmState::Yellow); // 5th frame: counter 5 > 4
}

static void test_escalation_to_orange_and_red() {
    AlarmStateMachine sm;
    FrameFacts near = yellow_frame();
    near.elevator_present = true;
    near.elevator_near_platform = true;

    // Yellow first, then orange counter must exceed 20 while elevator is near the platform.
    for (int i = 0; i < 5; ++i) sm.step(yellow_frame());
    CHECK(sm.current() == AlarmState::Yellow);

    int frames = 0;
    while (sm.current() != AlarmState::Orange && frames < 1000) {
        sm.step(near);
        ++frames;
    }
    CHECK(sm.current() == AlarmState::Orange);
    // Five yellow-only frames took the IDSS `orange--` branch, so orange starts at -5: 26 increments to pass 20.
    CHECK(frames == 26);

    // Collision drives red counter; red needs > 200. The 26 orange-phase frames already
    // decremented red to -26 (IDSS `--counter->red`), so 227 increments are needed.
    FrameFacts collide = near;
    collide.platform_elevator_collision = true;
    frames = 0;
    while (sm.current() != AlarmState::Red && frames < 1000) {
        sm.step(collide);
        ++frames;
    }
    CHECK(sm.current() == AlarmState::Red);
    CHECK(frames == 227);
    CHECK(sm.previous() == AlarmState::Red);
}

static void test_machine_absent_resets_to_yellow_only_when_tilt_near_derrickman() {
    AlarmStateMachine sm;
    for (int i = 0; i < 5; ++i) sm.step(yellow_frame());

    FrameFacts absent;
    absent.link_tilt_present = true;
    absent.link_tilt_near_derrickman = true;
    CHECK(sm.step(absent) == AlarmState::Yellow);

    FrameFacts clear;
    CHECK(sm.step(clear) == AlarmState::Yellow); // no link tilt, state holds
}

static void test_bridge_round_trip() {
    rigmind::EventStore store(":memory:");
    rigmind::Bridge bridge(store);

    const std::string frame = R"({"id":1,"method":"alarm.step","params":{"machinePresent":true,"linkTiltPresent":true,"linkTiltAngled":true}})";
    std::string reply;
    for (int i = 0; i < 5; ++i) reply = bridge.handle(frame);
    const auto parsed = nlohmann::json::parse(reply);
    CHECK(parsed["ok"] == true);
    CHECK(parsed["result"]["state"] == "YELLOW");
    CHECK(parsed["id"] == 1);

    const auto events = nlohmann::json::parse(bridge.handle(R"({"id":2,"method":"events.recent","params":{"limit":10}})"));
    CHECK(events["ok"] == true);
    CHECK(events["result"].size() == 1);
    CHECK(events["result"][0]["from"] == "NONE");
    CHECK(events["result"][0]["to"] == "YELLOW");

    const auto bad = nlohmann::json::parse(bridge.handle(R"({"id":3,"method":"nope"})"));
    CHECK(bad["ok"] == false);
}

static void test_alerts_and_records() {
    rigmind::EventStore store(":memory:");
    rigmind::Bridge bridge(store);

    const auto raised = nlohmann::json::parse(bridge.handle(
        R"({"id":4,"method":"alert.raise","params":{"id":"EV-1043","severity":"Critical","condition":"Occupied movement zone","place":"Demo rig 01 · Derrick board"}})"));
    CHECK(raised["ok"] == true);

    const auto responded = nlohmann::json::parse(bridge.handle(
        R"({"id":5,"method":"alert.respond","params":{"id":"EV-1043","status":"Acknowledged","owner":"Faisal A."}})"));
    CHECK(responded["ok"] == true);

    const auto bad_status = nlohmann::json::parse(bridge.handle(
        R"({"id":6,"method":"alert.respond","params":{"id":"EV-1043","status":"Closed","owner":"x"}})"));
    CHECK(bad_status["ok"] == false);

    // Seeded mock alerts were never raised: their responses must still be stored.
    const auto seeded = nlohmann::json::parse(bridge.handle(
        R"({"id":12,"method":"alert.respond","params":{"id":"EV-1042","status":"Escalated","owner":"HSE desk"}})"));
    CHECK(seeded["ok"] == true);

    const auto alerts = nlohmann::json::parse(bridge.handle(R"({"id":7,"method":"alerts.list"})"));
    CHECK(alerts["result"].size() == 2);
    // Newest response first; both were stored in the same millisecond or one after the other, so look them up by id.
    const auto response_for = [&](const char* id) {
        for (const auto& row : alerts["result"]) {
            if (row["id"] == id) return row;
        }
        return nlohmann::json::object();
    };
    CHECK(response_for("EV-1043")["status"] == "Acknowledged");
    CHECK(response_for("EV-1043")["owner"] == "Faisal A.");

    const auto put = nlohmann::json::parse(bridge.handle(
        R"({"id":8,"method":"records.set","params":{"key":"preferences","value":{"dark":true}}})"));
    CHECK(put["ok"] == true);
    const auto got = nlohmann::json::parse(bridge.handle(R"({"id":9,"method":"records.get","params":{"key":"preferences"}})"));
    CHECK(got["result"]["dark"] == true);
    const auto missing = nlohmann::json::parse(bridge.handle(R"({"id":10,"method":"records.get","params":{"key":"nope"}})"));
    CHECK(missing["result"].is_null());
    const auto listed = nlohmann::json::parse(bridge.handle(R"({"id":11,"method":"records.list","params":{"prefix":"pref"}})"));
    CHECK(listed["result"].size() == 1);
}

namespace {

struct FakeSink : rigmind::StreamSink {
    std::vector<nlohmann::json> commands;
    void send(const nlohmann::json& command) override { commands.push_back(command); }
};

struct NoModelSink : FakeSink {
    bool can_describe() const override { return false; }
};

} // namespace

static void test_stream_commands() {
    rigmind::EventStore store(":memory:");
    FakeSink sink;
    rigmind::Bridge bridge(store, &sink);

    const auto bad_scheme = nlohmann::json::parse(bridge.handle(
        R"({"id":1,"method":"stream.open","params":{"cam":"CAM-01","uri":"file:///etc/passwd"}})"));
    CHECK(bad_scheme["ok"] == false);

    const auto open = nlohmann::json::parse(bridge.handle(
        R"({"id":2,"method":"stream.open","params":{"cam":"CAM-01","uri":"rtsp://cam.local:554/live"}})"));
    CHECK(open["ok"] == true);
    CHECK(sink.commands.size() == 1);
    CHECK(sink.commands[0]["cmd"] == "open");
    CHECK(sink.commands[0]["uri"] == "rtsp://cam.local:554/live");

    bridge.handle(R"({"id":3,"method":"stream.offer","params":{"cam":"CAM-01","sdp":"v=0"}})");
    CHECK(sink.commands.size() == 2);
    CHECK(sink.commands[1]["cmd"] == "offer");

    rigmind::Bridge no_stream(store);
    const auto unavailable = nlohmann::json::parse(no_stream.handle(
        R"({"id":4,"method":"stream.close","params":{"cam":"CAM-01"}})"));
    CHECK(unavailable["ok"] == false);
}

static void test_simulate_detection_to_alerts() {
    rigmind::EventStore store(":memory:");
    rigmind::Bridge bridge(store);

    const auto red = nlohmann::json::parse(bridge.handle(
        R"({"id":1,"method":"alarm.simulate","params":{"scenario":"red","place":"Rig A · Deck"}})"));
    CHECK(red["ok"] == true);
    CHECK(red["result"]["state"] == "RED");
    const auto& alerts = red["result"]["alerts"];
    CHECK(alerts.size() == 3);
    CHECK(alerts[0]["severity"] == "Advisory");
    CHECK(alerts[1]["severity"] == "Warning");
    CHECK(alerts[2]["severity"] == "Critical");
    CHECK(alerts[2]["place"] == "Rig A · Deck");

    const auto events = nlohmann::json::parse(bridge.handle(R"({"id":2,"method":"events.recent","params":{"limit":10}})"));
    CHECK(events["result"].size() == 3);
    // Every simulated transition carries the id of its alert (joins to alerts.feed).
    for (const auto& row : events["result"]) CHECK(row["alertId"].get<std::string>().rfind("EV-", 0) == 0);
    CHECK(events["result"][0]["alertId"] == alerts[2]["id"]);

    const auto yellow = nlohmann::json::parse(bridge.handle(
        R"({"id":3,"method":"alarm.simulate","params":{"scenario":"yellow","place":"Rig A · Deck"}})"));
    CHECK(yellow["ok"] == true);
    CHECK(yellow["result"]["alerts"].size() == 1);

    const auto bad = nlohmann::json::parse(bridge.handle(
        R"({"id":4,"method":"alarm.simulate","params":{"scenario":"purple","place":"x"}})"));
    CHECK(bad["ok"] == false);
}

static void test_audit_trail() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-audit-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    {
        rigmind::EventStore store(db);
        rigmind::AuditLog audit(db);
        rigmind::Bridge bridge(store, nullptr, &audit);
        const auto ui = nlohmann::json::parse(bridge.handle(
            R"({"id":1,"method":"audit.write","params":{"entries":[{"ts":1000,"action":"click","target":"Acknowledge","detail":""},{"ts":1001,"action":"navigate","target":"/alerts"}]}})"));
        CHECK(ui["ok"] == true);
        CHECK(ui["result"]["accepted"] == 2);
        const auto bad = nlohmann::json::parse(bridge.handle(
            R"({"id":2,"method":"audit.write","params":{"entries":"nope"}})"));
        CHECK(bad["ok"] == false);
        bridge.handle(R"({"id":3,"method":"alert.respond","params":{"id":"EV-1042","status":"Acknowledged","owner":"Faisal A."}})");
    } // AuditLog destructor drains the queue before the database closes
    const std::string log = (dir / "audit.log").string();
    int lines = 0;
    {
        std::ifstream in(log); // closed before the folder is removed: Windows refuses to delete open files
        for (std::string line; std::getline(in, line);) ++lines;
    }
    CHECK(lines == 3);
    std::filesystem::remove_all(dir);
}

static void test_footage_retention() {
    const auto root = std::filesystem::temp_directory_path() / "rigmind-footage-test";
    std::filesystem::remove_all(root);
    const std::int64_t day = 24LL * 60 * 60 * 1000;
    const std::int64_t old_ts = 1000000000000LL;
    const std::int64_t fresh_ts = old_ts + 8 * day;
    std::filesystem::create_directories(root / "CAM-02" / std::to_string(old_ts));
    std::filesystem::create_directories(root / "CAM-02" / std::to_string(fresh_ts));

    rigmind::EventStore store(":memory:");
    store.set_footage_root(root.string());
    store.record_footage("CAM-02", old_ts, 3, "CAM-02/" + std::to_string(old_ts));
    store.record_footage("CAM-02", fresh_ts, 2, "CAM-02/" + std::to_string(fresh_ts));

    // Unsafe or mismatched paths are refused before anything reaches the disk.
    bool refused = false;
    try { store.record_footage("../etc", old_ts, 1, "../etc/" + std::to_string(old_ts)); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);

    // 8 days after the old clip: only the old one expires.
    CHECK(store.prune_footage(old_ts + 8 * day) == 1);
    CHECK(!std::filesystem::exists(root / "CAM-02" / std::to_string(old_ts)));
    CHECK(std::filesystem::exists(root / "CAM-02" / std::to_string(fresh_ts)));
    CHECK(store.list_footage(10).size() == 1);

    // Playback listing: only .mp4 files of a valid <cam>/<stamp> folder; traversal is refused.
    const auto clip = root / "CAM-02" / std::to_string(fresh_ts);
    std::ofstream(clip / "seg00001.mp4") << "x";
    std::ofstream(clip / "notes.txt") << "x";
    const auto listed = store.list_footage_files("CAM-02/" + std::to_string(fresh_ts));
    CHECK(listed["files"].size() == 1 && listed["files"][0] == "seg00001.mp4");
    bool listing_refused = false;
    try { store.list_footage_files("../etc/" + std::to_string(fresh_ts)); } catch (const std::runtime_error&) { listing_refused = true; }
    CHECK(listing_refused);
    std::filesystem::remove_all(root);
}

static void test_footage_images_and_migration() {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto root = tmp / "rigmind-footage-img-test";
    const auto dbfile = tmp / "rigmind-footage-old.db";
    std::filesystem::remove_all(root);
    std::filesystem::remove(dbfile);
    const std::int64_t ts = 1000000000000LL;
    // A database from before image evidence: footage without the kind column holds one video row.
    {
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(dbfile.string().c_str(), &db) == SQLITE_OK);
        CHECK(sqlite3_exec(db, "CREATE TABLE footage (id INTEGER PRIMARY KEY AUTOINCREMENT, cam_id TEXT NOT NULL, ts_ms INTEGER NOT NULL, clip_count INTEGER NOT NULL, path TEXT NOT NULL, expires_ms INTEGER NOT NULL);"
                               "INSERT INTO footage (cam_id, ts_ms, clip_count, path, expires_ms) VALUES ('CAM-01', 1000000000000, 2, 'CAM-01/1000000000000', 9999999999999);",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    {
        rigmind::EventStore store(dbfile.string());
        store.set_footage_root(root.string());
        const auto old_rows = store.list_footage(10);
        CHECK(old_rows.size() == 1 && old_rows[0]["kind"] == "video");
        const std::int64_t img_ts = ts + 5000;
        const auto dir = root / "CAM-01" / std::to_string(img_ts);
        std::filesystem::create_directories(dir);
        for (const char* n : {"orange-1.jpg", "orange-2.jpg", "orange-3.jpg"}) std::ofstream(dir / n) << "x";
        store.record_footage("CAM-01", img_ts, 3, "CAM-01/" + std::to_string(img_ts), "image");
        bool bad_kind = false;
        try { store.record_footage("CAM-01", img_ts + 1, 1, "CAM-01/" + std::to_string(img_ts + 1), "audio"); } catch (const std::runtime_error&) { bad_kind = true; }
        CHECK(bad_kind);
        const auto rows = store.list_footage(10);
        CHECK(rows.size() == 2 && rows[0]["kind"] == "image" && rows[0]["clipCount"] == 3 && rows[1]["kind"] == "video");
        const auto files = store.list_footage_files("CAM-01/" + std::to_string(img_ts));
        CHECK(files["kind"] == "image" && files["images"].size() == 3 && files["images"][0] == "orange-1.jpg" && files["files"].empty());
        // Same 7-day retention: the image set and its folder expire with the rows.
        CHECK(store.prune_footage(img_ts + 8LL * 24 * 60 * 60 * 1000) == 1);
        CHECK(!std::filesystem::exists(dir));
    }
    // Opening the migrated database again is harmless.
    { rigmind::EventStore again(dbfile.string()); CHECK(again.list_footage(10).size() >= 1); }
    std::filesystem::remove_all(root);
    std::filesystem::remove(dbfile);
}

static void test_snapshot_plan() {
    using namespace rigmind;
    CHECK(snapshots_due(0) == 1 && snapshots_due(2999) == 1 && snapshots_due(3000) == 2 && snapshots_due(5999) == 2 && snapshots_due(6000) == 3);
    CHECK(snapshot_file_name(2) == "orange-2.jpg");
    CHECK(!snapshot_set_complete(1, 3000) && !snapshot_set_complete(2, 6400) && snapshot_set_complete(2, 6501) && snapshot_set_complete(3, 6000));
    CHECK(snapshot_set_allowed(0, 100) && !snapshot_set_allowed(1000, 5000) && snapshot_set_allowed(1000, 21000));
}

static void test_slm_guard() {
    CHECK(rigmind::first_sentence("1: 28 Oct 2026 21:28, camera cam-03 recorded: Elevator. Extra.") ==
          "28 Oct 2026 21:28, camera cam-03 recorded: Elevator.");
    CHECK(rigmind::first_sentence(": At 28 Oct, camera CAM-05 recorded: Tilt.") == "At 28 Oct, camera CAM-05 recorded: Tilt.");
    CHECK(rigmind::first_sentence("28 Oct 2026 21:28 is the date.") == "28 Oct 2026 21:28 is the date.");
    CHECK(rigmind::first_sentence("") == "");
    {
        bool added = false;
        CHECK(rigmind::with_action("Camera CAM-01 at Rig 1 recorded a collision.", "Critical", &added) ==
                  "Camera CAM-01 at Rig 1 recorded a collision. Stop the work and check the area now." && added);
        CHECK(rigmind::with_action("Camera CAM-01 recorded an elevator near the platform", "Warning", &added) ==
                  "Camera CAM-01 recorded an elevator near the platform. Check the area and confirm the crew is clear." && added);
        CHECK(rigmind::with_action("Camera CAM-01 saw no vest.", "Advisory", &added).find("required PPE") != std::string::npos && added);
        CHECK(rigmind::with_action("Camera CAM-01 saw a collision; stop the work and check the area.", "Critical", &added) ==
                  "Camera CAM-01 saw a collision; stop the work and check the area." && !added);
        CHECK(rigmind::with_action("", "Critical", &added).empty() && !added);
        const nlohmann::json ev = {{"camId", "CAM-01"}, {"place", "Demo rig 01 · Derrick board"}};
        const std::string fixed = rigmind::with_context("At 30 Oct 2026 14:29, an elevator is near the platform.", ev);
        CHECK(fixed.find("Camera CAM-01, ") == 0 && fixed.find(": at 30 Oct 2026") != std::string::npos);
        CHECK(rigmind::reject_reason(fixed, ev).empty());
        CHECK(rigmind::with_context("Camera CAM-01 at Derrick board saw a collision.", ev) == "Camera CAM-01 at Derrick board saw a collision.");
        CHECK(rigmind::with_context("Elevator near Derrick board.", ev).find("Camera CAM-01") == 0);
        CHECK(rigmind::first_sentences("One thing happened. Check the area. Third one.", 2) == "One thing happened. Check the area.");
    }

    const nlohmann::json event = {{"camId", "CAM-03"}, {"place", "Demo rig 02 \xC2\xB7 Drill floor"}};
    CHECK(rigmind::reject_reason("At 28 Oct, camera cam-03 at Demo rig 02 recorded a fault.", event).empty());
    CHECK(rigmind::reject_reason("At 28 Oct, camera cam-03 at Drill floor recorded a fault.", event).empty());
    CHECK(!rigmind::reject_reason("At 28 Oct, camera cam-09 at Drill floor recorded a fault.", event).empty());
    CHECK(!rigmind::reject_reason("## At 28 Oct, camera cam-03 at Drill floor recorded a fault.", event).empty());
    CHECK(!rigmind::reject_reason("Short.", event).empty());
    CHECK(!rigmind::reject_reason(std::string(300, 'x'), event).empty());
}

static nlohmann::json alarm_line(const char* cam, const char* state, const char* condition = "", const char* place = "", int frame = 1) {
    return {{"ev", "alarm"}, {"cam", cam}, {"state", state}, {"previous", "x"}, {"severity", "x"},
            {"condition", condition}, {"place", place}, {"frame", frame}};
}

static int count_audit_lines(const std::filesystem::path& log, const std::string& action) {
    int n = 0;
    std::ifstream in(log);
    for (std::string line; std::getline(in, line);) {
        if (nlohmann::json::parse(line).value("action", "") == action) ++n;
    }
    return n;
}

static void test_alarm_episode_recorder() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-alarm-episode-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    {
        rigmind::EventStore store(db);
        rigmind::AuditLog audit(db);
        rigmind::AlarmRecorder rec(store, &audit, "b1");

        // Sequence None -> Yellow -> Orange -> Red -> None on one camera.
        auto e = rec.handle(alarm_line("CAM-01", "Yellow", "Link-tilt angled", "Drill floor", 10), 1000);
        const std::string id = e["alertId"];
        CHECK(id == "ALM-CAM-01-b11");
        CHECK(id.size() <= 32);
        CHECK(e["phase"] == "Monitoring" && e["escalation"] == 0 && e["episode"] == 1 && e["changed"] == true);
        auto rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["severity"] == "Advisory" && rows[0]["status"] == "Monitoring");

        // Repeated identical state changes nothing.
        e = rec.handle(alarm_line("CAM-01", "Yellow", "Link-tilt angled", "Drill floor", 11), 1100);
        CHECK(e["alertId"] == id && e["changed"] == false && e["phase"] == "Monitoring");
        CHECK(store.recent_transitions(50).size() == 1);

        e = rec.handle(alarm_line("CAM-01", "Orange", "Elevator near platform", "Drill floor", 30), 2000);
        CHECK(e["alertId"] == id && e["phase"] == "Escalated" && e["escalation"] == 1);
        rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["severity"] == "Warning" && rows[0]["status"] == "Escalated");
        CHECK(rows[0]["condition"] == "Orange alert" && rows[0]["place"] == "CAM-01"); // neutral label + camera; wording on the line is ignored

        e = rec.handle(alarm_line("CAM-01", "Red", "Platform-elevator collision", "Drill floor", 60), 3000);
        CHECK(e["alertId"] == id && e["phase"] == "Escalated" && e["escalation"] == 2);
        rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["severity"] == "Critical" && rows[0]["status"] == "Escalated");

        e = rec.handle(alarm_line("CAM-01", "None", "", "", 90), 4000);
        CHECK(e["alertId"] == id && e["phase"] == "Cleared");
        rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["severity"] == "Critical" && rows[0]["status"] == "Cleared");
        CHECK(rows[0]["condition"] == "Red alert"); // Cleared keeps the peak level label

        auto events = store.recent_transitions(50);
        CHECK(events.size() == 4);
        CHECK(events[3]["from"] == "NONE" && events[3]["to"] == "YELLOW" && events[3]["camId"] == "CAM-01" &&
              events[3]["alertId"] == id && events[3]["condition"] == "Yellow monitoring" &&
              events[3]["place"] == "CAM-01" && events[3]["frame"] == 10 && events[3]["severity"] == "Advisory");
        CHECK(events[0]["from"] == "RED" && events[0]["to"] == "NONE");

        // A repeated None changes nothing; a skip None -> Orange is recorded and opens a second episode.
        e = rec.handle(alarm_line("CAM-01", "None"), 4100);
        CHECK(e["changed"] == false && store.recent_transitions(50).size() == 4);
        e = rec.handle(alarm_line("CAM-01", "Orange", "Elevator near platform", "Drill floor", 5), 5000);
        CHECK(e["alertId"] == "ALM-CAM-01-b12" && e["episode"] == 2 && e["phase"] == "Escalated" && e["escalation"] == 1);
        rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 2);
        CHECK(store.recent_transitions(50).size() == 5);
        e = rec.handle(alarm_line("CAM-01", "Red", "Platform-elevator collision"), 5100); // Orange -> Red
        CHECK(e["alertId"] == "ALM-CAM-01-b12");
        rec.handle(alarm_line("CAM-01", "None"), 5200);

        // Yellow -> Red skip, another camera is independent, and a lone None with no episode is untouched.
        e = rec.handle(alarm_line("CAM-02", "Yellow", "a", "b"), 6000);
        CHECK(e["alertId"] == "ALM-CAM-02-b11");
        e = rec.handle(alarm_line("CAM-02", "Red", "c", "d"), 6100);
        CHECK(e["alertId"] == "ALM-CAM-02-b11" && e["escalation"] == 2);
        e = rec.handle(alarm_line("CAM-09", "None"), 6200);
        CHECK(!e.contains("alertId"));

        // Very long camera ids still give ids within 32 characters.
        e = rec.handle(alarm_line("CAMERA-WITH-A-VERY-LONG-IDENTIFIER-1", "Yellow"), 7000);
        CHECK(std::string(e["alertId"]).size() <= 32);

        // Acknowledging still works at any phase.
        store.respond_alert(id, "Acknowledged", "Faisal A.", 8000);
        CHECK(store.list_alerts()[0]["status"] == "Acknowledged");
        bool feed_acked = false, feed_ep2_escalated = false;
        for (const auto& row : store.list_raised_alerts(50)) {
            if (row["id"] == id) feed_acked = row["status"] == "Acknowledged" && row["owner"] == "Faisal A.";
            if (row["id"] == "ALM-CAM-01-b12") feed_ep2_escalated = row["status"] == "Cleared";
        }
        CHECK(feed_acked && feed_ep2_escalated); // responses overlay the feed until the alert changes again
    } // audit queue drained
    CHECK(count_audit_lines(dir / "audit.log", "alarm.transition") == 10);
    {
        std::ifstream in(dir / "audit.log");
        std::string line;
        std::getline(in, line);
        const auto first = nlohmann::json::parse(line);
        CHECK(first["source"] == "backend" && first["target"] == "ALM-CAM-01-b11" &&
              first["detail"] == "None -> Yellow | Yellow monitoring | CAM-01");
    }
    {
        rigmind::EventStore reopened(db);
        const auto audited = reopened.recent_audit("alarm.transition", 50); // real audit rows, newest first
        CHECK(audited.size() == 10);
        CHECK(audited[9]["detail"] == "None -> Yellow | Yellow monitoring | CAM-01");
        CHECK(reopened.recent_audit("nothing.here", 5).empty());
    }
    std::filesystem::remove_all(dir);
}

static void test_event_cycles() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-cycle-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    {
        rigmind::EventStore store(db);
        rigmind::AuditLog audit(db);
        rigmind::AlarmRecorder rec(store, &audit, "c1");
        std::int64_t t = 1000;
        auto step = [&](const char* state) { return rec.handle(alarm_line("CAM-01", state, "Elevator near platform", "Drill floor"), t += 100); };

        // Yellow > Orange > Yellow: one normal cycle; that Yellow opens the next row.
        auto e = step("Yellow");
        const std::string first = e["alertId"];
        CHECK(!e.contains("closed"));
        e = step("Orange");
        CHECK(e["alertId"] == first && e["phase"] == "Escalated");
        e = step("Yellow");
        CHECK(e.contains("closed") && e["closed"]["alertId"] == first && e["closed"]["kind"] == "normal" && e["closed"]["critical"] == false);
        CHECK(e["closed"]["cycle"] == "Y>O>Y" && e["closed"]["peak"] == 1);
        const std::string second = e["alertId"];
        CHECK(second != first && e["phase"] == "Monitoring" && e["episode"] == 2);

        // Yellow > Orange > Red > Orange > Yellow: one critical cycle.
        step("Orange");
        step("Red");
        e = step("Orange");
        CHECK(e["alertId"] == second && !e.contains("closed"));
        e = step("Yellow");
        CHECK(e["closed"]["alertId"] == second && e["closed"]["kind"] == "critical" && e["closed"]["cycle"] == "Y>O>R>O>Y" && e["closed"]["peak"] == 2);
        const std::string third = e["alertId"];

        // Yellow > Orange > Red > Yellow: also critical.
        step("Orange");
        step("Red");
        e = step("Yellow");
        CHECK(e["closed"]["alertId"] == third && e["closed"]["critical"] == true && e["closed"]["cycle"] == "Y>O>R>Y");
        const std::string fourth = e["alertId"];

        // A Yellow-only stretch ending in None is not an event cycle (peak 0) but is still cleared.
        e = step("None");
        CHECK(e["alertId"] == fourth && e["phase"] == "Cleared");

        // Rows: closed cycles are Cleared with their peak severity; the first is Warning, the second Critical.
        std::string status1, sev1, sev2;
        for (const auto& row : store.list_raised_alerts(50)) {
            if (row["id"] == first) { status1 = row["status"]; sev1 = row["severity"]; }
            if (row["id"] == second) sev2 = row["severity"];
        }
        CHECK(status1 == "Cleared" && sev1 == "Warning" && sev2 == "Critical");
    }
    CHECK(count_audit_lines(dir / "audit.log", "alarm.cycle") == 3);
    std::filesystem::remove_all(dir);
}

static nlohmann::json ppe_line(const char* cam, const char* state, std::vector<std::string> missing, int persons = 1) {
    return {{"ev", "ppe"}, {"cam", cam}, {"state", state}, {"missing", missing}, {"persons", persons}, {"since_ms", 1}};
}

static void test_ppe_episode_recorder() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-ppe-episode-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    {
        rigmind::EventStore store(db);
        rigmind::AuditLog audit(db);
        rigmind::PpeRecorder rec(store, &audit, "b1");

        auto e = rec.handle(ppe_line("CAM-01", "missing", {"vest"}), 1000);
        const std::string id = e["alertId"];
        CHECK(id == "PPE-CAM-01-b11" && id.size() <= 32);
        CHECK(e["phase"] == "Open" && e["changed"] == true && e["condition"] == "PPE items not detected" && e["severity"] == "Advisory" && e["action"] == "raised");
        auto rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["severity"] == "Advisory" && rows[0]["status"] == "Open" &&
              rows[0]["condition"] == "PPE items not detected" && rows[0]["place"] == "CAM-01" && rows[0]["raisedMs"] == 1000);
        CHECK(rows[0]["metadata"]["kind"] == "ppe" && rows[0]["metadata"]["camera"] == "CAM-01" && rows[0]["metadata"]["missing"] == nlohmann::json::array({"vest"}) &&
              rows[0]["metadata"]["persons"] == 1 && rows[0]["metadata"]["since_ms"] == 1);

        // Same missing set: nothing changes. A different set updates the same row in place.
        e = rec.handle(ppe_line("CAM-01", "missing", {"vest"}), 1100);
        CHECK(e["alertId"] == id && e["changed"] == false);
        e = rec.handle(ppe_line("CAM-01", "missing", {"vest", "gloves"}, 2), 2000);
        CHECK(e["alertId"] == id && e["changed"] == true && e["condition"] == "PPE items not detected" && e["action"] == "updated");
        rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["condition"] == "PPE items not detected" && rows[0]["status"] == "Open" &&
              rows[0]["raisedMs"] == 1000 && rows[0]["updatedMs"] == 2000);
        CHECK(rows[0]["metadata"]["missing"] == nlohmann::json::array({"vest", "gloves"}) && rows[0]["metadata"]["persons"] == 2);

        // An 'ok' with no episode on another camera is ignored; the open one is cleared and keeps its condition.
        e = rec.handle(ppe_line("CAM-09", "ok", {}), 2500);
        CHECK(!e.contains("alertId"));
        e = rec.handle(ppe_line("CAM-01", "ok", {}), 3000);
        CHECK(e["alertId"] == id && e["phase"] == "Cleared" && e["changed"] == true);
        rows = store.list_raised_alerts(10);
        CHECK(rows.size() == 1 && rows[0]["status"] == "Cleared" && rows[0]["condition"] == "PPE items not detected" &&
              rows[0]["metadata"]["closed"] == true && rows[0]["metadata"]["missing"].size() == 2);
        e = rec.handle(ppe_line("CAM-01", "ok", {}), 3100);
        CHECK(e["changed"] == false || !e.contains("alertId"));

        // A new episode gets a new id; the first row stays.
        e = rec.handle(ppe_line("CAM-01", "missing", {"helmet"}), 4000);
        CHECK(e["alertId"] == "PPE-CAM-01-b12" && e["episode"] == 2);
        CHECK(store.list_raised_alerts(10).size() == 2);
        // Acknowledging works like any alert.
        store.respond_alert("PPE-CAM-01-b12", "Acknowledged", "Faisal A.", 4500);
        bool acked = false;
        for (const auto& row : store.list_raised_alerts(10))
            if (row["id"] == "PPE-CAM-01-b12") acked = row["status"] == "Acknowledged";
        CHECK(acked);
        // Alarm episodes are untouched: no alarm_events rows from PPE lines.
        CHECK(store.recent_transitions(10).empty());
        // Not valid ppe lines are returned unchanged.
        CHECK(!rec.handle({{"ev", "ppe"}, {"cam", "CAM-01"}, {"state", "weird"}}, 5000).contains("alertId"));
    }
    CHECK(count_audit_lines(dir / "audit.log", "ppe.transition") == 4); // raised, updated, cleared, raised (episode 2)
    {
        rigmind::EventStore reopened(db);
        const auto audited = reopened.recent_audit("ppe.transition", 20);
        CHECK(audited.size() == 4);
        CHECK(audited[3]["source"] == "backend" && audited[3]["target"] == "PPE-CAM-01-b11" &&
              audited[3]["detail"] == "raised | PPE items not detected: vest | persons 1 | CAM-01");
        CHECK(audited[2]["detail"] == "updated | PPE items not detected: vest, gloves | persons 2 | CAM-01");
        CHECK(audited[1]["detail"].get<std::string>().rfind("cleared | PPE items not detected: vest, gloves", 0) == 0);
    }
    std::filesystem::remove_all(dir);
}

static void test_alarm_events_migration() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-alarm-migrate-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    {
        sqlite3* old = nullptr;
        CHECK(sqlite3_open(db.c_str(), &old) == SQLITE_OK);
        const char* sql =
            "CREATE TABLE alarm_events (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp_ms INTEGER NOT NULL,"
            " from_state TEXT NOT NULL, to_state TEXT NOT NULL);"
            "INSERT INTO alarm_events (timestamp_ms, from_state, to_state) VALUES (42, 'None', 'Yellow');"
            "CREATE TABLE alerts (id TEXT PRIMARY KEY, severity TEXT NOT NULL, condition TEXT NOT NULL, place TEXT NOT NULL,"
            " status TEXT NOT NULL DEFAULT 'Open', owner TEXT NOT NULL DEFAULT 'Unassigned', raised_ms INTEGER NOT NULL,"
            " updated_ms INTEGER NOT NULL);";
        CHECK(sqlite3_exec(old, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(old);
    }
    for (int pass = 0; pass < 2; ++pass) { // second open: the guarded migration is idempotent
        rigmind::EventStore store(db);
        auto rows = store.recent_transitions(10);
        if (pass == 1) { CHECK(rows.size() == 2); rows = store.recent_transitions(10); rows.erase(rows.begin()); }
        CHECK(rows.size() == 1 && rows[0]["timestampMs"] == 42 && rows[0]["to"] == "Yellow");
        CHECK(rows[0]["camId"] == "" && rows[0]["alertId"] == "" && rows[0]["frame"] == 0);
        if (pass == 0) {
            rigmind::AlarmRecorder rec(store, nullptr, "m1");
            rec.handle(alarm_line("CAM-01", "Orange", "c", "p", 7), 100);
            rows = store.recent_transitions(10);
            CHECK(rows.size() == 2 && rows[0]["camId"] == "CAM-01" && rows[0]["frame"] == 7);
        }
    }
    std::filesystem::remove_all(dir);
}
// event.describe: the prompt comes from the metadata stored for the alert id (never from page text); audit detail is "slm" only
// when the sink can reach a model; otherwise "unavailable", no command, and the row says so.
static nlohmann::json facts_line(const char* cam, bool angled, bool near_platform, double angle, double derrickman_cx) {
    return {{"ev", "facts"}, {"cam", cam}, {"zoomed", false}, {"scale_mode", "x"}, {"frames", 12}, {"w", 1000}, {"h", 500},
            {"machine_present", true}, {"link_tilt_present", true}, {"link_tilt_angled", angled}, {"elevator_present", near_platform},
            {"elevator_near_platform", near_platform}, {"angle_deg", angle}, {"derrickman_cx", derrickman_cx}, {"machine_cx", 600.0},
            {"pct", {{"link_tilt_angled", 80.0}}}, {"pct_no_roi", {{"x", 1}}}, {"angles", {1, 2}}, {"roi", {10, 20, 900, 480}}};
}

static void test_describe_source() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-describe-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    // Page-supplied wording must be ignored: only the alert id counts.
    const char* request =
        R"({"id":1,"method":"event.describe","params":{"alertId":"ALM-CAM-01-d11","severity":"Critical","condition":"PAGE-SUPPLIED-WORDS","place":"Demo rig 01","camId":"CAM-09"}})";
    FakeSink with_model;
    NoModelSink without_model;
    {
        rigmind::EventStore store(db);
        rigmind::AuditLog audit(db);
        rigmind::AlarmRecorder rec(store, &audit, "d1");
        rec.note_facts(facts_line("CAM-01", true, false, 28.4, 410.0), 900);
        rec.handle(alarm_line("CAM-01", "Yellow"), 1000);
        rec.note_facts(facts_line("CAM-01", true, true, 31.0, 410.0), 1900);
        rec.handle(alarm_line("CAM-01", "Orange"), 2000);
        rigmind::Bridge up(store, &with_model, &audit);
        const auto a = nlohmann::json::parse(up.handle(request));
        CHECK(a["ok"] == true && a["result"]["slm"] == true && a["result"]["text"].is_null());
        CHECK(with_model.commands.size() == 1 && with_model.commands[0]["cmd"] == "describe");
        const auto& cmd = with_model.commands[0];
        const std::string prompt = cmd["prompt"];
        CHECK(cmd["camId"] == "CAM-01" && cmd["place"] == "CAM-01" && cmd["severity"] == "Warning"); // from the stored row
        CHECK(prompt.find("PAGE-SUPPLIED-WORDS") == std::string::npos && prompt.find("CAM-09") == std::string::npos);
        CHECK(prompt.find("Camera CAM-01 reached the Orange level") != std::string::npos && prompt.find("severity Warning") != std::string::npos);
        CHECK(prompt.find("Yellow > Orange, still going") != std::string::npos);
        CHECK(prompt.find("link tilt angled 28 degrees") != std::string::npos && prompt.find("link tilt angled 31 degrees") != std::string::npos);
        CHECK(prompt.find("elevator near platform") != std::string::npos);
        CHECK(prompt.find("derrickman at 41% of the frame width") != std::string::npos && prompt.find("machine at 60%") != std::string::npos);
        rigmind::Bridge down(store, &without_model, &audit);
        const auto b = nlohmann::json::parse(down.handle(request));
        CHECK(b["ok"] == true);
        CHECK(b["result"]["slm"] == false && b["result"]["text"].is_null());
        CHECK(without_model.commands.empty());
        const auto row = store.find_raised_alert("ALM-CAM-01-d11");
        CHECK(row["summary"].is_null() && row["summarySource"] == "unavailable");
        // The SLM result is stored by the host: text survives, a later failed attempt does not wipe it.
        store.put_alert_summary("ALM-CAM-01-d11", "Camera CAM-01 saw the elevator near the platform. Check the area.", "slm", 3000);
        store.put_alert_summary("ALM-CAM-01-d11", "", "template", 3100);
        const auto done = store.list_raised_alerts(5)[0];
        CHECK(done["summary"] == "Camera CAM-01 saw the elevator near the platform. Check the area." && done["summarySource"] == "slm" && done["summaryAt"] == 3000);
        // alert.get: one row with metadata and summary, null for an unknown id
        const auto got = nlohmann::json::parse(up.handle(R"({"id":3,"method":"alert.get","params":{"id":"ALM-CAM-01-d11"}})"));
        CHECK(got["ok"] == true && got["result"]["metadata"]["cycle"] == "Y>O" && got["result"]["summarySource"] == "slm" && got["result"]["place"] == "CAM-01");
        CHECK(nlohmann::json::parse(up.handle(R"({"id":4,"method":"alert.get","params":{"id":"nope"}})"))["result"].is_null());
        // An alert with no stored metadata still gets a neutral prompt (label + camera), not a made-up sentence.
        const auto c = nlohmann::json::parse(up.handle(R"({"id":2,"method":"event.describe","params":{"alertId":"EV-9","severity":"Critical","camId":"CAM-01"}})"));
        CHECK(c["ok"] == true && c["result"]["hasMetadata"] == false);
        CHECK(with_model.commands.size() == 2 && std::string(with_model.commands[1]["prompt"]).find("Camera CAM-01") != std::string::npos);
    }
    std::string log;
    {
        std::ifstream in((dir / "audit.log").string());
        log.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const auto slm_row = log.find("event.describe");
    CHECK(slm_row != std::string::npos);
    const auto second = log.find("event.describe", slm_row + 1);
    CHECK(second != std::string::npos);
    CHECK(log.substr(slm_row, log.find('\n', slm_row) - slm_row).find("slm") != std::string::npos);
    CHECK(log.substr(second, log.find('\n', second) - second).find("unavailable") != std::string::npos);
    std::filesystem::remove_all(dir);
}

static const nlohmann::json* find_row(const nlohmann::json& rows, const std::string& id) {
    for (const auto& r : rows)
        if (r["id"] == id) return &r;
    return nullptr;
}

// Alert metadata is facts only: per state change the EWS facts true at that moment, the cycle path and the peak; the row keeps a
// neutral peak label (also after the cycle closes); persistence survives a restart.
static void test_alert_metadata() {
    const auto dir = std::filesystem::temp_directory_path() / "rigmind-metadata-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string db = (dir / "rigmind.db").string();
    std::string first, second, third;
    {
        rigmind::EventStore store(db);
        rigmind::AlarmRecorder rec(store, nullptr, "m1");
        // no facts yet: the first state change carries null facts
        auto e = rec.handle(alarm_line("CAM-01", "Yellow"), 1000);
        first = e["alertId"];
        CHECK(e["condition"] == "Yellow monitoring" && e["place"] == "CAM-01" && e["peakRaised"] == true);
        rec.note_facts(facts_line("CAM-01", true, false, 28.4, 410.0), 1500);
        e = rec.handle(alarm_line("CAM-01", "Yellow"), 1600);
        CHECK(e["changed"] == false && e["peakRaised"] == false);
        rec.note_facts(facts_line("CAM-01", true, true, 31.0, 420.0), 2900);
        e = rec.handle(alarm_line("CAM-01", "Orange", "Elevator near the platform", "Camera CAM-01", 40), 3000);
        CHECK(e["peakRaised"] == true && e["condition"] == "Orange alert");
        rec.note_facts(facts_line("CAM-01", false, false, 3.0, 430.0), 3900);
        e = rec.handle(alarm_line("CAM-01", "Yellow", "", "", 90), 4000);
        CHECK(e["closed"]["alertId"] == first && e["closed"]["cycle"] == "Y>O>Y");
        second = e["alertId"];
        CHECK(second != first);

        auto rows = store.list_raised_alerts(10);
        const auto* r1 = find_row(rows, first);
        CHECK(r1 && (*r1)["condition"] == "Orange alert" && (*r1)["place"] == "CAM-01" && (*r1)["status"] == "Cleared" && (*r1)["phase"] == "Cleared");
        const auto meta = (*r1)["metadata"];
        CHECK(meta["kind"] == "alarm" && meta["camera"] == "CAM-01" && meta["peak"] == "Orange" && meta["cycle"] == "Y>O>Y" && meta["closed"] == true);
        CHECK(meta["states"].size() == 3);
        CHECK(meta["states"][0]["state"] == "Yellow" && meta["states"][0]["tsMs"] == 1000 && meta["states"][0]["facts"].is_null());
        CHECK(meta["states"][1]["state"] == "Orange" && meta["states"][1]["frame"] == 40 && meta["states"][1]["facts"]["elevator_near_platform"] == true &&
              meta["states"][1]["facts"]["link_tilt_angled"] == true && meta["states"][1]["facts"]["angle_deg"] == 31.0 &&
              meta["states"][1]["facts"]["derrickman_cx"] == 420.0 && meta["states"][1]["facts"]["w"] == 1000);
        CHECK(!meta["states"][1]["facts"].contains("angles") && !meta["states"][1]["facts"].contains("pct_no_roi") && meta["states"][1]["facts"].contains("pct") &&
              meta["states"][1]["facts"]["roi"].size() == 4);
        CHECK(meta["states"][2]["state"] == "Yellow" && meta["states"][2]["facts"]["link_tilt_angled"] == false && meta["states"][2]["facts"]["angle_deg"] == 3.0);
        // the next cycle's row starts at that same Yellow and keeps its own label
        const auto* r2 = find_row(rows, second);
        CHECK(r2 && (*r2)["condition"] == "Yellow monitoring" && (*r2)["metadata"]["states"].size() == 1 && (*r2)["metadata"]["cycle"] == "Y" &&
              (*r2)["metadata"]["closed"] == false && (*r2)["metadata"]["states"][0]["facts"]["angle_deg"] == 3.0);

        // critical cycle: Y>O>R>O>Y, the Red raises the peak once; the label stays "Red alert" after the close
        rec.handle(alarm_line("CAM-01", "Orange"), 5000);
        e = rec.handle(alarm_line("CAM-01", "Red"), 6000);
        CHECK(e["peakRaised"] == true && e["condition"] == "Red alert" && e["escalation"] == 2);
        e = rec.handle(alarm_line("CAM-01", "Orange"), 7000);
        CHECK(e["peakRaised"] == false && e["condition"] == "Orange alert");
        e = rec.handle(alarm_line("CAM-01", "Yellow"), 8000);
        CHECK(e["closed"]["critical"] == true && e["closed"]["cycle"] == "Y>O>R>O>Y");
        third = e["closed"]["alertId"];
        rows = store.list_raised_alerts(10);
        const auto* r3 = find_row(rows, third);
        CHECK(r3 && (*r3)["condition"] == "Red alert" && (*r3)["severity"] == "Critical" && (*r3)["metadata"]["peak"] == "Red" &&
              (*r3)["metadata"]["states"].size() == 5 && (*r3)["metadata"]["states"][2]["state"] == "Red");
        // no prose in any stored field
        const std::string all = rows.dump();
        for (const char* prose : {"Machine present", "Elevator near the platform", "Camera CAM-01", "Safety condition"})
            CHECK(all.find(prose) == std::string::npos);

        // facts of another camera are not mixed in
        rec.note_facts(facts_line("CAM-02", true, true, 50.0, 100.0), 8100);
        e = rec.handle(alarm_line("CAM-03", "Yellow"), 8200);
        CHECK(store.find_raised_alert(e["alertId"])["metadata"]["states"][0]["facts"].is_null());
        CHECK(store.find_raised_alert("nope").is_null());
    }
    {
        rigmind::EventStore reopened(db); // restart: metadata is still on the rows, and a stored summary is kept
        reopened.put_alert_summary(first, "Camera CAM-01 saw it. Check the area.", "slm", 9000);
    }
    {
        rigmind::EventStore reopened(db);
        const auto row = reopened.find_raised_alert(first);
        CHECK(row["metadata"]["cycle"] == "Y>O>Y" && row["summary"] == "Camera CAM-01 saw it. Check the area." && row["summarySource"] == "slm");
        CHECK(reopened.get_alert_metadata(first).has_value());
    }

    // unit: fact trimming and the deterministic list
    const auto trimmed = rigmind::trim_facts(facts_line("CAM-01", true, true, 28.4, 410.0));
    CHECK(!trimmed.contains("ev") && !trimmed.contains("cam") && !trimmed.contains("frames") && trimmed["machine_present"] == true);
    const std::string list = rigmind::facts_list(trimmed);
    CHECK(list.find("link tilt angled 28 degrees") != std::string::npos && list.find("elevator near platform") != std::string::npos);
    CHECK(rigmind::facts_list(nlohmann::json::object()).empty() && rigmind::facts_list(nullptr).empty());
    // PPE prompt: items not detected and the people count
    const std::string ppe = rigmind::describe_prompt({{"kind", "ppe"}, {"camera", "CAM-01"}, {"missing", {"vest", "gloves"}}, {"persons", 2}}, "Advisory", "PPE items not detected", "CAM-01", 1000);
    CHECK(ppe.find("did not detect these PPE items: vest, gloves") != std::string::npos && ppe.find("with 2 people in view") != std::string::npos && ppe.find("Camera CAM-01") != std::string::npos);
    // guard: the place is the camera id - naming the camera is enough, nothing is added twice
    const nlohmann::json ev = {{"camId", "CAM-01"}, {"place", "CAM-01"}};
    CHECK(rigmind::reject_reason("Camera CAM-01 saw the elevator near the platform.", ev).empty());
    CHECK(rigmind::with_context("The elevator came near the platform.", ev) == "Camera CAM-01: the elevator came near the platform.");
    std::filesystem::remove_all(dir);
}

int main() {
    test_alarm_episode_recorder();
    test_event_cycles();
    test_ppe_episode_recorder();
    test_alarm_events_migration();
    test_slm_guard();
    test_footage_retention();
    test_footage_images_and_migration();
    test_snapshot_plan();
    test_audit_trail();
    test_describe_source();
    test_alert_metadata();
    test_zoom_thresholds();
    test_simulate_detection_to_alerts();
    test_yellow_after_stable_frames();
    test_escalation_to_orange_and_red();
    test_machine_absent_resets_to_yellow_only_when_tilt_near_derrickman();
    test_bridge_round_trip();
    test_alerts_and_records();
    test_stream_commands();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("backend tests passed\n");
    return EXIT_SUCCESS;
}
