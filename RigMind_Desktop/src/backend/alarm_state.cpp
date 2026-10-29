#include "alarm_state.h"

namespace rigmind {

const char* to_string(AlarmState state) {
    switch (state) {
    case AlarmState::Yellow: return "YELLOW";
    case AlarmState::Orange: return "ORANGE";
    case AlarmState::Red: return "RED";
    case AlarmState::None: break;
    }
    return "NONE";
}

AlarmStateMachine::AlarmStateMachine(AlarmThresholds thresholds) : thresholds_(thresholds) {}

// Mirrors ews.cpp:418-517. The IDSS `else if (CURRENT == RED)` inside the `!= ORANGE` branch
// was unreachable and is not ported.
AlarmState AlarmStateMachine::step(const FrameFacts& f) {
    if (f.machine_present) {
        if (f.link_tilt_angled) {
            // Stabilize the YELLOW state
            if (current_ != AlarmState::Yellow && ++yellow_ > thresholds_.yellow) {
                set_state(AlarmState::Yellow);
            }

            if (f.elevator_near_platform) {
                if (current_ != AlarmState::Orange && ++orange_ > thresholds_.orange) {
                    set_state(AlarmState::Orange);
                    yellow_ = 0;
                }

                if (f.elevator_present) {
                    if (f.platform_elevator_collision || f.derrickman_out_of_safe_zone || f.machine_past_right_edge) {
                        if (current_ != AlarmState::Red && ++red_ > thresholds_.red) {
                            set_state(AlarmState::Red);
                            orange_ = 0;
                            yellow_ = 0;
                        }
                    } else if (--red_ > thresholds_.red) {
                        set_state(AlarmState::Red);
                    }
                }
            } else {
                orange_--;
            }
        }
    } else {
        if (f.link_tilt_present && f.link_tilt_near_derrickman) {
            set_state(AlarmState::Yellow);
        }
        yellow_ = 0;
        orange_ = 0;
        red_ = 0;
    }

    previous_ = current_;
    return current_;
}

} // namespace rigmind
