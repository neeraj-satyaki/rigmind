// Backend unit tests. Plain asserts so they build with MSVC and g++ alike, no WebView2 needed.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/alarm_state.h"
#include "backend/audit_log.h"
#include "backend/bridge.h"
#include "slm/slm_guard.h"
#include "backend/event_store.h"

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
    CHECK(alerts["result"][0]["status"] == "Acknowledged");
    CHECK(alerts["result"][0]["owner"] == "Faisal A.");

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

static void test_slm_guard() {
    CHECK(rigmind::first_sentence("1: 28 Oct 2026 21:28, camera cam-03 recorded: Elevator. Extra.") ==
          "28 Oct 2026 21:28, camera cam-03 recorded: Elevator.");
    CHECK(rigmind::first_sentence(": At 28 Oct, camera CAM-05 recorded: Tilt.") == "At 28 Oct, camera CAM-05 recorded: Tilt.");
    CHECK(rigmind::first_sentence("28 Oct 2026 21:28 is the date.") == "28 Oct 2026 21:28 is the date.");
    CHECK(rigmind::first_sentence("") == "");

    const nlohmann::json event = {{"camId", "CAM-03"}, {"place", "Demo rig 02 \xC2\xB7 Drill floor"}};
    CHECK(rigmind::reject_reason("At 28 Oct, camera cam-03 at Demo rig 02 recorded a fault.", event).empty());
    CHECK(rigmind::reject_reason("At 28 Oct, camera cam-03 at Drill floor recorded a fault.", event).empty());
    CHECK(!rigmind::reject_reason("At 28 Oct, camera cam-09 at Drill floor recorded a fault.", event).empty());
    CHECK(!rigmind::reject_reason("## At 28 Oct, camera cam-03 at Drill floor recorded a fault.", event).empty());
    CHECK(!rigmind::reject_reason("Short.", event).empty());
    CHECK(!rigmind::reject_reason(std::string(300, 'x'), event).empty());
}

int main() {
    test_slm_guard();
    test_footage_retention();
    test_audit_trail();
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
