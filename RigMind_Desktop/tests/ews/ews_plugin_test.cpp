// Checks that rigmindews posts exactly the state transitions the EWS state machine produces for the same frames.
// Usage: ews_plugin_test <path to librigmindews.so>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <cstdio>
#include <string>
#include <vector>
#include "pipeline/plugins/ews/rigmind_facts_meta.h"

using rigmind::AlarmState;
using rigmind::FrameFacts;

namespace {

FrameFacts yellow_frame() {
    FrameFacts f;
    f.machine_present = true;
    f.link_tilt_present = true;
    f.link_tilt_angled = true;
    return f;
}

FrameFacts collision_frame() {
    FrameFacts f = yellow_frame();
    f.elevator_present = true;
    f.elevator_near_platform = true;
    f.platform_elevator_collision = true;
    return f;
}

// Expected transitions: the state machine run directly on the same frames.
std::vector<std::string> expected_transitions(const std::vector<FrameFacts>& frames) {
    std::vector<std::string> out;
    rigmind::AlarmStateMachine machine;
    for (const FrameFacts& f : frames) {
        const AlarmState before = machine.current();
        const AlarmState after = machine.step(f);
        if (after != before) {
            // Same names the plugin posts: "None", "Yellow", "Orange", "Red".
            const char* names[] = {"None", "Yellow", "Orange", "Red"};
            out.push_back(names[static_cast<int>(after)]);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ews_plugin_test <librigmindews.so>\n");
        return 2;
    }
    gst_init(nullptr, nullptr);
    GstPlugin* plugin = gst_plugin_load_file(argv[1], nullptr);
    if (!plugin) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }

    // Yellow stretch, a quiet stretch (counters fall), then a long collision stretch (escalates).
    std::vector<FrameFacts> frames;
    for (int i = 0; i < 30; ++i) frames.push_back(yellow_frame());
    for (int i = 0; i < 60; ++i) frames.push_back(FrameFacts{});
    for (int i = 0; i < 300; ++i) frames.push_back(collision_frame());
    const std::vector<std::string> expected = expected_transitions(frames);
    if (expected.size() < 3) {
        std::fprintf(stderr, "test sequence too weak: %zu transitions\n", expected.size());
        return 1;
    }

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "appsrc name=src caps=video/x-raw,format=RGB,width=2,height=2,framerate=25/1 ! "
        "rigmindews cam-id=CAM-01 ! fakesink sync=false",
        &error);
    if (!pipeline) {
        std::fprintf(stderr, "pipeline: %s\n", error ? error->message : "unknown");
        return 1;
    }
    GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    guint64 pts = 0;
    for (const FrameFacts& f : frames) {
        GstBuffer* buffer = gst_buffer_new_and_alloc(12);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 25;
        pts += GST_SECOND / 25;
        rigmind_facts_meta_add(buffer, f);
        gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    std::vector<std::string> actual;
    bool done = false;
    while (!done) {
        GstMessage* msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_APPLICATION | GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!msg) break; // timeout: fail below
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_APPLICATION: {
                const GstStructure* s = gst_message_get_structure(msg);
                if (gst_structure_has_name(s, "rigmind-alarm")) actual.push_back(gst_structure_get_string(s, "state"));
                break;
            }
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gst_message_parse_error(msg, &err, nullptr);
                std::fprintf(stderr, "pipeline error: %s\n", err ? err->message : "?");
                g_clear_error(&err);
                done = true;
                break;
            }
            default:
                done = true; // EOS
        }
        gst_message_unref(msg);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_object_unref(pipeline);

    if (actual != expected || actual.empty()) {
        std::fprintf(stderr, "FAIL: plugin posted %zu transitions, state machine expects %zu\n", actual.size(), expected.size());
        return 1;
    }
    std::printf("ews plugin test passed: %zu transitions match the state machine\n", actual.size());
    return 0;
}
