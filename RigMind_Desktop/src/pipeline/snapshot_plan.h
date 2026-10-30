// Orange alarm image evidence: when snapshots are taken. Pure functions, unit-tested in tests/backend_test.cpp.
//
// A set starts when the alarm enters Orange: one image at once, one after 3 s and one after 6 s, as long as the
// alarm is still Orange. The images are <data>\footage\<cam>\<stamp>\orange-1.jpg ... orange-3.jpg.
#pragma once

#include <cstdint>
#include <string>

namespace rigmind {

constexpr int kSnapshotMax = 3;
constexpr std::int64_t kSnapshotOffsetsMs[kSnapshotMax] = {0, 3000, 6000};
constexpr std::int64_t kSnapshotCooldownMs = 20000; // Yellow<->Orange flapping must not create a set every few seconds

// How many snapshots should exist `elapsed_ms` after the Orange onset (1..3).
inline int snapshots_due(std::int64_t elapsed_ms) {
    int due = 0;
    for (int i = 0; i < kSnapshotMax; ++i)
        if (elapsed_ms >= kSnapshotOffsetsMs[i]) due = i + 1;
    return due;
}

// 1-based number to file name: orange-1.jpg.
inline std::string snapshot_file_name(int number) {
    return "orange-" + std::to_string(number) + ".jpg";
}

// The set is complete once all snapshots exist or the window (last offset + 500 ms of slack) has passed.
inline bool snapshot_set_complete(int taken, std::int64_t elapsed_ms) {
    return taken >= kSnapshotMax || elapsed_ms > kSnapshotOffsetsMs[kSnapshotMax - 1] + 500;
}

// A new set may start unless the previous one began less than the cooldown ago.
inline bool snapshot_set_allowed(std::int64_t last_start_ms, std::int64_t now_ms) {
    return last_start_ms <= 0 || now_ms - last_start_ms >= kSnapshotCooldownMs;
}

} // namespace rigmind
