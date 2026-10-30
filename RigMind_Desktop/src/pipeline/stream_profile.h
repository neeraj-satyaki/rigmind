// Per-viewer stream profile of the live video: "full" (1280x720, 4000 kbps) for the Live page, "small" (640x360, 1200 kbps) for
// dashboard tiles. Header-only so it can be unit tested without GStreamer.
#pragma once

#include <string>

namespace rigmind {

struct StreamProfile {
    const char* name;
    int width, height;
    int kbps;
};

// Unknown or empty names give the full profile (the previous behaviour).
inline StreamProfile stream_profile(const std::string& name) {
    if (name == "small") return {"small", 640, 360, 1200};
    return {"full", 1280, 720, 4000};
}

// Encoder bitrate property value for a profile: kbps for mfh264enc / qsvh264enc / nvh264enc, bits per second for openh264enc.
inline unsigned encoder_bitrate_value(const StreamProfile& profile, const std::string& encoder_factory) {
    return encoder_factory == "openh264enc" ? static_cast<unsigned>(profile.kbps) * 1000u : static_cast<unsigned>(profile.kbps);
}

} // namespace rigmind
