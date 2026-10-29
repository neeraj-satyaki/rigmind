// Unit tests for ByteTrack (pipeline/bytetrack.h) and the PPE compliance check (pipeline/ppe_check.h).
// No GStreamer, no ONNX: builds in every configuration.
#include <cstdio>
#include <cstdlib>

#include "pipeline/bytetrack.h"
#include "pipeline/ppe_check.h"

using namespace rigmind;

static int failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static TrackDet td(int cls, float conf, float x, float y, float w = 60, float h = 80) {
    return {cls, cls == 0 ? "helmet" : "gloves", conf, {x, y, x + w, y + h}};
}

static void kalman() {
    KalmanBox k;
    k.init({100, 100, 160, 200});
    Box b = k.box();
    CHECK(std::fabs(b.x1 - 100) < 0.5 && std::fabs(b.y2 - 200) < 0.5);
    // A box moving 10 px per frame: after a few updates the filter has learned the velocity and predicts ahead.
    for (int i = 1; i <= 12; ++i) {
        k.predict();
        k.update({100.0f + 10 * i, 100, 160.0f + 10 * i, 200});
    }
    CHECK(std::fabs(k.x[4] - 10.0) < 1.5);
    const float before = k.box().cx();
    k.predict();
    CHECK(k.box().cx() > before + 5);
    // An update pulls the state towards the measurement without jumping all the way (smoothing).
    KalmanBox m;
    m.init({0, 0, 50, 100});
    m.predict();
    m.update({10, 0, 60, 100});
    CHECK(m.box().cx() > 30 && m.box().cx() < 35.5);
}

static void hungarian_cases() {
    const double c[3][3] = {{4, 1, 3}, {2, 0, 5}, {3, 2, 2}};
    auto a = hungarian({{4, 1, 3}, {2, 0, 5}, {3, 2, 2}});
    double total = 0;
    for (int i = 0; i < 3; ++i) total += c[i][a[static_cast<std::size_t>(i)]];
    CHECK(total == 5); // optimum
    // Greedy would take the 0 at (0,1) first and pay 100; the optimum pairs 0->1? no: (0,1)=0 and (1,0)=0 is greedy-free.
    auto g = hungarian({{1, 0}, {0, 100}});
    CHECK(g[0] == 1 && g[1] == 0);
    auto g2 = hungarian({{0, 1}, {1, 100}});
    CHECK(g2[0] == 1 && g2[1] == 0); // greedy picks (0,0)=0 then pays 100
    auto wide = hungarian({{9, 2, 7}, {3, 6, 1}});
    CHECK(wide[0] == 1 && wide[1] == 2);
    auto tall = hungarian({{9, 2}, {3, 6}, {1, 8}});
    int assigned = 0;
    for (int v : tall) assigned += v >= 0;
    CHECK(assigned == 2 && tall[0] == 1 && tall[2] == 0 && tall[1] == -1);
    CHECK(hungarian({}).empty());
}

static void flicker_keeps_one_id() {
    ByteTracker t;
    TrackConfig cfg;
    std::int64_t now = 1000;
    auto step = [&](std::vector<TrackDet> d) { now += 100; return t.update(now, d, cfg); };
    auto o = step({td(0, 0.6f, 100, 100)});
    CHECK(o.size() == 1 && !o[0].held); // confidence >= high_thresh: shown at once
    const int id = o[0].id;
    o = step({td(0, 0.6f, 101, 100)});
    CHECK(o.size() == 1 && o[0].id == id && !o[0].held);
    o = step({});
    CHECK(o.size() == 1 && o[0].id == id && o[0].held);
    o = step({td(0, 0.6f, 102, 100)});
    CHECK(o.size() == 1 && o[0].id == id && !o[0].held);
    for (int i = 0; i < 2; ++i) {
        o = step({});
        CHECK(o.size() == 1 && o[0].id == id && o[0].held);
    }
    o = step({td(0, 0.6f, 103, 100)});
    CHECK(o.size() == 1 && o[0].id == id && t.ids_issued() == 1);
}

static void id_kept_when_crossing() {
    ByteTracker t;
    TrackConfig cfg;
    std::int64_t now = 0;
    int id_top = 0, id_bottom = 0;
    bool swapped = false;
    for (int f = 0; f <= 40; ++f) {
        now += 100;
        const float ax = 100 + 10 * f, bx = 500 - 10 * f; // A moves right, B moves left, they overlap around f = 20
        auto o = t.update(now, {td(0, 0.8f, ax, 100), td(0, 0.8f, bx, 130)}, cfg);
        CHECK(o.size() == 2);
        for (const auto& r : o) {
            int& slot = r.box.cy() < 150 ? id_top : id_bottom;
            if (slot == 0) slot = r.id;
            else if (slot != r.id) swapped = true;
        }
    }
    CHECK(id_top != id_bottom && !swapped && t.ids_issued() == 2);
}

static void low_score_recovery() {
    ByteTracker t;
    TrackConfig cfg;
    std::int64_t now = 0;
    auto step = [&](std::vector<TrackDet> d) { now += 100; return t.update(now, d, cfg); };
    step({td(0, 0.7f, 100, 100)});
    auto o = step({td(0, 0.7f, 100, 100)});
    const int id = o[0].id;
    o = step({td(0, 0.2f, 101, 100)}); // low score: below high_thresh, above low_thresh
    CHECK(o.size() == 1 && o[0].id == id && !o[0].held); // matched in stage 2, not merely held
    o = step({td(0, 0.1f, 101, 100)}); // below low_thresh: ignored
    CHECK(o.size() == 1 && o[0].held);
}

static void buffer_expiry() {
    ByteTracker t;
    TrackConfig cfg; // buffer 1 s
    std::int64_t now = 100;
    t.update(now, {td(0, 0.8f, 100, 100)}, cfg);
    int shown = 0;
    for (int i = 0; i < 14; ++i) {
        now += 100;
        shown += static_cast<int>(t.update(now, {}, cfg).size());
    }
    CHECK(shown == 10); // held for the 1 s buffer (10 frames at 100 ms), then removed
    now += 100;
    auto o = t.update(now, {td(0, 0.8f, 100, 100)}, cfg);
    CHECK(o.size() == 1 && o[0].id == 2); // the old id is gone
}

static void new_track_gating() {
    ByteTracker t;
    TrackConfig cfg;
    std::int64_t now = 0;
    auto step = [&](std::vector<TrackDet> d) { now += 100; return t.update(now, d, cfg); };
    CHECK(step({td(0, 0.2f, 100, 100)}).empty()); // above low_thresh but below new_track_thresh: no track
    CHECK(step({td(0, 0.2f, 100, 100)}).empty());
    CHECK(t.ids_issued() == 0);
    CHECK(step({td(0, 0.3f, 100, 100)}).empty()); // tentative: needs a second hit
    auto o = step({td(0, 0.3f, 100, 100)});
    CHECK(o.size() == 1 && !o[0].held);
    ByteTracker u; // a tentative track that misses a frame is dropped
    u.update(100, {td(0, 0.3f, 100, 100)}, cfg);
    u.update(200, {}, cfg);
    CHECK(u.update(300, {td(0, 0.3f, 100, 100)}, cfg).empty());
    cfg.enabled = false; // disabled: a plain filter at new_track_thresh
    ByteTracker d;
    CHECK(d.update(100, {td(0, 0.3f, 1, 1), td(0, 0.2f, 200, 1)}, cfg).size() == 1);
}

static void per_class_separation() {
    ByteTracker t;
    TrackConfig cfg;
    auto o = t.update(100, {td(0, 0.8f, 100, 100), td(1, 0.8f, 100, 100)}, cfg); // same place, different class
    CHECK(o.size() == 2 && o[0].id != o[1].id);
    int helmet = 0, gloves = 0;
    for (const auto& r : o) (r.cls == 0 ? helmet : gloves) = r.id;
    o = t.update(200, {td(1, 0.8f, 102, 100), td(0, 0.8f, 102, 100)}, cfg);
    for (const auto& r : o) CHECK(r.id == (r.cls == 0 ? helmet : gloves));
    ByteTracker s; // a track never takes a detection of another class
    s.update(100, {td(0, 0.8f, 100, 100)}, cfg);
    auto q = s.update(200, {td(1, 0.8f, 100, 100)}, cfg);
    CHECK(q.size() == 2); // the held helmet and a new gloves track
}

// ---- PPE check ---------------------------------------------------------------------------------------------------
static NamedBox item(const char* label, float cx, float cy) { return {label, 0.8f, {cx - 10, cy - 10, cx + 10, cy + 10}, {}}; }
static PpePerson person(int id, float x1, float y1, float x2, float y2) { return {id, {x1, y1, x2, y2}}; }

static void ppe_presence_and_padding() {
    PpeChecker c;
    c.configure({});
    const PpePerson p = person(1, 100, 100, 200, 300);
    CHECK(c.evaluate({p}, {}).size() == 3);
    CHECK(c.evaluate({p}, {item("helmet", 150, 110), item("Vest", 150, 200), item("Gloves", 110, 250)}).empty());
    CHECK(c.evaluate({p}, {item("HARDHAT", 150, 110), item("safety_vest", 150, 200), item("glove", 110, 250)}).empty());
    CHECK(c.evaluate({p}, {item("hi-vis", 150, 200), item("helmet", 150, 110), item("gloves", 150, 250)}).empty());
    // centroid outside the box but inside the 15 % pad (15 x 30): present; outside the pad: missing
    CHECK(c.evaluate({p}, {item("helmet", 90, 110), item("vest", 150, 200), item("gloves", 150, 250)}).empty());
    auto m = c.evaluate({p}, {item("helmet", 70, 110), item("vest", 150, 200), item("gloves", 150, 250)});
    CHECK(m.size() == 1 && m[0] == "helmet");
    m = c.evaluate({p}, {item("helmet", 150, 110)});
    CHECK(m.size() == 2 && m[0] == "vest" && m[1] == "gloves"); // required order
    // several persons: missing when ANY person lacks it (the other person's vest does not help)
    const PpePerson q = person(2, 400, 100, 500, 300);
    m = c.evaluate({p, q}, {item("helmet", 150, 110), item("helmet", 450, 110), item("vest", 150, 200), item("gloves", 150, 250), item("gloves", 450, 250)});
    CHECK(m.size() == 1 && m[0] == "vest");
}

static void ppe_debounce() {
    PpeChecker c;
    c.configure({}); // 5 s to raise, 3 s to clear
    const std::vector<PpePerson> one = {person(1, 100, 100, 200, 300)};
    const std::vector<NamedBox> partial = {item("helmet", 150, 110), item("gloves", 150, 250)}; // vest missing
    const std::vector<NamedBox> all = {item("helmet", 150, 110), item("gloves", 150, 250), item("vest", 150, 200)};
    std::int64_t now = 1000;
    int events = 0;
    PpeEvent last;
    for (int i = 0; i < 49; ++i, now += 100) { // 4.8 s missing: not yet
        if (auto e = c.update(now, one, partial)) { ++events; last = *e; }
    }
    CHECK(events == 0);
    if (auto e = c.update(now, one, partial)) { ++events; last = *e; } // 4.9 s after the first frame
    now += 100;
    if (auto e = c.update(now, one, partial)) { ++events; last = *e; } // 5.0 s
    CHECK(events == 1 && last.state == "missing" && last.missing.size() == 1 && last.missing[0] == "vest" && last.persons == 1);
    for (int i = 0; i < 100; ++i) { now += 100; CHECK(!c.update(now, one, partial)); } // no repeat
    // present for less than the clear time, then missing again: stays raised
    for (int i = 0; i < 20; ++i) { now += 100; CHECK(!c.update(now, one, all)); }
    for (int i = 0; i < 5; ++i) { now += 100; CHECK(!c.update(now, one, partial)); }
    // no person: state unchanged, never an event, however long
    for (int i = 0; i < 100; ++i) { now += 100; CHECK(!c.update(now, {}, all)); }
    CHECK(c.in_episode());
    events = 0; // present for the clear time: one ok event
    for (int i = 0; i < 40; ++i) { now += 100; if (auto e = c.update(now, one, all)) { ++events; last = *e; } }
    CHECK(events == 1 && last.state == "ok" && last.missing.empty() && !c.in_episode());
}

static void ppe_no_person_and_update() {
    PpeChecker c;
    c.configure({});
    std::int64_t now = 1000;
    for (int i = 0; i < 200; ++i, now += 100) CHECK(!c.update(now, {}, {}));
    CHECK(!c.in_episode());
    // vest missing first, gloves later: a second event with both, the episode start is kept
    const std::vector<PpePerson> one = {person(1, 100, 100, 200, 300)};
    std::vector<PpeEvent> events;
    for (int i = 0; i < 160; ++i, now += 100) {
        std::vector<NamedBox> items = {item("helmet", 150, 110), item("gloves", 150, 250)};
        if (i >= 60) items.pop_back();
        if (auto e = c.update(now, one, items)) events.push_back(*e);
    }
    CHECK(events.size() == 2);
    if (events.size() == 2) {
        CHECK(events[0].missing.size() == 1 && events[1].missing.size() == 2 && events[0].since_ms == events[1].since_ms);
    }
    PpeStatus s = c.take_status();
    CHECK(s.evals == 160 && s.frames == 360 && s.present_evals.at("helmet") == 160);
}

static void ppe_per_person_timers() {
    PpeChecker c;
    c.configure({});
    const PpePerson a = person(1, 100, 100, 200, 300), b = person(2, 400, 100, 500, 300);
    // person 2 never shows a vest; person 1 does. Person 2 appears at 3 s: its own timer starts then.
    const std::vector<NamedBox> items = {item("helmet", 150, 110), item("gloves", 150, 250), item("vest", 150, 200), item("helmet", 450, 110), item("gloves", 450, 250)};
    std::int64_t now = 1000;
    int events = 0;
    for (int i = 0; i < 100; ++i, now += 100) {
        std::vector<PpePerson> people = {a};
        if (i >= 30) people.push_back(b);
        if (auto e = c.update(now, people, items)) {
            ++events;
            CHECK(i >= 30 + 50 && e->missing.size() == 1 && e->missing[0] == "vest" && e->persons == 2);
        }
    }
    CHECK(events == 1);
}

static void ppe_id_churn_does_not_clear() {
    PpeChecker c;
    c.configure({});
    const std::vector<NamedBox> items = {item("helmet", 150, 110), item("gloves", 150, 250)}; // vest never detected
    std::int64_t now = 1000;
    int raised = 0, other = 0;
    // after 8 s the person track id changes every 2 s: the vest stays raised, no ok/missing flapping
    for (int i = 0; i < 400; ++i, now += 100) {
        const std::vector<PpePerson> one = {person(i < 80 ? 1 : 2 + (i - 80) / 20, 100, 100, 200, 300)};
        if (auto e = c.update(now, one, items)) { if (e->state == "missing") ++raised; else ++other; }
    }
    CHECK(raised == 1 && other == 0);
}

int main() {
    kalman();
    hungarian_cases();
    flicker_keeps_one_id();
    id_kept_when_crossing();
    low_score_recovery();
    buffer_expiry();
    new_track_gating();
    per_class_separation();
    ppe_presence_and_padding();
    ppe_debounce();
    ppe_no_person_and_update();
    ppe_per_person_timers();
    ppe_id_churn_does_not_clear();
    if (failures == 0) std::printf("tracking_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
