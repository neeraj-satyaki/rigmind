#pragma once

// Port of IDSS src/safety/ews.cpp IDSS::algorithm() state machine.
// Geometry (centroids, displacements, edges) is decided by the caller and passed in as FrameFacts.

namespace rigmind {

enum class AlarmState { None, Yellow, Orange, Red };

const char* to_string(AlarmState state);

struct AlarmThresholds {
    int yellow = 4;
    int orange = 20;
    int red = 200;

    // IDSS setThresholds(): zoom mode uses longer counters.
    static AlarmThresholds for_zoom(bool zoom) {
        return zoom ? AlarmThresholds{30, 100, 200} : AlarmThresholds{4, 20, 200};
    }
};

struct FrameFacts {
    bool machine_present = false;
    bool link_tilt_present = false;
    bool link_tilt_angled = false;
    bool elevator_present = false;
    bool elevator_near_platform = false;      // elevator-platform distance < MAX_ELEVATOR_PLATFORM_DIST_THRESHOLD
    bool platform_elevator_collision = false;
    bool derrickman_out_of_safe_zone = false; // derrickman x > MAX_DERRICKAMAN_SAFE_ZONE
    bool machine_past_right_edge = false;     // machine x > RIGHT_EDGE_THRESHOLD
    bool link_tilt_near_derrickman = false;   // derrickman-link-tilt y < DERRICKMAN_LINKTILT_DIST_THRESHOLD
};

class AlarmStateMachine {
public:
    explicit AlarmStateMachine(AlarmThresholds thresholds = {});

    // Advances one frame and returns the state after it.
    AlarmState step(const FrameFacts& facts);

    AlarmState current() const { return current_; }
    AlarmState previous() const { return previous_; }

private:
    void set_state(AlarmState state) { current_ = state; }

    AlarmThresholds thresholds_;
    int yellow_ = 0;
    int orange_ = 0;
    int red_ = 0;
    AlarmState current_ = AlarmState::None;
    AlarmState previous_ = AlarmState::None;
};

} // namespace rigmind
