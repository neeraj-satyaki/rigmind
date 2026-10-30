// Unit tests for the per-camera parts of the detection pipeline (pipeline/multi_camera.h): two cameras must not influence each other.
// No GStreamer, no ONNX: builds in every configuration.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "pipeline/bytetrack.h"
#include "pipeline/multi_camera.h"
#include "pipeline/ppe_check.h"
#include "pipeline/stream_profile.h"

using namespace rigmind;

static int failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

namespace {

struct TestFrame {
    int n = 0;
};
using Slots = FrameSlots<TestFrame>;

std::shared_ptr<const TestFrame> frame(int n) {
    auto f = std::make_shared<TestFrame>();
    f->n = n;
    return f;
}

FrameFacts orange_facts() {
    FrameFacts f;
    f.machine_present = true;
    f.link_tilt_angled = true;
    f.elevator_near_platform = true;
    return f;
}

// ---- FrameSlots: one slot per camera, round robin ------------------------------------------------------------------
void slots_do_not_overwrite_each_other() {
    Slots slots;
    Slots::Cursor cur;
    CHECK(!slots.has_unseen(cur));
    CHECK(!slots.take(cur).frame);
    slots.put("CAM-01", frame(1));
    slots.put("CAM-01", frame(2));
    slots.put("CAM-01", frame(3)); // CAM-01 floods
    slots.put("CAM-02", frame(100)); // the old single slot would have lost this one only if CAM-01 pushed after it
    slots.put("CAM-01", frame(4));
    // Old behaviour: latest_ == CAM-01 frame 4, CAM-02 never served. Now both are served, each with its own newest frame.
    auto a = slots.take(cur);
    auto b = slots.take(cur);
    CHECK(a.frame && b.frame);
    CHECK(a.frame->n == 4 && b.frame->n == 100); // CAM-01 first (alphabetical start), then CAM-02
    CHECK(!slots.has_unseen(cur));
    CHECK(!slots.take(cur).frame);
    CHECK(slots.latest("CAM-02")->n == 100 && slots.latest("CAM-01")->n == 4);
    CHECK(slots.latest("")->n == 4); // newest of any camera
    CHECK(!slots.latest("CAM-99"));
    CHECK(slots.seq("CAM-01") > 0 && slots.seq("CAM-99") == 0);
}

void round_robin_gives_both_cameras_turns() {
    // A slow worker (one frame per iteration) with two cameras that both deliver a new frame every iteration.
    Slots slots;
    Slots::Cursor cur;
    int served[2] = {0, 0};
    int longest_wait[2] = {0, 0}, wait[2] = {0, 0};
    for (int i = 0; i < 200; ++i) {
        slots.put("CAM-01", frame(i));
        slots.put("CAM-02", frame(i)); // CAM-02 always pushes last: the old "latest_" slot would always hold CAM-02
        const auto t = slots.take(cur);
        CHECK(t.frame != nullptr);
        const int which = t.seq == slots.seq("CAM-01") ? 0 : 1;
        for (int c = 0; c < 2; ++c) {
            wait[c] = (c == which) ? 0 : wait[c] + 1;
            longest_wait[c] = std::max(longest_wait[c], wait[c]);
        }
        ++served[which];
    }
    CHECK(served[0] == 100 && served[1] == 100);
    CHECK(longest_wait[0] <= 1 && longest_wait[1] <= 1);
    // A worker that is faster than the input (two takes per round) serves both, no frame twice.
    Slots::Cursor fast;
    for (int i = 0; i < 20; ++i) {
        slots.put("CAM-01", frame(1000 + i));
        slots.put("CAM-02", frame(2000 + i));
        const auto x = slots.take(fast), y = slots.take(fast);
        CHECK(x.frame && y.frame && x.frame.get() != y.frame.get());
        CHECK(!slots.take(fast).frame);
    }
    // Each worker has its own cursor: the other model sees the same frames.
    Slots::Cursor other;
    CHECK(slots.has_unseen(other));
}

void three_cameras_in_turn() {
    Slots slots;
    Slots::Cursor cur;
    std::string order;
    for (int i = 0; i < 3; ++i) {
        slots.put("A", frame(i));
        slots.put("B", frame(i));
        slots.put("C", frame(i));
        for (int k = 0; k < 3; ++k) {
            const auto t = slots.take(cur);
            order += t.seq == slots.seq("A") ? 'A' : t.seq == slots.seq("B") ? 'B' : 'C';
        }
    }
    CHECK(order == "ABCABCABC");
}

// ---- DrawHistory: results keyed by (camera, PTS) ----------------------------------------------------------------------
DrawResult result(const char* model, const char* cam, std::int64_t key, const char* label) {
    DrawResult r;
    r.model = model;
    r.cam = cam;
    r.key = key;
    DrawItem it;
    it.label = label;
    r.items.push_back(it);
    return r;
}

void draw_results_keyed_by_camera() {
    DrawHistory h;
    const auto t0 = DrawHistory::Clock::now();
    const std::int64_t pts = 5000000000LL; // both cameras' RTSP clocks happen to give the same PTS
    h.arrived("CAM-01", pts, t0);
    h.arrived("CAM-02", pts, t0);
    h.publish(result("idss", "CAM-01", pts, "platform-of-01"));
    CHECK(h.exact(1, "CAM-01", pts));
    CHECK(!h.exact(1, "CAM-02", pts)); // not drawn on the other camera's frame
    CHECK(h.pick(1, "CAM-02", pts, 2) == nullptr);
    h.publish(result("idss", "CAM-02", pts, "platform-of-02"));
    const DrawResult* a = h.pick(1, "CAM-01", pts, 2);
    const DrawResult* b = h.pick(1, "CAM-02", pts, 2);
    CHECK(a && b && a->items[0].label == "platform-of-01" && b->items[0].label == "platform-of-02");
    CHECK(h.pick(0, "CAM-01", pts, 2) == nullptr); // other model has nothing yet
    CHECK(h.arrival("CAM-01", pts).has_value() && !h.arrival("CAM-03", pts).has_value());

    // History depth is per camera and model: a chatty camera does not evict the other one's results.
    for (int i = 1; i <= 20; ++i) h.publish(result("ppe", "CAM-01", pts + i * 80000000LL, "x"));
    h.publish(result("ppe", "CAM-02", pts, "kept"));
    for (int i = 1; i <= 20; ++i) h.publish(result("ppe", "CAM-01", pts + (20 + i) * 80000000LL, "x"));
    CHECK(h.exact(0, "CAM-02", pts));
    CHECK(!h.exact(0, "CAM-01", pts + 80000000LL)); // CAM-01's own old ones are trimmed (8 kept)
    CHECK(h.exact(0, "CAM-01", pts + 40 * 80000000LL));

    // Stale fallback: the newest earlier result is used only for the same camera and only within 2 frames / 400 ms.
    DrawHistory s;
    for (int i = 0; i < 5; ++i) {
        s.arrived("A", 1000000000LL + i * 80000000LL, t0);
        s.arrived("B", 1000000000LL + i * 80000000LL, t0);
    }
    s.publish(result("ppe", "A", 1000000000LL, "a0"));
    s.publish(result("ppe", "B", 1000000000LL + 3 * 80000000LL, "b3"));
    const DrawResult* p = s.pick(0, "A", 1000000000LL + 2 * 80000000LL, 2);
    CHECK(p && p->items[0].label == "a0"); // 2 frames behind: allowed
    CHECK(s.pick(0, "A", 1000000000LL + 4 * 80000000LL, 2) == nullptr); // 4 frames behind: nothing is better than a lagging box
    CHECK(s.pick(0, "B", 1000000000LL + 2 * 80000000LL, 2) == nullptr); // B's result is from the future of this frame
    CHECK(s.newest(0, "A")->items[0].label == "a0" && s.newest(0, "B")->items[0].label == "b3" && s.newest(0, "C") == nullptr);
}

// ---- CameraFactsBoard + alarm state: camera A's facts never drive camera B's alarm -------------------------------------
void facts_board_per_camera() {
    CameraFactsBoard board;
    board.on_derived("CAM-01", orange_facts(), true);
    CHECK(board.get("CAM-01").bits == facts_to_bits(orange_facts()) && board.get("CAM-01").zoom);
    CHECK(board.get("CAM-02").bits == 0 && !board.get("CAM-02").zoom);
    board.on_derived("CAM-02", FrameFacts{}, false);
    CHECK(board.get("CAM-01").bits != 0 && board.get("CAM-02").bits == 0);
    CHECK(facts_from_bits(board.get("CAM-01").bits).link_tilt_angled);
    // Manual mode for one camera only.
    board.set_auto("CAM-01", false);
    board.set_manual("CAM-01", 1u);
    board.on_derived("CAM-01", FrameFacts{}, false); // ignored: manual
    CHECK(board.get("CAM-01").bits == 1u);
    board.on_derived("CAM-02", orange_facts(), false); // the other camera still follows the detections
    CHECK(board.get("CAM-02").bits == facts_to_bits(orange_facts()));
    board.set_auto("CAM-01", true);
    board.on_derived("CAM-01", FrameFacts{}, false);
    CHECK(board.get("CAM-01").bits == 0);
    // No camera id = every camera, and the start value of cameras that appear later.
    board.set_auto("", false);
    board.set_manual("", 4u);
    CHECK(board.get("CAM-01").bits == 4u && board.get("CAM-02").bits == 4u && board.get("CAM-07").bits == 4u);
    board.on_derived("CAM-07", orange_facts(), true);
    CHECK(board.get("CAM-07").bits == 4u);
}

void only_the_camera_with_the_facts_reaches_orange() {
    // CAM-01 sees the dangerous scene, CAM-02 an idle platform. One alarm machine per camera, each fed through the board.
    CameraFactsBoard board;
    AlarmStateMachine m1, m2;
    bool orange1 = false, orange2 = false, any2 = false;
    for (int frame_no = 0; frame_no < 60; ++frame_no) {
        board.on_derived("CAM-01", orange_facts(), false); // IDSS worker
        board.on_derived("CAM-02", FrameFacts{}, false);
        orange1 |= m1.step(facts_from_bits(board.get("CAM-01").bits)) == AlarmState::Orange; // the camera's probe
        const AlarmState s2 = m2.step(facts_from_bits(board.get("CAM-02").bits));
        orange2 |= s2 == AlarmState::Orange;
        any2 |= s2 != AlarmState::None;
    }
    CHECK(orange1);
    CHECK(!orange2 && !any2);
    CHECK(m1.current() == AlarmState::Orange && m2.current() == AlarmState::None);

    // Same, with the two IDSS "workers" and the two probes running concurrently on separate threads.
    CameraFactsBoard shared;
    std::atomic<bool> go{true};
    std::atomic<int> orange_a{0}, orange_b{0}, nonidle_b{0};
    auto producer = [&](const char* cam, FrameFacts facts) {
        while (go.load()) {
            shared.on_derived(cam, facts, false);
            std::this_thread::yield();
        }
    };
    auto consumer = [&](const char* cam, std::atomic<int>& orange, std::atomic<int>* nonidle) {
        AlarmStateMachine m;
        for (int i = 0; i < 400; ++i) {
            const AlarmState s = m.step(facts_from_bits(shared.get(cam).bits));
            if (s == AlarmState::Orange) ++orange;
            if (nonidle && s != AlarmState::None) ++*nonidle;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };
    std::thread pa(producer, "CAM-01", orange_facts()), pb(producer, "CAM-02", FrameFacts{});
    std::thread ca(consumer, "CAM-01", std::ref(orange_a), nullptr), cb(consumer, "CAM-02", std::ref(orange_b), &nonidle_b);
    ca.join();
    cb.join();
    go = false;
    pa.join();
    pb.join();
    CHECK(orange_a.load() > 0);
    CHECK(orange_b.load() == 0 && nonidle_b.load() == 0);
}

// ---- Identical streams: identical per-camera output, equal to a single-camera run ------------------------------------------
struct CamPipeline {
    ByteTracker tracker;
    PpeChecker ppe;
    AlarmStateMachine alarm;
    std::vector<int> ids;                 // every track id output, in order
    std::vector<std::string> ppe_events;  // "<ms>:<state>"
    std::vector<int> alarm_states;
    CamPipeline() { ppe.configure({}); }
};

NamedBox nb(const char* label, float x, float y) { return {label, 0.9f, {x, y, x + 60, y + 80}, {}}; }

void run_frame(CamPipeline& p, int i, bool vest_missing, bool danger) {
    const std::int64_t now = 1000 + i * 100;
    TrackConfig tc;
    const float x = 100.0f + 2 * i;
    std::vector<TrackDet> dets = {{0, "person", 0.9f, {x, 100, x + 100, 300}}, {1, "person", 0.9f, {600, 100, 700, 300}}};
    std::vector<PpePerson> people;
    for (const TrackOut& o : p.tracker.update(now, dets, tc)) {
        p.ids.push_back(o.id);
        people.push_back({o.id, o.box});
    }
    std::vector<NamedBox> items = {nb("helmet", x + 20, 110), nb("gloves", x + 20, 250), nb("helmet", 620, 110), nb("gloves", 620, 250)};
    if (!vest_missing) {
        items.push_back(nb("vest", x + 20, 180));
        items.push_back(nb("vest", 620, 180));
    }
    if (auto e = p.ppe.update(now, people, items)) p.ppe_events.push_back(std::to_string(now) + ":" + e->state);
    FrameFacts f;
    if (danger) f = orange_facts();
    p.alarm_states.push_back(static_cast<int>(p.alarm.step(f)));
}

void identical_streams_identical_output() {
    const int frames = 150;
    CamPipeline single;
    for (int i = 0; i < frames; ++i) run_frame(single, i, i >= 20, i >= 30);
    PerCamera<CamPipeline> cams;
    for (int i = 0; i < frames; ++i) { // frames of the two cameras interleaved, as the workers see them
        run_frame(cams.get("CAM-01"), i, i >= 20, i >= 30);
        run_frame(cams.get("CAM-02"), i, i >= 20, i >= 30);
    }
    const CamPipeline& a = cams.get("CAM-01");
    const CamPipeline& b = cams.get("CAM-02");
    CHECK(cams.size() == 2);
    CHECK(a.ids == b.ids && a.ids == single.ids);
    CHECK(a.ppe_events == b.ppe_events && a.ppe_events == single.ppe_events);
    CHECK(a.alarm_states == b.alarm_states && a.alarm_states == single.alarm_states);
    CHECK(!single.ppe_events.empty() && single.ppe_events[0].find("missing") != std::string::npos);
    CHECK(single.alarm_states.back() == static_cast<int>(AlarmState::Orange));
}

void different_streams_do_not_leak() {
    // CAM-01: vest missing and a dangerous scene; CAM-02: everything fine. Nothing of CAM-01 may show on CAM-02.
    PerCamera<CamPipeline> cams;
    for (int i = 0; i < 150; ++i) {
        run_frame(cams.get("CAM-01"), i, true, true);
        run_frame(cams.get("CAM-02"), i, false, false);
    }
    CamPipeline& a = cams.get("CAM-01");
    CamPipeline& b = cams.get("CAM-02");
    CHECK(!a.ppe_events.empty() && a.ppe_events[0].find("missing") != std::string::npos);
    CHECK(b.ppe_events.empty());
    CHECK(a.alarm_states.back() == static_cast<int>(AlarmState::Orange));
    bool b_idle = true;
    for (int s : b.alarm_states) b_idle &= s == static_cast<int>(AlarmState::None);
    CHECK(b_idle);
    // The same track ids are given independently (ids are per camera, both start at 1).
    CHECK(!a.ids.empty() && !b.ids.empty() && a.ids.front() == b.ids.front());
}

void stream_profiles() {
    const StreamProfile s = stream_profile("small"), f = stream_profile("full");
    CHECK(s.width == 640 && s.height == 360 && s.kbps == 1200);
    CHECK(f.width == 1280 && f.height == 720 && f.kbps == 4000);
    const StreamProfile d = stream_profile("");
    const StreamProfile x = stream_profile("bogus");
    CHECK(d.width == 1280 && x.width == 1280 && std::string(x.name) == "full");
    CHECK(encoder_bitrate_value(s, "mfh264enc") == 1200u && encoder_bitrate_value(f, "nvh264enc") == 4000u);
    CHECK(encoder_bitrate_value(s, "openh264enc") == 1200000u && encoder_bitrate_value(f, "openh264enc") == 4000000u);
}

} // namespace

int main() {
    stream_profiles();
    slots_do_not_overwrite_each_other();
    round_robin_gives_both_cameras_turns();
    three_cameras_in_turn();
    draw_results_keyed_by_camera();
    facts_board_per_camera();
    only_the_camera_with_the_facts_reaches_orange();
    identical_streams_identical_output();
    different_streams_do_not_leak();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("multi_camera_test: all checks passed\n");
    return 0;
}
