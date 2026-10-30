// Per-camera building blocks of the detection pipeline (header-only, no GStreamer, no ONNX: unit tested in multi_camera_test).
//
// With two or more cameras nothing may be shared between them except the models: this file holds
//  - FrameSlots     the newest frame of EACH camera plus a round-robin cursor per model worker (no camera starves another),
//  - DrawHistory    the overlay results per model AND camera, found by (camera, buffer PTS): two cameras whose PTS coincide
//                   never draw each other's detections,
//  - CameraFactsBoard  the EWS facts / zoom flag per camera (the alarm element of camera A reads only camera A's facts),
//  - PerCamera<T>   a map camera id -> state, created on first use.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "backend/alarm_state.h"
#include "pipeline/draw_types.h"

namespace rigmind {

// ---- facts <-> bits -------------------------------------------------------------------------------------------------
struct FactField {
    const char* name;
    bool FrameFacts::*member;
};
inline const FactField* fact_fields(std::size_t& count) {
    static const FactField fields[] = {
        {"machine_present", &FrameFacts::machine_present},
        {"link_tilt_present", &FrameFacts::link_tilt_present},
        {"link_tilt_angled", &FrameFacts::link_tilt_angled},
        {"elevator_present", &FrameFacts::elevator_present},
        {"elevator_near_platform", &FrameFacts::elevator_near_platform},
        {"platform_elevator_collision", &FrameFacts::platform_elevator_collision},
        {"derrickman_out_of_safe_zone", &FrameFacts::derrickman_out_of_safe_zone},
        {"machine_past_right_edge", &FrameFacts::machine_past_right_edge},
        {"link_tilt_near_derrickman", &FrameFacts::link_tilt_near_derrickman},
    };
    count = sizeof(fields) / sizeof(fields[0]);
    return fields;
}
inline FrameFacts facts_from_bits(std::uint32_t bits) {
    std::size_t n = 0;
    const FactField* f = fact_fields(n);
    FrameFacts facts;
    for (std::size_t i = 0; i < n; ++i) facts.*(f[i].member) = (bits >> i) & 1u;
    return facts;
}
inline std::uint32_t facts_to_bits(const FrameFacts& facts) {
    std::size_t n = 0;
    const FactField* f = fact_fields(n);
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (facts.*(f[i].member)) bits |= 1u << i;
    return bits;
}

// ---- PerCamera ------------------------------------------------------------------------------------------------------
template <class T>
class PerCamera {
public:
    T& get(const std::string& cam) { return items_[cam]; } // created on first use
    template <class F>
    void for_each(F&& fn) {
        for (auto& [cam, item] : items_) fn(cam, item);
    }
    std::size_t size() const { return items_.size(); }

private:
    std::map<std::string, T> items_;
};

// ---- FrameSlots -----------------------------------------------------------------------------------------------------
// One slot per camera holding only its newest frame (an older one is overwritten: models skip frames, never queue). Each model
// worker owns a Cursor and takes the next camera after the one it served last that has a frame it has not seen yet: when the
// worker is slower than the combined input, every camera still gets turns (round robin) instead of the camera that pushed last
// winning. Not thread safe: the owner locks.
template <class FrameT>
class FrameSlots {
public:
    struct Cursor {
        std::map<std::string, std::uint64_t> seen; // per camera: sequence of the last frame this worker took
        std::string last;                          // camera served last
    };
    struct Taken {
        std::shared_ptr<const FrameT> frame;
        std::uint64_t seq = 0;
    };

    void put(const std::string& cam, std::shared_ptr<const FrameT> frame) {
        Slot& s = slots_[cam];
        s.frame = std::move(frame);
        s.seq = ++counter_;
        newest_cam_ = cam;
    }
    bool has_unseen(const Cursor& c) const {
        for (const auto& [cam, s] : slots_)
            if (s.seq != seen_of(c, cam)) return true;
        return false;
    }
    // The newest frame of the next camera (round robin from the one served last) with something new; a null frame when none.
    Taken take(Cursor& c) const {
        if (slots_.empty()) return {};
        auto start = slots_.upper_bound(c.last);
        for (std::size_t n = 0; n < slots_.size(); ++n) {
            if (start == slots_.end()) start = slots_.begin();
            if (start->second.seq != seen_of(c, start->first)) {
                c.seen[start->first] = start->second.seq;
                c.last = start->first;
                return {start->second.frame, start->second.seq};
            }
            ++start;
        }
        return {};
    }
    std::uint64_t seq(const std::string& cam) const {
        const auto it = slots_.find(cam);
        return it == slots_.end() ? 0 : it->second.seq;
    }
    // Newest frame of one camera; an empty id gives the newest frame of any camera.
    std::shared_ptr<const FrameT> latest(const std::string& cam) const {
        const auto it = slots_.find(cam.empty() ? newest_cam_ : cam);
        return it == slots_.end() ? nullptr : it->second.frame;
    }

private:
    struct Slot {
        std::shared_ptr<const FrameT> frame;
        std::uint64_t seq = 0;
    };
    static std::uint64_t seen_of(const Cursor& c, const std::string& cam) {
        const auto it = c.seen.find(cam);
        return it == c.seen.end() ? 0 : it->second;
    }
    std::map<std::string, Slot> slots_;
    std::uint64_t counter_ = 0;
    std::string newest_cam_;
};

// ---- DrawHistory ----------------------------------------------------------------------------------------------------
// Overlay results for the burn-in. model index 0 = ppe, 1 = idss. Everything is keyed by (camera, buffer PTS). Not thread safe.
class DrawHistory {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kKeepResults = 8;   // per model and camera
    static constexpr std::size_t kKeepArrivals = 32; // per camera
    static constexpr std::int64_t kMaxAgeNs = 400000000LL;

    static int model_index(const std::string& model) { return model == "idss" ? 1 : 0; }

    void arrived(const std::string& cam, std::int64_t key, Clock::time_point when) {
        auto& list = arrivals_[cam];
        list.emplace_back(key, when);
        if (list.size() > kKeepArrivals) list.erase(list.begin());
    }
    std::optional<Clock::time_point> arrival(const std::string& cam, std::int64_t key) const {
        const auto it = arrivals_.find(cam);
        if (it == arrivals_.end()) return std::nullopt;
        std::optional<Clock::time_point> out;
        for (const auto& a : it->second)
            if (a.first == key) out = a.second;
        return out;
    }
    // Position of a frame in the camera's recent arrivals: frame distance between two keys. -1 unknown.
    int index_of(const std::string& cam, std::int64_t key) const {
        const auto it = arrivals_.find(cam);
        if (it == arrivals_.end()) return -1;
        for (std::size_t i = 0; i < it->second.size(); ++i)
            if (it->second[i].first == key) return static_cast<int>(i);
        return -1;
    }
    void publish(DrawResult result) {
        auto& history = results_[model_index(result.model)][result.cam];
        history.push_back(std::move(result));
        if (history.size() > kKeepResults) history.erase(history.begin());
    }
    bool exact(int model, const std::string& cam, std::int64_t key) const {
        const auto* h = history(model, cam);
        if (!h) return false;
        for (const DrawResult& r : *h)
            if (r.key == key) return true;
        return false;
    }
    // The result to draw on frame (cam, key): its own, else the newest earlier one that is under 400 ms and `max_stale` frames old.
    const DrawResult* pick(int model, const std::string& cam, std::int64_t key, int max_stale) const {
        const auto* h = history(model, cam);
        if (!h) return nullptr;
        const int cur_index = index_of(cam, key);
        const DrawResult* pick = nullptr;
        for (const DrawResult& r : *h) {
            if (r.key == key) return &r;
            if (r.key < key && key - r.key < kMaxAgeNs) {
                const int idx = index_of(cam, r.key);
                const int lag = (cur_index >= 0 && idx >= 0) ? cur_index - idx : 99;
                if (lag <= max_stale) pick = &r;
            }
        }
        return pick;
    }
    const DrawResult* newest(int model, const std::string& cam) const {
        const auto* h = history(model, cam);
        return h && !h->empty() ? &h->back() : nullptr;
    }

private:
    const std::vector<DrawResult>* history(int model, const std::string& cam) const {
        const auto it = results_[model].find(cam);
        return it == results_[model].end() ? nullptr : &it->second;
    }
    std::map<std::string, std::vector<DrawResult>> results_[2];
    std::map<std::string, std::vector<std::pair<std::int64_t, Clock::time_point>>> arrivals_;
};

// ---- CameraFactsBoard -----------------------------------------------------------------------------------------------
// The facts and the zoom flag the EWS alarm element of each camera is fed with. Thread safe (IDSS worker writes, one GStreamer
// streaming thread per camera reads). "auto" (facts follow the detections) is per camera; a manual write with an empty camera id
// applies to every camera and becomes the starting value of cameras that appear later.
class CameraFactsBoard {
public:
    struct Entry {
        std::uint32_t bits = 0;
        bool zoom = false;
        bool automatic = true;
    };

    // From the IDSS worker: ignored for a camera in manual mode.
    void on_derived(const std::string& cam, const FrameFacts& facts, bool zoomed) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = entry(cam);
        if (!e.automatic) return;
        e.zoom = zoomed;
        e.bits = facts_to_bits(facts);
    }
    Entry get(const std::string& cam) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(cam);
        return it == entries_.end() ? default_ : it->second;
    }
    void set_manual(const std::string& cam, std::uint32_t bits) {
        std::lock_guard<std::mutex> lock(mutex_);
        apply(cam, [&](Entry& e) { e.bits = bits; });
    }
    void set_auto(const std::string& cam, bool automatic) {
        std::lock_guard<std::mutex> lock(mutex_);
        apply(cam, [&](Entry& e) { e.automatic = automatic; });
    }

private:
    Entry& entry(const std::string& cam) {
        const auto it = entries_.find(cam);
        if (it != entries_.end()) return it->second;
        return entries_.emplace(cam, default_).first->second;
    }
    template <class F>
    void apply(const std::string& cam, F&& fn) {
        if (cam.empty()) {
            fn(default_);
            for (auto& [id, e] : entries_) fn(e);
        } else {
            fn(entry(cam));
        }
    }
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    Entry default_;
};

} // namespace rigmind
