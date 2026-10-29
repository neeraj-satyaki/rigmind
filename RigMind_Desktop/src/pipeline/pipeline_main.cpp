// rigmind-pipeline: child process of the RigMind host, one per app instance.
// Per camera: RTSP H.264 is repacketised to RTP and sent with webrtcbin.
// No decode and no encode here. Inference is added to this process later; the host never decodes video.
//
// Protocol, one JSON object per line:
//   stdin : {"cmd":"open","cam":"CAM-01","uri":"rtsp://..."}
//           {"cmd":"offer","cam":"CAM-01","sdp":"..."}     (browser recvonly offer, full SDP)
//           {"cmd":"close","cam":"CAM-01"}
//   stdout: {"ev":"answer","cam":"CAM-01","sdp":"..."}     (sent after ICE gathering completes)
//           {"ev":"error","cam":"CAM-01","message":"..."}
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <atomic>
#include <map>
#include <set>
#include "pipeline/plugins/ews/rigmind_facts_meta.h"
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#endif

using json = nlohmann::json;

namespace {

struct Camera {
    std::string id;
    GstElement* pipeline = nullptr;
    GstElement* webrtc = nullptr;
    std::string cache_dir;
    guint bus_watch = 0;
    bool answer_sent = false;
    // Alarm episode (None -> any alarm -> None). Evidence is kept only if the episode reaches Red.
    std::filesystem::path evidence_dir; // empty when no episode is active
    std::string evidence_stamp;
    std::set<std::string> evidence_files; // segment names already copied
    bool evidence_saved = false;          // footage row already reported for this episode
};

std::map<std::string, Camera> cameras;
GMainLoop* loop = nullptr;
std::string data_root = ".";     // cache/ and footage/ live under this folder

void emit(const json& message) {
    std::cout << message.dump() << '\n' << std::flush;
}

void emit_error(const std::string& cam, const std::string& text) {
    emit({{"ev", "error"}, {"cam", cam}, {"message", text}});
}

void close_camera(const std::string& id) {
    auto it = cameras.find(id);
    if (it == cameras.end()) return;
    Camera& cam = it->second;
    if (cam.bus_watch) g_source_remove(cam.bus_watch);
    gst_element_set_state(cam.pipeline, GST_STATE_NULL);
    gst_object_unref(cam.pipeline);
    cameras.erase(it);
}

// Vanilla ICE: one answer message, written once local candidates are complete.
void maybe_send_answer(Camera* cam) {
    if (cam->answer_sent) return;
    GstWebRTCICEGatheringState state;
    g_object_get(cam->webrtc, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;

    GstWebRTCSessionDescription* local = nullptr;
    g_object_get(cam->webrtc, "local-description", &local, nullptr);
    if (!local) return;

    cam->answer_sent = true;
    gchar* sdp = gst_sdp_message_as_text(local->sdp);
    emit({{"ev", "answer"}, {"cam", cam->id}, {"sdp", sdp}});
    g_free(sdp);
    gst_webrtc_session_description_free(local);
}

void on_gathering_state(GObject*, GParamSpec*, gpointer user) {
    maybe_send_answer(static_cast<Camera*>(user));
}

void on_answer_created(GstPromise* promise, gpointer user) {
    auto* cam = static_cast<Camera*>(user);
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
    gst_promise_unref(promise);
    if (!answer) {
        emit_error(cam->id, "no answer generated");
        return;
    }
    g_signal_emit_by_name(cam->webrtc, "set-local-description", answer, nullptr);
    gst_webrtc_session_description_free(answer);
    maybe_send_answer(cam);
}

void on_offer_set(GstPromise* promise, gpointer user) {
    gst_promise_unref(promise);
    auto* cam = static_cast<Camera*>(user);
    GstPromise* answer = gst_promise_new_with_change_func(on_answer_created, cam, nullptr);
    g_signal_emit_by_name(cam->webrtc, "create-answer", nullptr, answer);
}

void handle_offer(Camera* cam, const std::string& sdp_text) {
    GstSDPMessage* sdp = nullptr;
    gst_sdp_message_new(&sdp);
    if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(sdp_text.data()), sdp_text.size(), sdp) != GST_SDP_OK) {
        gst_sdp_message_free(sdp);
        emit_error(cam->id, "invalid offer");
        return;
    }
    // Takes ownership of sdp.
    GstWebRTCSessionDescription* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    GstPromise* promise = gst_promise_new_with_change_func(on_offer_set, cam, nullptr);
    g_signal_emit_by_name(cam->webrtc, "set-remote-description", offer, promise);
    gst_webrtc_session_description_free(offer);
}

// rtspsrc adds one pad per RTP stream; only the video stream goes to the depayloader.
void on_pad_added(GstElement*, GstPad* pad, gpointer depay) {
    GstCaps* caps = gst_pad_query_caps(pad, nullptr);
    bool is_video = false;
    if (gst_caps_get_size(caps) > 0) {
        const GstStructure* s = gst_caps_get_structure(caps, 0);
        is_video = g_strcmp0(gst_structure_get_string(s, "media"), "video") == 0;
    }
    gst_caps_unref(caps);
    if (!is_video) return;

    GstPad* sink = gst_element_get_static_pad(GST_ELEMENT(depay), "sink");
    if (!gst_pad_is_linked(sink)) gst_pad_link(pad, sink);
    gst_object_unref(sink);
}

void save_clip(const std::string& id);
void begin_evidence(Camera& cam);
void end_evidence(Camera& cam);

gboolean on_bus(GstBus*, GstMessage* msg, gpointer user) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &error, &debug);
        emit_error(static_cast<Camera*>(user)->id, error ? error->message : "pipeline error");
        g_clear_error(&error);
        g_free(debug);
    }
    // rigmindews posts every EWS state change here. Forwarded at once; a red alarm also saves footage.
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_APPLICATION) {
        const GstStructure* s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "rigmind-alarm")) {
            guint64 frame = 0;
            gst_structure_get_uint64(s, "frame", &frame);
            const std::string state = gst_structure_get_string(s, "state");
            emit({{"ev", "alarm"},
                  {"cam", gst_structure_get_string(s, "cam")},
                  {"state", state},
                  {"previous", gst_structure_get_string(s, "previous")},
                  {"severity", gst_structure_get_string(s, "severity")},
                  {"condition", gst_structure_get_string(s, "condition")},
                  {"place", gst_structure_get_string(s, "place")},
                  {"frame", frame}});
            // Evidence: starts at the first alarm state (Yellow), is kept on Red, and is dropped if the episode ends without Red.
            Camera& cam = *static_cast<Camera*>(user);
            if (state == "None") end_evidence(cam);
            else if (std::string(gst_structure_get_string(s, "previous")) == "None") begin_evidence(cam);
            if (state == "Red") save_clip(cam.id);
        }
    }
    return TRUE;
}

// One live stream at a time: keeps RAM and GPU decode within a 4 GB machine.
constexpr std::size_t kMaxCameras = 1;

// EWS facts for the frames that follow. The "facts" command sets them now; the detector will set them
// in-process later. All flags live in one atomic word, so the streaming thread reads a consistent
// snapshot without a lock.
struct FactField {
    const char* name;
    bool rigmind::FrameFacts::*member;
};
const FactField kFactFields[] = {
    {"machine_present", &rigmind::FrameFacts::machine_present},
    {"link_tilt_present", &rigmind::FrameFacts::link_tilt_present},
    {"link_tilt_angled", &rigmind::FrameFacts::link_tilt_angled},
    {"elevator_present", &rigmind::FrameFacts::elevator_present},
    {"elevator_near_platform", &rigmind::FrameFacts::elevator_near_platform},
    {"platform_elevator_collision", &rigmind::FrameFacts::platform_elevator_collision},
    {"derrickman_out_of_safe_zone", &rigmind::FrameFacts::derrickman_out_of_safe_zone},
    {"machine_past_right_edge", &rigmind::FrameFacts::machine_past_right_edge},
    {"link_tilt_near_derrickman", &rigmind::FrameFacts::link_tilt_near_derrickman},
};
std::atomic<std::uint32_t> ews_facts{0};

rigmind::FrameFacts facts_from_bits(std::uint32_t bits) {
    rigmind::FrameFacts facts;
    for (std::size_t i = 0; i < sizeof(kFactFields) / sizeof(kFactFields[0]); ++i) {
        facts.*(kFactFields[i].member) = (bits >> i) & 1u;
    }
    return facts;
}

// Attaches the current facts to every frame that reaches the EWS branch. The tee shares buffers between
// branches, so the frame is made writable first; that copies only if another branch still holds it.
GstPadProbeReturn on_facts_probe(GstPad*, GstPadProbeInfo* info, gpointer) {
    GstBuffer* buffer = gst_buffer_make_writable(gst_pad_probe_info_get_buffer(info));
    rigmind_facts_meta_add(buffer, facts_from_bits(ews_facts.load(std::memory_order_relaxed)));
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

void open_camera(const std::string& id, const std::string& uri) {
    if (uri.rfind("rtsp://", 0) != 0) {
        emit_error(id, "invalid uri");
        return;
    }
    if (cameras.find(id) == cameras.end() && cameras.size() >= kMaxCameras) {
        emit_error(id, "camera limit reached");
        return;
    }
    close_camera(id);

    Camera& cam = cameras[id];
    cam.id = id;
    cam.cache_dir = (std::filesystem::path(data_root) / "cache" / id).string();
    std::filesystem::create_directories(cam.cache_dir);

    cam.pipeline = gst_pipeline_new(id.c_str());
    GstElement* src = gst_element_factory_make("rtspsrc", "src");
    GstElement* depay = gst_element_factory_make("rtph264depay", "depay");
    GstElement* parse = gst_element_factory_make("h264parse", "parse");
    GstElement* tee = gst_element_factory_make("tee", "tee");
    GstElement* live_queue = gst_element_factory_make("queue", "live_queue");
    GstElement* pay = gst_element_factory_make("rtph264pay", "pay");
    GstElement* rec_queue = gst_element_factory_make("queue", "rec_queue");
    GstElement* rec_sink = gst_element_factory_make("splitmuxsink", "rec_sink");
    GstElement* alarm_queue = gst_element_factory_make("queue", "alarm_queue");
    GstElement* ews = gst_element_factory_make("rigmindews", "ews");
    GstElement* ews_sink = gst_element_factory_make("fakesink", "ews_sink");
    cam.webrtc = gst_element_factory_make("webrtcbin", "webrtc");
    if (!src || !depay || !parse || !tee || !live_queue || !pay || !rec_queue || !rec_sink || !cam.webrtc ||
        !alarm_queue || !ews || !ews_sink) {
        emit_error(id, "missing GStreamer plugin (rtsp, rtp, webrtc, splitmuxsink, rigmindews)");
        close_camera(id);
        return;
    }

    g_object_set(src, "location", uri.c_str(), "latency", static_cast<guint>(0), nullptr);
    g_object_set(parse, "config-interval", -1, nullptr);
    g_object_set(pay, "config-interval", -1, "pt", static_cast<guint>(96), nullptr);
    g_object_set(cam.webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
    // Live branch drops old frames when no peer is connected, so it can never stall the tee and the cache.
    g_object_set(live_queue, "leaky", 2 /* downstream */, nullptr);
    // Rolling cache: 10 s segments, newest 12 kept (2 minutes). Old segments are replaced automatically.
    const std::string location = (std::filesystem::path(cam.cache_dir) / "seg%05d.mp4").string();
    g_object_set(rec_sink, "location", location.c_str(), "max-size-time", static_cast<guint64>(10 * GST_SECOND),
                 "max-files", static_cast<guint>(12), "muxer-factory", "mp4mux", nullptr);
    g_signal_connect(src, "pad-added", G_CALLBACK(on_pad_added), depay);
    // Alarm branch: small leaky queue, so the EWS check never delays the live or cache branches.
    g_object_set(alarm_queue, "leaky", 2 /* downstream */, "max-size-buffers", static_cast<guint>(8),
                 "max-size-bytes", static_cast<guint>(0), "max-size-time", static_cast<guint64>(0), nullptr);
    g_object_set(ews, "cam-id", id.c_str(), nullptr);
    g_object_set(ews_sink, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(cam.pipeline), src, depay, parse, tee, live_queue, pay, rec_queue, rec_sink,
                     cam.webrtc, alarm_queue, ews, ews_sink, nullptr);
    // One decode-free stream feeds three branches: live WebRTC, the segment cache, and EWS alarms.
    if (!gst_element_link_many(depay, parse, tee, nullptr) ||
        !gst_element_link_many(live_queue, pay, nullptr) ||
        !gst_element_link_many(rec_queue, rec_sink, nullptr) ||
        !gst_element_link_many(alarm_queue, ews, ews_sink, nullptr) ||
        !gst_element_link(tee, live_queue) || !gst_element_link(tee, rec_queue) ||
        !gst_element_link(tee, alarm_queue)) {
        emit_error(id, "cannot link pipeline");
        close_camera(id);
        return;
    }

    GstPad* alarm_in = gst_element_get_static_pad(alarm_queue, "sink");
    gst_pad_add_probe(alarm_in, GST_PAD_PROBE_TYPE_BUFFER, on_facts_probe, nullptr, nullptr);
    gst_object_unref(alarm_in);

    // webrtcbin takes RTP through a requested sink pad bound to a sendonly transceiver.
    GstCaps* rtp_caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000");
    GstWebRTCRTPTransceiver* transceiver = nullptr;
    g_signal_emit_by_name(cam.webrtc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, rtp_caps, &transceiver);
    gst_caps_unref(rtp_caps);
    if (transceiver) gst_object_unref(transceiver);

    GstPad* sinkpad = gst_element_request_pad_simple(cam.webrtc, "sink_%u");
    GstPad* srcpad = gst_element_get_static_pad(pay, "src");
    const bool linked = sinkpad && gst_pad_link(srcpad, sinkpad) == GST_PAD_LINK_OK;
    gst_object_unref(srcpad);
    if (sinkpad) gst_object_unref(sinkpad);
    if (!linked) {
        emit_error(id, "cannot link webrtc sink");
        close_camera(id);
        return;
    }

    g_signal_connect(cam.webrtc, "notify::ice-gathering-state", G_CALLBACK(on_gathering_state), &cam);
    GstBus* bus = gst_element_get_bus(cam.pipeline);
    cam.bus_watch = gst_bus_add_watch(bus, on_bus, &cam);
    gst_object_unref(bus);

    gst_element_set_state(cam.pipeline, GST_STATE_PLAYING);
}

// Newest completed cache segment (the newest file is still being written, so it is skipped), or empty.
std::filesystem::path newest_completed_segment(const std::string& cache_dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> segments;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.path().extension() == ".mp4") segments.push_back(entry.path());
    }
    std::sort(segments.begin(), segments.end()); // zero-padded names sort chronologically
    if (segments.size() < 2) return {};
    return segments[segments.size() - 2];
}

// Copies the newest completed 10 s segment into the episode folder, once per segment.
void copy_newest_segment(Camera& cam) {
    const auto segment = newest_completed_segment(cam.cache_dir);
    if (segment.empty()) return;
    const std::string name = segment.filename().string();
    if (cam.evidence_files.count(name)) return;
    std::filesystem::copy_file(segment, cam.evidence_dir / name, std::filesystem::copy_options::overwrite_existing);
    cam.evidence_files.insert(name);
}

// Yellow onset: the episode folder is created and evidence starts with the newest completed segment.
void begin_evidence(Camera& cam) {
    if (!cam.evidence_dir.empty()) return;
    cam.evidence_stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    cam.evidence_dir = std::filesystem::path(data_root) / "footage" / cam.id / cam.evidence_stamp;
    std::filesystem::create_directories(cam.evidence_dir);
    copy_newest_segment(cam);
    emit({{"ev", "evidence"}, {"cam", cam.id}, {"stamp", cam.evidence_stamp}, {"phase", "started"}});
}

// Red: evidence is completed and reported once per episode. The footage row counts the 10 s segments.
void save_clip(const std::string& id) {
    auto it = cameras.find(id);
    if (it == cameras.end()) {
        emit_error(id, "camera not open");
        return;
    }
    Camera& cam = it->second;
    begin_evidence(cam);
    copy_newest_segment(cam);
    if (cam.evidence_saved) return;
    cam.evidence_saved = true;
    emit({{"ev", "clip"}, {"cam", id}, {"stamp", cam.evidence_stamp}, {"count", cam.evidence_files.size()}});
}

// Episode over without Red: nothing is kept.
void end_evidence(Camera& cam) {
    if (!cam.evidence_saved && !cam.evidence_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(cam.evidence_dir, ec);
    }
    cam.evidence_dir.clear();
    cam.evidence_stamp.clear();
    cam.evidence_files.clear();
    cam.evidence_saved = false;
}

void handle_command(const std::string& text) {
    json command;
    try {
        command = json::parse(text);
    } catch (const json::exception&) {
        emit({{"ev", "error"}, {"message", "invalid json"}});
        return;
    }
    const std::string cmd = command.value("cmd", std::string());
    const std::string cam = command.value("cam", std::string());

    if (cmd == "open") {
        open_camera(cam, command.value("uri", std::string()));
    } else if (cmd == "offer") {
        auto it = cameras.find(cam);
        if (it == cameras.end()) emit_error(cam, "camera not open");
        else handle_offer(&it->second, command.value("sdp", std::string()));
    } else if (cmd == "close") {
        close_camera(cam);
    } else if (cmd == "clip") {
        save_clip(cam);
    } else if (cmd == "facts") {
        // {"cmd":"facts","cam":"CAM-01","machine_present":true,...}: unknown keys are ignored, missing keys are false.
        std::uint32_t bits = 0;
        for (std::size_t i = 0; i < sizeof(kFactFields) / sizeof(kFactFields[0]); ++i) {
            if (command.value(kFactFields[i].name, false)) bits |= 1u << i;
        }
        ews_facts.store(bits, std::memory_order_relaxed);
    } else {
        emit_error(cam, "unknown command");
    }
}

gboolean on_stdin(GIOChannel* channel, GIOCondition, gpointer) {
    gchar* line = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    const GIOStatus status = g_io_channel_read_line(channel, &line, &length, nullptr, &error);
    g_clear_error(&error);

    if (status == G_IO_STATUS_AGAIN) return TRUE;
    if (status != G_IO_STATUS_NORMAL) {
        g_free(line);
        g_main_loop_quit(loop); // host closed stdin or the pipe broke
        return FALSE;
    }
    handle_command(std::string(line, length));
    g_free(line);
    return TRUE;
}

} // namespace

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    if (argc > 1) data_root = argv[1];
    // argv[2]: path to the rigmindews plugin module. Without it, the GStreamer plugin registry must already know it.
    if (argc > 2 && !gst_plugin_load_file(argv[2], nullptr)) {
        emit({{"ev", "error"}, {"message", std::string("cannot load plugin ") + argv[2]}});
    }
    loop = g_main_loop_new(nullptr, FALSE);

#ifdef _WIN32
    GIOChannel* input = g_io_channel_win32_new_fd(_fileno(stdin));
#else
    GIOChannel* input = g_io_channel_unix_new(0);
#endif
    g_io_channel_set_encoding(input, nullptr, nullptr); // raw bytes, one JSON line per read
    g_io_add_watch(input, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), on_stdin, nullptr);

    g_main_loop_run(loop);

    std::vector<std::string> open_ids;
    for (const auto& entry : cameras) open_ids.push_back(entry.first);
    for (const auto& id : open_ids) close_camera(id);

    g_io_channel_unref(input);
    g_main_loop_unref(loop);
    return 0;
}
