// Compliance check: is every person (IDSS class 'derrickman') wearing the REQUIRED PPE (helmet, vest, gloves by default)?
// Plain C++, no GStreamer or ONNX: unit tested alone.
//
// Wording is "not detected", never "not worn": the camera may not see the item.
//
// Per evaluation (one PPE frame with fresh person boxes): a required class is present for a person when a PPE item of that
// class has its centroid inside the person box padded by `person_box_pad`. No person visible: no evaluation, state unchanged
// (absence of people never raises and never clears).
// Debounce, per person id (a ByteTrack id; without tracking the index in the list): an item is "missing" for a person once
// it has been missing in every evaluation for `missing_ms`. The reported item is raised as soon as one person has it missing,
// and cleared only after every person in view has had it detected for `clear_ms`. A gap between evaluations over kMaxGapMs restarts the timers.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pipeline/ews_facts.h"

namespace rigmind {

struct PpeCheckConfig {
    bool enabled = true;
    std::vector<std::string> required = {"helmet", "vest", "gloves"};
    std::string person_class = "derrickman";
    float person_box_pad = 0.15f;
    int missing_ms = 5000;
    int clear_ms = 3000;
};

// Maps model labels (case varies, several spellings) to the canonical class name; unknown labels are lower-cased.
inline std::string canonical_ppe_class(const std::string& label) {
    std::string s;
    for (char c : label) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (char& c : s) if (c == ' ' || c == '_') c = '-';
    if (s == "vest" || s == "safety-vest" || s == "hi-vis" || s == "hivis" || s == "hi-vis-vest") return "vest";
    if (s == "glove" || s == "gloves") return "gloves";
    if (s == "helmet" || s == "hardhat" || s == "hard-hat") return "helmet";
    return s;
}

struct PpePerson {
    int id = 0;
    Box box;
};

struct PpeEvent {
    std::string state;                // "missing" | "ok"
    std::vector<std::string> missing; // raised items, in `required` order
    int persons = 0;
    std::int64_t since_ms = 0;        // when the episode's first item was raised
};

struct PpeStatus {
    int frames = 0, evals = 0, persons_max = 0;
    std::map<std::string, int> present_evals;   // evaluations on which the class was present for every person
    std::map<std::string, int> detected_frames; // PPE frames with at least one item of the class (anywhere)
};

class PpeChecker {
public:
    static constexpr std::int64_t kMaxGapMs = 2000;

    void configure(const PpeCheckConfig& cfg) {
        cfg_ = cfg;
        for (auto& r : cfg_.required) r = canonical_ppe_class(r);
    }
    const PpeCheckConfig& config() const { return cfg_; }

    // Persons lacking a class on one frame, no state involved.
    std::vector<std::string> evaluate(const std::vector<PpePerson>& persons, const std::vector<NamedBox>& items) const {
        std::vector<std::string> missing;
        for (const std::string& need : cfg_.required) {
            for (const PpePerson& p : persons) {
                if (!has(p.box, need, items)) { missing.push_back(need); break; }
            }
        }
        return missing;
    }

    // `persons`: the fresh person boxes (the caller drops stale ones). Returns an event only on change.
    std::optional<PpeEvent> update(std::int64_t now_ms, const std::vector<PpePerson>& persons, const std::vector<NamedBox>& items) {
        if (!cfg_.enabled) return std::nullopt;
        ++status_.frames;
        std::set<std::string> seen;
        for (const NamedBox& it : items) seen.insert(canonical_ppe_class(it.label));
        for (const std::string& c : seen) ++status_.detected_frames[c];
        if (persons.empty()) return std::nullopt; // no evaluation: never an alert, never an all-clear
        ++status_.evals;
        status_.persons_max = std::max<int>(status_.persons_max, static_cast<int>(persons.size()));
        const bool gap = last_eval_ms_ >= 0 && now_ms - last_eval_ms_ > kMaxGapMs;
        last_eval_ms_ = now_ms;
        last_persons_ = static_cast<int>(persons.size());
        if (gap) {
            people_.clear();
            for (auto& [need, s] : items_) s.clear_since = -1;
        }
        // Forget people who left; keep the timers of those still here.
        std::set<int> here;
        for (const PpePerson& p : persons) here.insert(p.id);
        for (auto it = people_.begin(); it != people_.end();) it = here.count(it->first) ? std::next(it) : people_.erase(it);

        std::map<std::string, bool> any_raised, all_present;
        for (const std::string& need : cfg_.required) { any_raised[need] = false; all_present[need] = true; }
        for (const PpePerson& p : persons) {
            auto& state = people_[p.id];
            for (const std::string& need : cfg_.required) {
                std::int64_t& since = state[need];
                if (!has(p.box, need, items)) {
                    all_present[need] = false;
                    if (since <= 0) since = now_ms;
                    if (now_ms - since >= cfg_.missing_ms) any_raised[need] = true;
                } else {
                    since = 0;
                }
            }
        }
        bool changed = false;
        for (const std::string& need : cfg_.required) {
            Item& s = items_[need];
            if (all_present[need]) ++status_.present_evals[need];
            if (any_raised[need]) {
                s.clear_since = -1;
                if (!s.raised) { s.raised = true; changed = true; }
            } else if (s.raised && !all_present[need]) {
                s.clear_since = -1; // somebody still lacks it (their timer has not run out yet, e.g. a new track id): stays raised
            } else if (s.raised) {
                if (s.clear_since < 0) s.clear_since = now_ms;
                if (now_ms - s.clear_since >= cfg_.clear_ms) { s.raised = false; s.clear_since = -1; changed = true; }
            }
        }
        if (!changed) return std::nullopt;
        PpeEvent ev;
        ev.missing = raised();
        ev.persons = last_persons_;
        if (ev.missing.empty()) {
            if (!in_episode_) return std::nullopt;
            in_episode_ = false;
            ev.state = "ok";
        } else {
            if (!in_episode_) { in_episode_ = true; episode_start_ms_ = now_ms; }
            ev.state = "missing";
        }
        ev.since_ms = episode_start_ms_;
        return ev;
    }

    std::vector<std::string> raised() const {
        std::vector<std::string> out;
        for (const std::string& need : cfg_.required) {
            const auto it = items_.find(need);
            if (it != items_.end() && it->second.raised) out.push_back(need);
        }
        return out;
    }
    bool in_episode() const { return in_episode_; }
    int last_persons() const { return last_persons_; }
    PpeStatus take_status() { PpeStatus s = std::move(status_); status_ = {}; return s; }

private:
    struct Item {
        std::int64_t clear_since = -1;
        bool raised = false;
    };

    bool has(const Box& person, const std::string& need, const std::vector<NamedBox>& items) const {
        const float px = person.width() * cfg_.person_box_pad, py = person.height() * cfg_.person_box_pad;
        for (const NamedBox& it : items) {
            if (canonical_ppe_class(it.label) != need) continue;
            const float cx = it.box.cx(), cy = it.box.cy();
            if (cx >= person.x1 - px && cx <= person.x2 + px && cy >= person.y1 - py && cy <= person.y2 + py) return true;
        }
        return false;
    }

    PpeCheckConfig cfg_;
    std::map<std::string, Item> items_;
    std::map<int, std::map<std::string, std::int64_t>> people_; // person id -> item -> missing since (0 = present)
    PpeStatus status_;
    bool in_episode_ = false;
    std::int64_t episode_start_ms_ = 0, last_eval_ms_ = -1;
    int last_persons_ = 0;
};

} // namespace rigmind
