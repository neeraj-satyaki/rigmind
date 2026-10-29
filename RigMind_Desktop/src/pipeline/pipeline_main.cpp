// rigmind-pipeline: child process of the RigMind host, one per app instance.
//
// Per camera, one RTSP H.264 stream (the original, never altered) feeds three branches:
//   - segment cache + alarm evidence (original quality, no decode)
//   - EWS alarm element (no decode)
//   - live view: hardware decode (running all the time), scale to 1280x720 @ <=30 fps, re-encode without B-frames
//     (keyframe every 30 frames, and at once when a viewer attaches), then WebRTC. The source is a large High-profile
//     stream with B-frames and a single keyframe per session; browsers cannot play that directly.
//
// A camera is kept connected: if the stream fails or stalls it is torn down and re-opened with back-off
// (2, 4, 8, 10 s; 30 s after an authentication failure) until the host closes it. Other cameras are not affected.
//
// Detection: a second branch taps the decoded 1280x720 frames (about 10 per second, RGB, leaky, never blocking the live
// path) and feeds the PPE and IDSS ONNX models (pipeline/inference.*). It runs whenever the camera is online.
//
// Usage: rigmind-pipeline [data_root] [path-to-rigmindews-plugin] [models_dir]
//
// Protocol, one JSON object per line:
//   stdin : {"cmd":"open","cam":"CAM-01","uri":"rtsp://...","user":"","pass":""}   (user/pass optional; may also be in the URI)
//           {"cmd":"offer","cam":"CAM-01","sdp":"..."}     (browser recvonly offer, full SDP)
//           {"cmd":"detach","cam":"CAM-01"}                (viewer left; camera keeps recording)
//           {"cmd":"status"}                               (re-emit the status of every camera)
//           {"cmd":"close","cam":"CAM-01"}                 (stop the camera and its reconnects)
//   stdout: {"ev":"status","cam":..,"state":"connecting|online|offline","reason":..,"width":..,"height":..,"fps":..}
//           {"ev":"stats","cam":..,"fps":..,"kbps":..,"reconnects":..,"resyncs":..,"live":..,"encoder":..}   (every 2 s)
//           {"ev":"detections","cam":..,"model":"ppe|idss","ts":<unix ms>,"w":1280,"h":720,"ms":..,"items":[{"cls":..,"label":..,"conf":..,"box":[x1,y1,x2,y2]}]}
//           {"ev":"inference","cam":..,"model":..,"fps":..,"avg_ms":..,"conf":..,"iou":..}   (every 2 s, per model)
//           {"ev":"answer","cam":"CAM-01","sdp":"..."}     (sent after ICE gathering completes)
//           {"ev":"error","cam":"CAM-01","message":"..."}
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include "pipeline/inference.h"
#include "pipeline/overlay_draw.h"
#include <opencv2/core.hpp>
#include "pipeline/plugins/ews/rigmind_facts_meta.h"
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

using json = nlohmann::json;

namespace {

// One live stream at a time: keeps RAM and GPU decode within a 4 GB machine.
constexpr std::size_t kMaxCameras = 1;
constexpr gint64 kConnectTimeoutUs = 25 * G_USEC_PER_SEC; // no first frame after this: reconnect
constexpr gint64 kStallTimeoutUs = 6 * G_USEC_PER_SEC;    // online but no frame for this long: reconnect
constexpr guint kRetryMinMs = 2000;
constexpr guint kRetryMaxMs = 10000;
constexpr guint kRetryAuthMs = 30000;                      // do not hammer a camera with wrong credentials

struct Camera {
    std::string id;
    // Connection settings, kept across reconnects.
    std::string uri, user, pass;
    bool want = false; // the host wants this camera open; reconnect until closed
    std::string state = "offline";
    std::string reason;
    unsigned retry_count = 0; // consecutive failures, drives the back-off
    unsigned reconnects = 0;  // times the stream came back after being online
    bool ever_online = false;
    guint retry_source = 0;
    guint generation = 0;   // bumped for every pipeline built; stale callbacks compare it
    guint peer_serial = 0;  // bumped for every viewer
    guint variant = 0;      // index into the live branch candidates
    gint64 started_us = 0;

    std::atomic<gint64> last_buffer_us{0};
    std::atomic<guint64> frames{0};
    std::atomic<guint64> bytes{0};
    std::atomic<bool> got_buffer{false};
    std::atomic<bool> live_enabled{false}; // a viewer is attached: let video into the live branch
    std::atomic<unsigned> resyncs{0};
    std::atomic<guint64> keyframes{0};
    std::atomic<gint64> last_key_us{0};
    guint64 stats_frames = 0, stats_bytes = 0;
    gint64 stats_us = 0;
    int width = 0, height = 0;
    double fps = 0;

    GstElement* pipeline = nullptr;
    GstElement* live_bin = nullptr;   // owned by pipeline
    GstElement* live_queue = nullptr; // owned by live_bin
    GstElement* live_tee = nullptr;   // owned by pipeline
    GstElement* webrtc = nullptr;     // owned by pipeline; created per viewer
    GstPad* webrtc_tee_pad = nullptr; // request pad on live_tee, one ref held here (set once the viewer is negotiated)
    GstPad* webrtc_sink_pad = nullptr; // request pad on webrtc, one ref held here
    GstElement* webrtc_filter = nullptr; // capsfilter between the live tee and webrtc; owned by pipeline
    std::string cache_dir;
    guint bus_watch = 0;
    bool answer_sent = false;
    // Alarm episode (None -> any alarm -> None). Evidence is kept only if the episode reaches Red.
    std::filesystem::path evidence_dir; // empty when no episode is active
    std::string evidence_stamp;
    std::set<std::string> evidence_files; // segment names already copied
    bool evidence_saved = false;          // footage row already reported for this episode
    bool evidence_pending = false;        // Red seen: waiting for the segment holding the Red moment to be finished
    bool evidence_closing = false;        // the alarm returned to None while a clip was still pending
    std::string evidence_first;           // oldest segment that belongs to the episode (the pre-roll)
    gint64 red_wall_ms = 0;               // wall clock of the Red that is being saved
    gint64 last_red_ms = 0;
    guint evidence_timer = 0;
    std::atomic<gint64> rec_last_pts{0};  // last timestamp given to the recording branch
};

std::map<std::string, Camera> cameras;
GMainLoop* loop = nullptr;
std::string data_root = ".";     // cache/ and footage/ live under this folder

// Inference workers write events too: one lock keeps the lines whole.
std::mutex emit_mutex;

void emit(const json& message) {
    const std::string line = message.dump() + '\n';
    std::lock_guard<std::mutex> lock(emit_mutex);
    std::cout << line << std::flush;
}

// Detections go to the WebView, which must not be flooded: at most kMaxDetectionLines per second over all models.
constexpr std::size_t kMaxDetectionLines = 30;
std::mutex alarm_state_mutex;
std::string last_alarm_state = "None"; // for the "facts" diagnostic
void emit_inference_event(const json& message) {
    if (message.value("ev", std::string()) == "facts") {
        json with_state = message;
        std::lock_guard<std::mutex> lock(alarm_state_mutex);
        with_state["state"] = last_alarm_state;
        emit(with_state);
        return;
    }
    if (message.value("ev", std::string()) == "detections") {
        static std::mutex mutex;
        static std::deque<std::chrono::steady_clock::time_point> sent;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        while (!sent.empty() && now - sent.front() > std::chrono::seconds(1)) sent.pop_front();
        if (sent.size() >= kMaxDetectionLines) return;
        sent.push_back(now);
    }
    emit(message);
}

std::unique_ptr<rigmind::Inference> inference;
std::atomic<std::uint64_t> overlay_frames{0}, overlay_drawn{0}, overlay_us{0}; // burn-in counters (stats event)
std::atomic<bool> auto_facts{true};
std::string models_dir;

void emit_error(const std::string& cam, const std::string& text) {
    emit({{"ev", "error"}, {"cam", cam}, {"message", text}});
}

void emit_status(const Camera& cam) {
    json message = {{"ev", "status"}, {"cam", cam.id}, {"state", cam.state}, {"reason", cam.reason},
                    {"reconnects", cam.reconnects}, {"attempt", cam.retry_count}};
    if (cam.state == "online") {
        message["width"] = cam.width;
        message["height"] = cam.height;
        message["fps"] = cam.fps;
    }
    emit(message);
}

void set_state(Camera& cam, const std::string& state, const std::string& reason) {
    if (cam.state == state && cam.reason == reason) return;
    cam.state = state;
    cam.reason = reason;
    emit_status(cam);
}

void save_clip(const std::string& id);
void begin_evidence(Camera& cam);
void end_evidence(Camera& cam, bool force = false);

// ---- Live branch candidates -------------------------------------------------------------------------------------
// Decode on the GPU (d3d11h264dec): the camera stream has B-frames, which the software OpenH264 decoder cannot follow.
// Decode the camera stream, scale to 1280x720 (H.264 level 3.1, which every browser negotiates), cap at 30 fps,
// and encode with no B-frames and a keyframe every 30 frames. First candidate whose elements exist is used; if one
// fails at run time the next is tried on the following reconnect.
struct LiveBranch {
    const char* name;
    const char* decoder;
    const char* encoder;
    const char* description;
};

// The decoder runs for the whole session (the camera sends one keyframe, at the start, then deltas for a very long
// time), so it never loses its references. The gate sits after it, on raw frames, where dropping is harmless.
const char* const kLiveHead =
    "queue name=lq leaky=downstream max-size-buffers=0 max-size-bytes=0 max-size-time=6000000000 ! ";
const char* const kLiveTail = " ! h264parse name=lparse config-interval=-1 ! rtph264pay name=lpay config-interval=-1 pt=96 timestamp-offset=0";
// Frame rate of the live view and of the frames given to the models: "stream.output_fps" in inference.cfg (default 12.5).
// Frames are dropped to reach it, never duplicated. Set once in main() before the live branches are built.
double output_fps = 12.5;

std::string gpu_scale() {
    // Frame rate: the `pq` queue and the pacer probe on `rate` (on_pace_probe) cap the rate and even out the cadence. videorate
    // is not used: it works on buffer timestamps, and this camera stamps frames at a nominal 25 fps although it delivers fewer.
    const char* compliance = std::getenv("RIGMIND_DEC_COMPLIANCE");
    return std::string(
        std::string("d3d11h264dec") + (compliance ? std::string(" compliance=") + compliance : std::string()) + " ! d3d11convert ! video/x-raw(memory:D3D11Memory),width=1280,height=720 ! d3d11download ! "
        // The camera delivers in bursts: a short leaky queue (own thread) lets the decoder run on, and the pacer probe on
        // `rate` releases the newest frames one per interval (never duplicated) and stamps them with the release time.
        "queue name=pq leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! "
        "identity name=rate ! tee name=itee "
        // Detection tap, before the gate so it runs with or without a viewer: one buffer, always the newest frame.
        "itee. ! queue name=iq leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
        "videoconvert ! video/x-raw,format=RGB ! appsink name=isink "
        "itee. ! queue name=bq leaky=downstream max-size-buffers=6 max-size-bytes=0 max-size-time=0 ! "
        "identity name=gate ! videoconvert ! ") + (std::getenv("RIGMIND_NO_BURNIN") ? "" :
        // The detections are burned into this BGR frame (pad probe on `ovl`) just before it is encoded.
        "video/x-raw,format=BGR ! identity name=ovl ! videoconvert ! ") +
        "queue name=eq leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! ";
}

// Pacer on the decoded frames (see gpu_scale): at most output_fps frames per second, never duplicated, released at an even
// cadence from the queue thread. The camera delivers in bursts (several frames within milliseconds, then a gap); picking
// frames by arrival time, as the first version did, threw most of them away and left uneven gaps, and the browser's jitter
// buffer grew to about a second to absorb it. Released frames are stamped with the pipeline running time of the release, so
// the RTP timestamps describe the real send cadence and the browser needs almost no playout buffer.
void lat_record(int slot, double ms);
constexpr gint64 kMaxInferenceWaitUs = 300000;
std::atomic<int> pace_released{0}, pace_dropped{0}, pace_timeouts{0};
const bool inference_gate_off = std::getenv("RIGMIND_INFERENCE_GATE") && std::string(std::getenv("RIGMIND_INFERENCE_GATE")) == "0";

GstPadProbeReturn on_pace_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
    static gint64 last_us = 0;
    static gint64 last_pts = -1;
    auto* cam = static_cast<Camera*>(user);
    const gint64 interval = static_cast<gint64>(G_USEC_PER_SEC / output_fps);
    gint64 now = g_get_monotonic_time();
    static const bool arrival_mode = std::getenv("RIGMIND_RATE_MODE") && std::string(std::getenv("RIGMIND_RATE_MODE")) == "arrival";
    if (arrival_mode) { // the first version, kept for A/B measurements: drop frames that arrive too soon after the last kept one
        if (now - last_us < interval - 6000) return GST_PAD_PROBE_DROP;
        last_us = now;
        return GST_PAD_PROBE_OK;
    }
    if (const guint64 queued = GST_BUFFER_OFFSET(GST_PAD_PROBE_INFO_BUFFER(info)); queued > 1000000000ull && now > static_cast<gint64>(queued))
        lat_record(4, (now - static_cast<gint64>(queued)) / 1000.0); // time this frame waited in the pacing queue
    if (last_us && now - last_us < interval) {
        g_usleep(static_cast<gulong>(interval - (now - last_us)));
        now = g_get_monotonic_time();
    }
    // Inference-paced release: the next frame goes out only when the models have finished the previous one, so every frame
    // that is shown also reaches the models and its own detections can be drawn on it. (Models skip frames otherwise, and the
    // burn-in would draw results of an older frame.) Bounded: a stuck model delays the video by at most kMaxInferenceWaitUs.
    if (inference && !inference_gate_off) {
        const gint64 give_up = now + kMaxInferenceWaitUs;
        bool waited = false;
        while (!inference->ready_for_frame() && g_get_monotonic_time() < give_up) {
            g_usleep(2000);
            waited = true;
        }
        if (!inference->ready_for_frame()) ++pace_timeouts;
        now = g_get_monotonic_time();
        if (waited) {
            // The frame in hand is older than what arrived meanwhile: take the newest instead (drop this one).
            guint queued = 0;
            if (GstPad* peer = gst_pad_get_peer(pad)) {
                if (GstElement* q = gst_pad_get_parent_element(peer)) {
                    g_object_get(q, "current-level-buffers", &queued, nullptr);
                    gst_object_unref(q);
                }
                gst_object_unref(peer);
            }
            if (queued > 0) {
                ++pace_dropped;
                return GST_PAD_PROBE_DROP;
            }
        }
    }
    last_us = now;
    ++pace_released;
    if (!cam->pipeline) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    gint64 running = static_cast<gint64>(gst_element_get_current_running_time(cam->pipeline));
    if (last_pts >= 0 && running <= last_pts) running = last_pts + GST_MSECOND;
    last_pts = running;
    guint64 position = static_cast<guint64>(running);
    if (GstEvent* ev = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0)) {
        const GstSegment* seg = nullptr;
        gst_event_parse_segment(ev, &seg);
        if (seg && seg->format == GST_FORMAT_TIME) position = gst_segment_position_from_running_time(seg, GST_FORMAT_TIME, position);
        gst_event_unref(ev);
    }
    GST_BUFFER_PTS(buffer) = position;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

// An encoder element can exist without working (no GPU, missing driver). Run it for a few frames before trusting it.
GstPadProbeReturn count_probe_buffers(GstPad*, GstPadProbeInfo*, gpointer count) {
    ++*static_cast<int*>(count);
    return GST_PAD_PROBE_OK;
}

bool encoder_works(const char* encoder_description) {
    const std::string description =
        std::string("videotestsrc num-buffers=30 ! video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! ") +
        encoder_description + " ! h264parse ! fakesink name=probe_sink";
    GError* error = nullptr;
    GstElement* test = gst_parse_launch(description.c_str(), &error);
    if (!test) {
        g_clear_error(&error);
        return false;
    }
    g_clear_error(&error);
    int outputs = 0;
    GstElement* sink = gst_bin_get_by_name(GST_BIN(test), "probe_sink");
    GstPad* pad = sink ? gst_element_get_static_pad(sink, "sink") : nullptr;
    if (pad) gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe_buffers, &outputs, nullptr);
    GstBus* bus = gst_element_get_bus(test);
    gst_element_set_state(test, GST_STATE_PLAYING);
    GstMessage* msg = gst_bus_timed_pop_filtered(bus, 8 * GST_SECOND,
                                                 static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool eos = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg) gst_message_unref(msg);
    gst_element_set_state(test, GST_STATE_NULL); // streaming threads are stopped: `outputs` is final
    if (pad) gst_object_unref(pad);
    if (sink) gst_object_unref(sink);
    gst_object_unref(bus);
    gst_object_unref(test);
    return eos && outputs >= 10; // an encoder that fails inside (OpenH264 here) logs an error and emits nothing
}

std::vector<std::pair<std::string, std::string>> live_candidates() {
    struct Spec {
        const char* name;
        const char* decoder;
        const char* encoder;
        const char* scale;
        const char* encoder_description;
    };
    // Media Foundation first: it is part of Windows and falls back to Microsoft's software encoder without a GPU.
    const std::string scale = gpu_scale();
    const Spec specs[] = {
        {"mf", "d3d11h264dec", "mfh264enc", scale.c_str(), "mfh264enc name=enc bitrate=4000 gop-size=30 low-latency=true rc-mode=cbr"},
        {"qsv", "d3d11h264dec", "qsvh264enc", scale.c_str(), "qsvh264enc name=enc bitrate=4000 gop-size=30"},
        {"nvenc", "d3d11h264dec", "nvh264enc", scale.c_str(), "nvh264enc name=enc bitrate=4000 gop-size=30 bframes=0 zerolatency=true"},
        {"openh264", "d3d11h264dec", "openh264enc", scale.c_str(), "openh264enc name=enc bitrate=4000000 gop-size=30 rate-control=bitrate enable-frame-skip=false"},
    };

    std::vector<std::pair<std::string, std::string>> available;
    for (const Spec& spec : specs) {
        GstElementFactory* dec = gst_element_factory_find(spec.decoder);
        GstElementFactory* enc = gst_element_factory_find(spec.encoder);
        if (dec) gst_object_unref(dec);
        if (enc) gst_object_unref(enc);
        if (!dec || !enc) continue;
        if (!encoder_works(spec.encoder_description)) continue;
        available.emplace_back(spec.name, std::string(kLiveHead) + spec.scale + spec.encoder_description + kLiveTail);
    }
    return available;
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------------
void detach_peer(Camera& cam) {
    cam.live_enabled = false;
    if (!cam.webrtc) return;
    if (cam.webrtc_tee_pad) {
        GstPad* sink = gst_pad_get_peer(cam.webrtc_tee_pad);
        if (sink) {
            gst_pad_unlink(cam.webrtc_tee_pad, sink);
            gst_object_unref(sink);
        }
        gst_element_release_request_pad(cam.live_tee, cam.webrtc_tee_pad); // the tee keeps flowing: allow-not-linked
        gst_object_unref(cam.webrtc_tee_pad);
        cam.webrtc_tee_pad = nullptr;
    }
    if (cam.webrtc_filter) {
        gst_element_set_state(cam.webrtc_filter, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(cam.pipeline), cam.webrtc_filter);
        cam.webrtc_filter = nullptr;
    }
    if (cam.webrtc_sink_pad) {
        gst_object_unref(cam.webrtc_sink_pad);
        cam.webrtc_sink_pad = nullptr;
    }
    gst_element_set_state(cam.webrtc, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(cam.pipeline), cam.webrtc); // drops the bin's reference
    cam.webrtc = nullptr;
    cam.answer_sent = false;
}

void teardown_pipeline(Camera& cam) {
    if (!cam.pipeline) return;
    if (cam.bus_watch) {
        g_source_remove(cam.bus_watch);
        cam.bus_watch = 0;
    }
    if (cam.webrtc_tee_pad) {
        gst_object_unref(cam.webrtc_tee_pad);
        cam.webrtc_tee_pad = nullptr;
    }
    if (cam.webrtc_sink_pad) {
        gst_object_unref(cam.webrtc_sink_pad);
        cam.webrtc_sink_pad = nullptr;
    }
    cam.live_enabled = false;
    gst_element_set_state(cam.pipeline, GST_STATE_NULL);
    gst_object_unref(cam.pipeline);
    cam.pipeline = cam.webrtc = cam.live_tee = cam.live_bin = cam.live_queue = cam.webrtc_filter = nullptr;
    cam.answer_sent = false;
    end_evidence(cam, true); // an unfinished episode without Red is dropped; a pending or saved clip stays
}

void start_pipeline(Camera& cam);

struct Ref {
    std::string id;
    guint generation;
    guint serial;
    std::string reason;
    bool auth;
};

gboolean retry_cb(gpointer data) {
    std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    auto it = cameras.find(ref->id);
    if (it == cameras.end() || !it->second.want) return G_SOURCE_REMOVE;
    it->second.retry_source = 0;
    start_pipeline(it->second);
    return G_SOURCE_REMOVE;
}

void go_offline(Camera& cam, const std::string& reason, bool auth_failure = false) {
    if (!cam.want || !cam.pipeline) return; // already offline: a second error from the same pipeline is ignored
    teardown_pipeline(cam);
    ++cam.retry_count;
    set_state(cam, "offline", reason);
    const guint delay = auth_failure ? kRetryAuthMs
                                     : std::min(kRetryMaxMs, kRetryMinMs << std::min(cam.retry_count - 1, 3u));
    if (cam.retry_source) g_source_remove(cam.retry_source);
    cam.retry_source = g_timeout_add(delay, retry_cb, new Ref{cam.id, cam.generation, 0, "", false});
}

gboolean deferred_offline_cb(gpointer data) {
    std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    auto it = cameras.find(ref->id);
    if (it != cameras.end() && it->second.generation == ref->generation) go_offline(it->second, ref->reason, ref->auth);
    return G_SOURCE_REMOVE;
}

// Callbacks run on GStreamer threads or inside the pipeline's own bus: tear down later, from the main loop.
void defer_offline(const Camera& cam, std::string reason, bool auth = false) {
    g_idle_add(deferred_offline_cb, new Ref{cam.id, cam.generation, 0, std::move(reason), auth});
}

gboolean first_buffer_cb(gpointer data) {
    std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    auto it = cameras.find(ref->id);
    if (it == cameras.end() || it->second.generation != ref->generation || !it->second.pipeline) return G_SOURCE_REMOVE;
    Camera& cam = it->second;
    // Stream format from the parser's negotiated caps.
    if (GstElement* parse = gst_bin_get_by_name(GST_BIN(cam.pipeline), "parse")) {
        if (GstPad* pad = gst_element_get_static_pad(parse, "src")) {
            if (GstCaps* caps = gst_pad_get_current_caps(pad)) {
                const GstStructure* s = gst_caps_get_structure(caps, 0);
                int num = 0, den = 1;
                gst_structure_get_int(s, "width", &cam.width);
                gst_structure_get_int(s, "height", &cam.height);
                if (gst_structure_get_fraction(s, "framerate", &num, &den) && den > 0) cam.fps = static_cast<double>(num) / den;
                gst_caps_unref(caps);
            }
            gst_object_unref(pad);
        }
        gst_object_unref(parse);
    }
    if (cam.ever_online) ++cam.reconnects;
    cam.ever_online = true;
    cam.retry_count = 0;
    cam.stats_frames = cam.frames.load();
    cam.stats_bytes = cam.bytes.load();
    cam.stats_us = g_get_monotonic_time();
    set_state(cam, "online", "");
    return G_SOURCE_REMOVE;
}

gboolean peer_gone_cb(gpointer data) {
    std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    auto it = cameras.find(ref->id);
    if (it != cameras.end() && it->second.generation == ref->generation && it->second.peer_serial == ref->serial)
        detach_peer(it->second);
    return G_SOURCE_REMOVE;
}

void close_camera(const std::string& id) {
    auto it = cameras.find(id);
    if (it == cameras.end()) return;
    Camera& cam = it->second;
    cam.want = false;
    if (cam.retry_source) g_source_remove(cam.retry_source);
    teardown_pipeline(cam);
    cam.state = "offline";
    cam.reason = "closed";
    emit_status(cam);
    cameras.erase(it);
}

void emit_latency(const std::string& cam);

// One tick per second: stall watchdog, and a stats line every second tick.
gboolean on_tick(gpointer) {
    static unsigned ticks = 0;
    ++ticks;
    const gint64 now = g_get_monotonic_time();
    std::vector<std::string> ids;
    for (const auto& entry : cameras) ids.push_back(entry.first);
    for (const auto& id : ids) {
        auto it = cameras.find(id);
        if (it == cameras.end()) continue;
        Camera& cam = it->second;
        if (!cam.want || !cam.pipeline) continue;
        if (cam.state == "connecting" && now - cam.started_us > kConnectTimeoutUs) {
            go_offline(cam, "connection timed out");
        } else if (cam.state == "online" && now - cam.last_buffer_us.load() > kStallTimeoutUs) {
            go_offline(cam, "no video received");
        } else if (cam.state == "online" && ticks % 2 == 0) {
            const guint64 frames = cam.frames.load(), bytes = cam.bytes.load();
            const double seconds = std::max(0.001, static_cast<double>(now - cam.stats_us) / G_USEC_PER_SEC);
            emit({{"ev", "stats"}, {"cam", cam.id},
                  {"fps", static_cast<double>(frames - cam.stats_frames) / seconds},
                  {"kbps", static_cast<double>(bytes - cam.stats_bytes) * 8.0 / seconds / 1000.0},
                  {"reconnects", cam.reconnects}, {"resyncs", cam.resyncs.load()},
                  {"keyframes", cam.keyframes.load()}, {"live", cam.webrtc != nullptr},
                  {"gate_open", cam.live_enabled.load()},
                  {"overlay_frames", overlay_frames.load()}, {"overlay_drawn", overlay_drawn.load()},
                  {"overlay_avg_ms", overlay_frames.load() ? overlay_us.load() / 1000.0 / overlay_frames.load() : 0.0}});
            if (ticks % 4 == 0) emit_latency(cam.id);
            cam.stats_frames = frames;
            cam.stats_bytes = bytes;
            cam.stats_us = now;
            if (inference) {
                for (const auto& m : inference->take_stats()) {
                    if (!m.running) continue;
                    json event = {{"ev", "inference"}, {"cam", cam.id}, {"model", m.model}, {"fps", std::round(m.fps * 10) / 10},
                                  {"avg_ms", std::round(m.avg_ms * 10) / 10}, {"conf", std::round(m.conf * 1e4) / 1e4},
                                  {"iou", std::round(m.iou * 1e4) / 1e4}};
                    event["burned_in"] = true; // the detections are drawn into the video: the UI keeps its canvas hidden
                    event["provider"] = m.provider;
                    event["load_s"] = std::round(m.load_seconds * 10) / 10;
                    if (!m.provider_note.empty()) event["provider_note"] = m.provider_note.substr(0, 300);
                    if (!m.scale_mode.empty()) {
                        event["scale_mode"] = m.scale_mode;
                        event["scale"] = std::round(m.scale * 1e3) / 1e3;
                    }
                    emit(event);
                }
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

// ---- WebRTC -------------------------------------------------------------------------------------------------------
// Vanilla ICE: one answer message, written once local candidates are complete.
void maybe_send_answer(Camera* cam) {
    if (cam->answer_sent || !cam->webrtc) return;
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

void on_connection_state(GObject* obj, GParamSpec*, gpointer user) {
    auto* cam = static_cast<Camera*>(user);
    GstWebRTCPeerConnectionState state;
    GstWebRTCICEConnectionState ice_state;
    g_object_get(obj, "connection-state", &state, "ice-connection-state", &ice_state, nullptr);
    emit({{"ev", "peer"}, {"cam", cam->id}, {"connection", static_cast<int>(state)}, {"ice", static_cast<int>(ice_state)}});
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED || state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED ||
        state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED) {
        g_idle_add(peer_gone_cb, new Ref{cam->id, cam->generation, cam->peer_serial, "", false}); // viewer gone: stop live decode
    }
}

gboolean link_peer_cb(gpointer data);
void request_keyframe(Camera& cam);

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
    g_idle_add(link_peer_cb, new Ref{cam->id, cam->generation, cam->peer_serial, "", false});
    maybe_send_answer(cam);
}

// The local description is set, so webrtcbin now has its transport stream. Only now may video reach it: the caps
// event that arrives with the first link asserts otherwise. Runs on the main loop: link the viewer to the live tee
// and open the gate.
gboolean link_peer_cb(gpointer data) {
    std::unique_ptr<Ref> ref(static_cast<Ref*>(data));
    auto it = cameras.find(ref->id);
    if (it == cameras.end()) return G_SOURCE_REMOVE;
    Camera& cam = it->second;
    if (cam.generation != ref->generation || cam.peer_serial != ref->serial || !cam.webrtc || !cam.webrtc_tee_pad) {
        return G_SOURCE_REMOVE; // the viewer or the pipeline is already gone
    }
    // The browser chose the H.264 payload type in the answer (e.g. 102). The payloader must use that number.
    GstWebRTCSessionDescription* local = nullptr;
    g_object_get(cam.webrtc, "local-description", &local, nullptr);
    if (local) {
        for (guint m = 0; m < gst_sdp_message_medias_len(local->sdp); ++m) {
            const GstSDPMedia* media = gst_sdp_message_get_media(local->sdp, m);
            for (guint a = 0; a < gst_sdp_media_attributes_len(media); ++a) {
                const GstSDPAttribute* attr = gst_sdp_media_get_attribute(media, a);
                int pt = 0;
                if (g_strcmp0(attr->key, "rtpmap") == 0 && attr->value && std::string(attr->value).find("H264") != std::string::npos &&
                    sscanf(attr->value, "%d", &pt) == 1) {
                    if (GstElement* pay = gst_bin_get_by_name(GST_BIN(cam.live_bin), "lpay")) {
                        g_object_set(pay, "pt", static_cast<guint>(pt), nullptr);
                        gst_object_unref(pay);
                    }
                }
            }
        }
        gst_webrtc_session_description_free(local);
    }
    cam.live_enabled = true; // decoded frames now reach the encoder
    request_keyframe(cam);
    return G_SOURCE_REMOVE;
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

// A viewer: a fresh webrtcbin on the live tee. The camera, cache and EWS keep running whether or not anyone watches.
void attach_peer(Camera& cam, const std::string& sdp_text) {
    if (!cam.pipeline || !cam.live_tee) {
        emit_error(cam.id, "camera offline");
        return;
    }
    detach_peer(cam); // a new page load is a new browser peer
    cam.webrtc = gst_element_factory_make("webrtcbin", nullptr);
    if (!cam.webrtc) {
        emit_error(cam.id, "missing GStreamer plugin (webrtc)");
        return;
    }
    ++cam.peer_serial;
    g_object_set(cam.webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
    gst_bin_add(GST_BIN(cam.pipeline), cam.webrtc);
    gst_element_sync_state_with_parent(cam.webrtc);

    // No add-transceiver: the requested sink pad creates its own transceiver from the caps linked to it, so there is
    // exactly one (an extra, unused sendonly transceiver is what the browser's offer was matched to before).
    cam.webrtc_sink_pad = gst_element_request_pad_simple(cam.webrtc, "sink_%u");
    if (!cam.webrtc_sink_pad) {
        emit_error(cam.id, "cannot create webrtc sink");
        detach_peer(cam);
        return;
    }
    // Linked before the offer is applied, through a filter that states the media: webrtcbin uses what is linked to
    // its pad to pick the transceiver. No video flows yet: the raw-frame gate stays closed until the answer is set.
    cam.webrtc_filter = gst_element_factory_make("capsfilter", nullptr);
    GstCaps* media = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,clock-rate=90000");
    g_object_set(cam.webrtc_filter, "caps", media, nullptr);
    gst_caps_unref(media);
    gst_bin_add(GST_BIN(cam.pipeline), cam.webrtc_filter);
    gst_element_sync_state_with_parent(cam.webrtc_filter);
    GstPad* teepad = gst_element_request_pad_simple(cam.live_tee, "src_%u");
    GstPad* filter_in = gst_element_get_static_pad(cam.webrtc_filter, "sink");
    GstPad* filter_out = gst_element_get_static_pad(cam.webrtc_filter, "src");
    const bool linked = teepad && gst_pad_link(teepad, filter_in) == GST_PAD_LINK_OK &&
                        gst_pad_link(filter_out, cam.webrtc_sink_pad) == GST_PAD_LINK_OK;
    gst_object_unref(filter_in);
    gst_object_unref(filter_out);
    if (!linked) {
        if (teepad) {
            gst_element_release_request_pad(cam.live_tee, teepad);
            gst_object_unref(teepad);
        }
        emit_error(cam.id, "cannot link webrtc sink");
        detach_peer(cam);
        return;
    }
    cam.webrtc_tee_pad = teepad;
    g_signal_connect(cam.webrtc, "notify::ice-gathering-state", G_CALLBACK(on_gathering_state), &cam);
    g_signal_connect(cam.webrtc, "notify::connection-state", G_CALLBACK(on_connection_state), &cam);
    handle_offer(&cam, sdp_text);
}

// ---- Pipeline -----------------------------------------------------------------------------------------------------
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

gboolean on_bus(GstBus*, GstMessage* msg, gpointer user) {
    auto* cam = static_cast<Camera*>(user);
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &error, &debug);
        const std::string text = error && error->message ? error->message : "pipeline error";
        // A failure inside the live branch (a missing GPU, an unsupported encoder): try the next candidate on reconnect.
        if (cam->live_bin && GST_MESSAGE_SRC(msg) && gst_object_has_as_ancestor(GST_MESSAGE_SRC(msg), GST_OBJECT(cam->live_bin))) {
            ++cam->variant;
        }
        const bool auth = text.find("Unauthorized") != std::string::npos || text.find("401") != std::string::npos;
        defer_offline(*cam, auth ? "authentication failed: check the camera user and password" : text, auth);
        g_clear_error(&error);
        g_free(debug);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        defer_offline(*cam, "stream ended");
    }
    // rigmindews posts every EWS state change here. Forwarded at once; a red alarm also saves footage.
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_APPLICATION) {
        const GstStructure* s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "rigmind-alarm")) {
            guint64 frame = 0;
            gst_structure_get_uint64(s, "frame", &frame);
            const std::string state = gst_structure_get_string(s, "state");
            {
                std::lock_guard<std::mutex> lock(alarm_state_mutex);
                last_alarm_state = state;
            }
            emit({{"ev", "alarm"},
                  {"cam", gst_structure_get_string(s, "cam")},
                  {"state", state},
                  {"previous", gst_structure_get_string(s, "previous")},
                  {"severity", gst_structure_get_string(s, "severity")},
                  {"condition", gst_structure_get_string(s, "condition")},
                  {"place", gst_structure_get_string(s, "place")},
                  {"frame", frame}});
            // Evidence: starts at the first alarm state (Yellow), is kept on Red, and is dropped if the episode ends without Red.
            if (state == "None") end_evidence(*cam);
            else if (std::string(gst_structure_get_string(s, "previous")) == "None") begin_evidence(*cam);
            if (state == "Red") save_clip(cam->id);
        }
    }
    return TRUE;
}

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

// Facts derived from the IDSS detections (pipeline/ews_facts.h). A manual "facts" command with "auto":false switches
// this off so tests can drive the EWS by hand; "auto":true switches it back on.
std::atomic<bool> ews_zoom{false}; // IDSS zoomFlag: the EWS counters follow it, like the fact thresholds (ews.cpp setThresholds)
void on_derived_facts(const rigmind::FrameFacts& facts, bool zoomed) {
    if (!auto_facts.load()) return;
    ews_zoom.store(zoomed, std::memory_order_relaxed);
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < sizeof(kFactFields) / sizeof(kFactFields[0]); ++i) {
        if (facts.*(kFactFields[i].member)) bits |= 1u << i;
    }
    ews_facts.store(bits, std::memory_order_relaxed);
}

// Attaches the current facts to every frame that reaches the EWS branch. The tee shares buffers between
// branches, so the frame is made writable first; that copies only if another branch still holds it.
GstPadProbeReturn on_facts_probe(GstPad*, GstPadProbeInfo* info, gpointer ews) {
    if (auto_facts.load()) g_object_set(G_OBJECT(ews), "zoom", ews_zoom.load(std::memory_order_relaxed) ? TRUE : FALSE, nullptr);
    GstBuffer* buffer = gst_buffer_make_writable(gst_pad_probe_info_get_buffer(info));
    rigmind_facts_meta_add(buffer, facts_from_bits(ews_facts.load(std::memory_order_relaxed)));
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

// The recording branch needs timestamps: this camera sends the same RTP timestamp on every frame, so the buffers reach
// splitmuxsink without PTS/DTS and it never reaches its 10 s split point (one endless, unfinished file). Frames get their
// arrival time on the pipeline clock (strictly increasing). Only this branch's buffer is changed; live and EWS see the original.
GstPadProbeReturn on_rec_stamp(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* cam = static_cast<Camera*>(user);
    if (!cam->pipeline) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    gint64 t = static_cast<gint64>(gst_element_get_current_running_time(cam->pipeline));
    const gint64 last = cam->rec_last_pts.load(std::memory_order_relaxed);
    if (t <= last) t = last + GST_MSECOND;
    cam->rec_last_pts.store(t, std::memory_order_relaxed);
    GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(t);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

// Every frame from the camera: feeds the stall watchdog and the stats; the first one marks the camera online.
GstPadProbeReturn on_source_probe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* cam = static_cast<Camera*>(user);
    cam->frames.fetch_add(1, std::memory_order_relaxed);
    cam->bytes.fetch_add(gst_buffer_get_size(GST_PAD_PROBE_INFO_BUFFER(info)), std::memory_order_relaxed);
    cam->last_buffer_us.store(g_get_monotonic_time(), std::memory_order_relaxed);
    if (!GST_BUFFER_FLAG_IS_SET(GST_PAD_PROBE_INFO_BUFFER(info), GST_BUFFER_FLAG_DELTA_UNIT)) {
        cam->keyframes.fetch_add(1, std::memory_order_relaxed);
        cam->last_key_us.store(g_get_monotonic_time(), std::memory_order_relaxed);
    }
    if (!cam->got_buffer.exchange(true)) g_idle_add(first_buffer_cb, new Ref{cam->id, cam->generation, 0, "", false});
    return GST_PAD_PROBE_OK;
}

// Gate on decoded frames. Frames enter the encoder only while a viewer is attached. Dropping a raw frame cannot
// corrupt anything: the encoder makes its own references. (Dropping encoded frames would.)
GstPadProbeReturn on_live_gate(GstPad*, GstPadProbeInfo*, gpointer user) {
    auto* cam = static_cast<Camera*>(user);
    return cam->live_enabled.load(std::memory_order_relaxed) ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
}

// The live queue overran: the decoder fell behind. Counted so it shows up in the stats.
void on_live_overrun(GstElement*, gpointer user) {
    static_cast<Camera*>(user)->resyncs.fetch_add(1, std::memory_order_relaxed);
}

// Asks the encoder for a keyframe now, so a new viewer starts at once instead of waiting a whole GOP.
void request_keyframe(Camera& cam) {
    if (!cam.live_bin) return;
    GstElement* enc = gst_bin_get_by_name(GST_BIN(cam.live_bin), "enc");
    if (!enc) return;
    if (GstPad* src = gst_element_get_static_pad(enc, "src")) {
        GstStructure* s = gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr);
        gst_pad_send_event(src, gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s));
        gst_object_unref(src);
    }
    gst_object_unref(enc);
}

// A decoded RGB frame from the detection tap (GStreamer streaming thread): copied out and handed to the models.
GstFlowReturn on_inference_sample(GstAppSink* sink, gpointer user) {
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    if (inference) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        const GstStructure* s = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
        int width = 0, height = 0;
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);
        GstMapInfo map;
        // RGB rows are 3 * width bytes; 1280 and 720 leave no padding. Any other size is skipped rather than guessed.
        if (width > 0 && height > 0 && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            if (map.size == static_cast<gsize>(width) * height * 3) {
                auto frame = std::make_shared<rigmind::Frame>();
                frame->cam = static_cast<Camera*>(user)->id;
                frame->pts_key = GST_BUFFER_PTS_IS_VALID(buffer) ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer)) : -1;
                frame->ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                GstBuffer* buf = buffer;
                if (GST_BUFFER_PTS_IS_VALID(buf)) {
                    const GstSegment* segment = gst_sample_get_segment(sample);
                    const guint64 running = segment ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buf)) : GST_BUFFER_PTS(buf);
                    if (running != GST_CLOCK_TIME_NONE) {
                        frame->pts_ms = static_cast<std::int64_t>(running / GST_MSECOND);
                        // The payloader runs with timestamp-offset=0: RTP timestamp = running time at 90 kHz, mod 2^32. The page matches
                        // it with requestVideoFrameCallback().rtpTimestamp to draw this frame's detections on exactly that frame.
                        frame->rtp = static_cast<std::int64_t>(gst_util_uint64_scale(running, 90000, GST_SECOND) & 0xFFFFFFFFull);
                    }
                }
                frame->width = width;
                frame->height = height;
                frame->rgb.assign(map.data, map.data + map.size);
                inference->push_frame(std::move(frame));
            }
            gst_buffer_unmap(buffer, &map);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Burn-in: runs on the streaming thread of the live branch, on the BGR frame just before the encoder. Waits (bounded, 250 ms by default:
// after the frame reached the detection tap) for the models to finish this very frame, then draws on it. A frame the
// models could not finish in time goes through unmodified, or with the previous result if that is under 400 ms old; the
// video is never held longer than the budget.
// RIGMIND_OVERLAY_BUDGET_MS overrides it (0 = never wait: draw the newest earlier result).
std::chrono::milliseconds overlay_budget() {
    static const std::chrono::milliseconds budget(std::getenv("RIGMIND_OVERLAY_BUDGET_MS") ? std::atoi(std::getenv("RIGMIND_OVERLAY_BUDGET_MS")) : 250);
    return budget;
}

GstPadProbeReturn on_overlay_probe(GstPad*, GstPadProbeInfo* info, gpointer) {
    if (!inference) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!GST_BUFFER_PTS_IS_VALID(buffer)) return GST_PAD_PROBE_OK;
    const auto started = std::chrono::steady_clock::now();
    const std::vector<rigmind::DrawResult> results = inference->collect(static_cast<std::int64_t>(GST_BUFFER_PTS(buffer)), overlay_budget());
    ++overlay_frames;
    if (!results.empty()) {
        buffer = gst_buffer_make_writable(buffer);
        GST_PAD_PROBE_INFO_DATA(info) = buffer;
        GstMapInfo map;
        // The caps are BGR 1280x720 (rows of 3 * width bytes, no padding at this width).
        if (gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) {
            if (map.size == static_cast<gsize>(1280) * 720 * 3) {
                cv::Mat frame(720, 1280, CV_8UC3, map.data);
                rigmind::draw_overlay(frame, results);
                ++overlay_drawn;
            }
            gst_buffer_unmap(buffer, &map);
        }
    }
    overlay_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    return GST_PAD_PROBE_OK;
}

// ---- Latency diagnostics ------------------------------------------------------------------------------------------
// Cheap per-frame stage timestamps keyed by the buffer PTS, reported as a "latency" event every 4 s (p50/p95/max in ms):
// 0 live queue in, 1 decoded+scaled (rate), 2 before burn-in, 3 after burn-in, 4 encoder out, 5 payloader out.
struct LatDiag {
    std::mutex mutex;
    std::map<gint64, std::array<gint64, 6>> frames;
    std::deque<gint64> src_fifo;
    std::deque<gint64> enc_fifo; // stage-3 times; the encoder rewrites the PTS, so frames are matched in order
    std::vector<double> d[6];       // 0..4: stage k -> k+1, 5: total 0 -> 5
    std::vector<double> in_gap, out_gap;
    gint64 last_in = 0, last_out = 0;
    guint64 in_n = 0, out_n = 0, matched = 0, rate_n = 0;
};
LatDiag lat_diag;

GstPadProbeReturn on_lat_probe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    const int stage = GPOINTER_TO_INT(user);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;
    const gint64 now = g_get_monotonic_time();
    const gint64 key = GST_BUFFER_PTS_IS_VALID(buffer) ? static_cast<gint64>(GST_BUFFER_PTS(buffer)) : -1;
    std::lock_guard<std::mutex> lock(lat_diag.mutex);
    LatDiag& L = lat_diag;
    if (stage == 0) { // source frame entering the live branch (before decode)
        ++L.in_n;
        if (L.last_in) L.in_gap.push_back((now - L.last_in) / 1000.0);
        L.last_in = now;
        L.src_fifo.push_back(now); // the decoder emits in order, so decode latency is matched first in, first out
        if (L.src_fifo.size() > 12) L.src_fifo.pop_front();
        return GST_PAD_PROBE_OK;
    }
    if (stage == 6) { // entering the pacing queue: decode latency, and the entry time travels in the buffer offset
        if (!L.src_fifo.empty()) {
            L.d[0].push_back((now - L.src_fifo.front()) / 1000.0);
            L.src_fifo.pop_front();
        }
        GstBuffer* w = gst_buffer_make_writable(buffer);
        GST_BUFFER_OFFSET(w) = static_cast<guint64>(now);
        GST_PAD_PROBE_INFO_DATA(info) = w;
        return GST_PAD_PROBE_OK;
    }
    if (stage == 1) { // decoded, scaled and downloaded: the frame is identified by this PTS from here on
        ++L.rate_n;
        std::array<gint64, 6> t{};
        t[1] = now;
        L.frames[key] = t;
        while (L.frames.size() > 96) L.frames.erase(L.frames.begin());
        return GST_PAD_PROBE_OK;
    }
    auto it = L.frames.find(key);
    if (it != L.frames.end()) it->second[stage] = now;
    if (stage == 3) {
        L.enc_fifo.push_back(now);
        if (L.enc_fifo.size() > 8) L.enc_fifo.pop_front();
        if (it != L.frames.end()) {
            const auto& t = it->second;
            ++L.matched;
            L.d[1].push_back((t[2] - t[1]) / 1000.0);
            L.d[2].push_back((t[3] - t[2]) / 1000.0);
            L.d[5].push_back((t[3] - t[1]) / 1000.0);
            L.frames.erase(it);
        }
    }
    if (stage == 4) { // encoder output: the cadence the browser gets (the payloader splits a frame into packets)
        ++L.out_n;
        if (L.last_out) L.out_gap.push_back((now - L.last_out) / 1000.0);
        L.last_out = now;
        if (!L.enc_fifo.empty()) {
            L.d[3].push_back((now - L.enc_fifo.front()) / 1000.0);
            L.enc_fifo.pop_front();
        }
    }
    return GST_PAD_PROBE_OK;
}

void lat_record(int slot, double ms) {
    std::lock_guard<std::mutex> lock(lat_diag.mutex);
    lat_diag.d[slot].push_back(ms);
}

json lat_stat(std::vector<double>& v) {
    if (v.empty()) return nullptr;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return std::round(v[static_cast<size_t>(p * (v.size() - 1))] * 10) / 10; };
    return {{"p50", pct(0.5)}, {"p95", pct(0.95)}, {"max", std::round(v.back() * 10) / 10}, {"n", v.size()}};
}

void emit_latency(const std::string& cam) {
    json e = {{"ev", "latency"}, {"cam", cam}};
    {
        std::lock_guard<std::mutex> lock(lat_diag.mutex);
        LatDiag& L = lat_diag;
        static const char* names[6] = {"decode_scale", "to_overlay", "overlay_wait", "convert_encode", "pace_wait", "pace_to_overlay_out"};
        for (int k = 0; k < 6; ++k) e[names[k]] = lat_stat(L.d[k]);
        e["src_gap"] = lat_stat(L.in_gap);
        e["out_gap"] = lat_stat(L.out_gap);
        if (inference) {
            const json sync = inference->take_sync();
            if (!sync.empty()) e["sync"] = sync;
        }
        e["pace"] = {{"released", pace_released.exchange(0)}, {"dropped_newer_waiting", pace_dropped.exchange(0)}, {"inference_timeouts", pace_timeouts.exchange(0)}};
        e["in"] = L.in_n; e["decoded"] = L.rate_n; e["out"] = L.out_n; e["matched"] = L.matched;
        for (auto& v : L.d) v.clear();
        L.in_gap.clear(); L.out_gap.clear();
        L.in_n = L.out_n = L.matched = L.rate_n = 0;
    }
    emit(e);
}

std::vector<std::pair<std::string, std::string>> live_branches;

void start_pipeline(Camera& cam) {
    namespace fs = std::filesystem;
    cam.cache_dir = (fs::path(data_root) / "cache" / cam.id).string();
    std::error_code ec;
    fs::create_directories(cam.cache_dir, ec);
    if (ec) {
        // Not retried quickly: a read-only data folder will not fix itself.
        set_state(cam, "offline", "cannot write to " + cam.cache_dir + ": " + ec.message());
        cam.retry_source = g_timeout_add(kRetryAuthMs, retry_cb, new Ref{cam.id, cam.generation, 0, "", false});
        return;
    }
    // The rolling cache starts empty: leftovers of an earlier run would look like the newest segments.
    for (const auto& entry : fs::directory_iterator(cam.cache_dir, ec)) {
        if (entry.path().extension() == ".mp4") fs::remove(entry.path(), ec);
    }
    if (live_branches.empty()) {
        set_state(cam, "offline", "no H.264 decoder/encoder available for the live view");
        return;
    }

    ++cam.generation;
    cam.got_buffer = false;
    cam.started_us = g_get_monotonic_time();
    cam.last_buffer_us = cam.started_us;
    cam.width = cam.height = 0;
    cam.fps = 0;

    const auto& branch = live_branches[cam.variant % live_branches.size()];
    GError* parse_error = nullptr;
    GstElement* live_bin = gst_parse_bin_from_description(branch.second.c_str(), TRUE, &parse_error);
    if (!live_bin) {
        const std::string text = parse_error ? parse_error->message : "cannot build the live branch";
        g_clear_error(&parse_error);
        ++cam.variant;
        set_state(cam, "offline", "live branch: " + text);
        ++cam.retry_count;
        cam.retry_source = g_timeout_add(kRetryMinMs, retry_cb, new Ref{cam.id, cam.generation, 0, "", false});
        return;
    }
    g_clear_error(&parse_error);

    cam.pipeline = gst_pipeline_new(cam.id.c_str());
    GstElement* src = gst_element_factory_make("rtspsrc", "src");
    GstElement* depay = gst_element_factory_make("rtph264depay", "depay");
    GstElement* parse = gst_element_factory_make("h264parse", "parse");
    GstElement* tee = gst_element_factory_make("tee", "tee");
    GstElement* rec_queue = gst_element_factory_make("queue", "rec_queue");
    GstElement* rec_sink = gst_element_factory_make("splitmuxsink", "rec_sink");
    GstElement* alarm_queue = gst_element_factory_make("queue", "alarm_queue");
    GstElement* ews = gst_element_factory_make("rigmindews", "ews");
    GstElement* ews_sink = gst_element_factory_make("fakesink", "ews_sink");
    GstElement* live_tee = gst_element_factory_make("tee", "live_tee");
    if (!src || !depay || !parse || !tee || !rec_queue || !rec_sink || !alarm_queue || !ews || !ews_sink || !live_tee) {
        gst_object_unref(live_bin);
        gst_object_unref(cam.pipeline);
        cam.pipeline = nullptr;
        set_state(cam, "offline", "missing GStreamer plugin (rtsp, rtp, splitmuxsink, rigmindews)");
        cam.retry_source = g_timeout_add(kRetryAuthMs, retry_cb, new Ref{cam.id, cam.generation, 0, "", false});
        return;
    }

    // TCP interleaved: no UDP packet loss, which shows up as smeared blocks in H.264. A little jitter buffer on top.
    gst_util_set_object_arg(G_OBJECT(src), "protocols", "tcp");
    static const guint rtsp_latency = std::getenv("RIGMIND_RTSP_LATENCY") ? static_cast<guint>(std::atoi(std::getenv("RIGMIND_RTSP_LATENCY"))) : 200u;
    g_object_set(src, "location", cam.uri.c_str(), "latency", rtsp_latency, "drop-on-latency", FALSE,
                 "tcp-timeout", static_cast<guint64>(10 * G_USEC_PER_SEC), nullptr);
    if (!cam.user.empty()) g_object_set(src, "user-id", cam.user.c_str(), "user-pw", cam.pass.c_str(), nullptr);
    g_object_set(parse, "config-interval", -1, nullptr);
    g_object_set(live_tee, "allow-not-linked", TRUE, nullptr);
    // Rolling cache: 10 s segments, newest 12 kept (2 minutes). Old segments are replaced automatically.
    const std::string location = (fs::path(cam.cache_dir) / "seg%05d.mp4").string();
    g_object_set(rec_sink, "location", location.c_str(), "max-size-time", static_cast<guint64>(10 * GST_SECOND),
                 "max-files", static_cast<guint>(12), "muxer-factory", "mp4mux", nullptr);
    g_signal_connect(src, "pad-added", G_CALLBACK(on_pad_added), depay);
    // Alarm branch: small leaky queue, so the EWS check never delays the live or cache branches.
    g_object_set(alarm_queue, "leaky", 2 /* downstream */, "max-size-buffers", static_cast<guint>(8),
                 "max-size-bytes", static_cast<guint>(0), "max-size-time", static_cast<guint64>(0), nullptr);
    g_object_set(ews, "cam-id", cam.id.c_str(), nullptr);
    g_object_set(ews_sink, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(cam.pipeline), src, depay, parse, tee, rec_queue, rec_sink, alarm_queue, ews, ews_sink,
                     live_bin, live_tee, nullptr);
    cam.live_bin = live_bin;
    cam.live_tee = live_tee;
    cam.live_queue = gst_bin_get_by_name(GST_BIN(live_bin), "lq");
    if (cam.live_queue) gst_object_unref(cam.live_queue); // the bin owns it; the pointer is only used while the bin lives

    if (!gst_element_link_many(depay, parse, tee, nullptr) ||
        !gst_element_link_many(rec_queue, rec_sink, nullptr) ||
        !gst_element_link_many(alarm_queue, ews, ews_sink, nullptr) ||
        !gst_element_link(tee, rec_queue) || !gst_element_link(tee, alarm_queue) ||
        !gst_element_link(tee, live_bin) || !gst_element_link(live_bin, live_tee)) {
        emit_error(cam.id, "cannot link pipeline");
        teardown_pipeline(cam);
        set_state(cam, "offline", "cannot link pipeline");
        cam.retry_source = g_timeout_add(kRetryAuthMs, retry_cb, new Ref{cam.id, cam.generation, 0, "", false});
        return;
    }

    GstPad* alarm_in = gst_element_get_static_pad(alarm_queue, "sink");
    gst_pad_add_probe(alarm_in, GST_PAD_PROBE_TYPE_BUFFER, on_facts_probe, ews, nullptr);
    gst_object_unref(alarm_in);
    cam.rec_last_pts = 0;
    GstPad* rec_in = gst_element_get_static_pad(rec_queue, "sink");
    gst_pad_add_probe(rec_in, GST_PAD_PROBE_TYPE_BUFFER, on_rec_stamp, &cam, nullptr);
    gst_object_unref(rec_in);
    GstPad* tee_in = gst_element_get_static_pad(tee, "sink");
    gst_pad_add_probe(tee_in, GST_PAD_PROBE_TYPE_BUFFER, on_source_probe, &cam, nullptr);
    gst_object_unref(tee_in);
    if (GstElement* gate = gst_bin_get_by_name(GST_BIN(live_bin), "gate")) {
        if (GstPad* gate_in = gst_element_get_static_pad(gate, "sink")) {
            gst_pad_add_probe(gate_in, GST_PAD_PROBE_TYPE_BUFFER, on_live_gate, &cam, nullptr);
            gst_object_unref(gate_in);
        }
        gst_object_unref(gate);
    }
    if (GstElement* rate = gst_bin_get_by_name(GST_BIN(live_bin), "rate")) {
        if (GstPad* pad = gst_element_get_static_pad(rate, "sink")) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_pace_probe, &cam, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(rate);
    }
    {
        struct P { const char* el; const char* pad; int stage; };
        for (const P& q : {P{"lq", "sink", 0}, P{"pq", "sink", 6}, P{"rate", "src", 1}, P{"ovl", "sink", 2}, P{"ovl", "src", 3}, P{"enc", "src", 4}}) {
            if (GstElement* el = gst_bin_get_by_name(GST_BIN(live_bin), q.el)) {
                if (GstPad* pad = gst_element_get_static_pad(el, q.pad)) {
                    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_lat_probe, GINT_TO_POINTER(q.stage), nullptr);
                    gst_object_unref(pad);
                }
                gst_object_unref(el);
            }
        }
    }
    if (GstElement* ovl = gst_bin_get_by_name(GST_BIN(live_bin), "ovl")) {
        if (GstPad* pad = gst_element_get_static_pad(ovl, "sink")) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_overlay_probe, nullptr, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(ovl);
    }
    if (GstElement* isink = gst_bin_get_by_name(GST_BIN(live_bin), "isink")) {
        GstAppSinkCallbacks callbacks{};
        callbacks.new_sample = on_inference_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(isink), &callbacks, &cam, nullptr);
        g_object_set(isink, "drop", TRUE, "max-buffers", 1u, "sync", FALSE, "async", FALSE, nullptr);
        gst_object_unref(isink);
    }
    if (cam.live_queue) g_signal_connect(cam.live_queue, "overrun", G_CALLBACK(on_live_overrun), &cam);

    GstBus* bus = gst_element_get_bus(cam.pipeline);
    cam.bus_watch = gst_bus_add_watch(bus, on_bus, &cam);
    gst_object_unref(bus);

    set_state(cam, "connecting", "");
    gst_element_set_state(cam.pipeline, GST_STATE_PLAYING);
}

void open_camera(const std::string& id, const std::string& uri, const std::string& user, const std::string& pass) {
    if (uri.rfind("rtsp://", 0) != 0) {
        emit_error(id, "invalid uri");
        return;
    }
    if (cameras.find(id) == cameras.end() && cameras.size() >= kMaxCameras) {
        emit_error(id, "camera limit reached");
        return;
    }
    Camera& cam = cameras[id];
    cam.id = id;
    // The page re-sends open on every visit. Same settings: leave the camera running (it keeps recording).
    if (cam.want && cam.uri == uri && cam.user == user && cam.pass == pass) {
        emit_status(cam);
        return;
    }
    if (cam.want) {
        if (cam.retry_source) g_source_remove(cam.retry_source);
        cam.retry_source = 0;
        teardown_pipeline(cam);
    }
    cam.uri = uri;
    cam.user = user;
    cam.pass = pass;
    cam.want = true;
    cam.retry_count = 0;
    cam.ever_online = false;
    cam.reconnects = 0;
    start_pipeline(cam);
}

// Segments sort chronologically by number (seg00000.mp4 ...); length first so a number past 99999 still sorts last.
bool segment_before(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::string x = a.filename().string(), y = b.filename().string();
    return x.size() != y.size() ? x.size() < y.size() : x < y;
}

// Completed cache segments, oldest first. The newest file is still being written, so it is left out.
std::vector<std::filesystem::path> completed_segments(const std::string& cache_dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> segments;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.path().extension() == ".mp4") segments.push_back(entry.path());
    }
    std::sort(segments.begin(), segments.end(), segment_before);
    if (!segments.empty()) segments.pop_back();
    return segments;
}

gint64 wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Copies every completed segment of the episode (from the pre-roll on) that is not in the folder yet.
void copy_new_segments(Camera& cam) {
    namespace fs = std::filesystem;
    for (const auto& segment : completed_segments(cam.cache_dir)) {
        const std::string name = segment.filename().string();
        if (cam.evidence_files.count(name)) continue;
        if (!cam.evidence_first.empty() && segment_before(segment, fs::path(cam.evidence_first))) continue;
        std::error_code ec;
        const auto size = fs::file_size(segment, ec);
        if (ec || size == 0) continue;
        fs::copy_file(segment, cam.evidence_dir / name, fs::copy_options::overwrite_existing, ec);
        if (!ec) cam.evidence_files.insert(name);
    }
}

// True when a segment was finished after the Red moment, i.e. the segment that holds the Red frame is complete.
bool red_segment_finished(const Camera& cam) {
    namespace fs = std::filesystem;
    const gint64 since_red_ms = wall_ms() - cam.red_wall_ms;
    for (const auto& segment : completed_segments(cam.cache_dir)) {
        std::error_code ec;
        const auto written = fs::last_write_time(segment, ec);
        if (ec) continue;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::now() - written).count();
        if (age <= since_red_ms) return true;
    }
    return false;
}

void clear_evidence(Camera& cam) {
    if (cam.evidence_timer) g_source_remove(cam.evidence_timer);
    cam.evidence_timer = 0;
    cam.evidence_dir.clear();
    cam.evidence_stamp.clear();
    cam.evidence_first.clear();
    cam.evidence_files.clear();
    cam.evidence_saved = cam.evidence_pending = cam.evidence_closing = false;
}

// The Red clip is complete (or as complete as it will get): the footage row is reported once.
void finish_clip(Camera& cam) {
    copy_new_segments(cam);
    cam.evidence_pending = false;
    if (cam.evidence_files.empty()) {
        emit_error(cam.id, "alarm footage: no recorded segment to save");
        std::error_code ec;
        std::filesystem::remove_all(cam.evidence_dir, ec);
        return;
    }
    cam.evidence_saved = true;
    emit({{"ev", "clip"}, {"cam", cam.id}, {"stamp", cam.evidence_stamp}, {"count", cam.evidence_files.size()}});
}

// Once a second while an episode runs: keeps copying finished segments until the Red clip is complete.
gboolean evidence_tick(gpointer data) {
    const auto* ref = static_cast<Ref*>(data); // freed by the source's destroy notify
    auto it = cameras.find(ref->id);
    if (it == cameras.end() || it->second.evidence_dir.empty() || !it->second.evidence_timer) return G_SOURCE_REMOVE;
    Camera& cam = it->second;
    copy_new_segments(cam);
    if (cam.evidence_pending && (red_segment_finished(cam) || wall_ms() - cam.red_wall_ms > 45000)) {
        finish_clip(cam);
        cam.evidence_timer = 0; // this source returns G_SOURCE_REMOVE: the clip is complete, no more segments are added
        if (cam.evidence_closing || cam.evidence_files.empty()) clear_evidence(cam);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// Yellow onset: the episode folder is created. Pre-roll = the newest finished segment; later segments are added as they finish.
void begin_evidence(Camera& cam) {
    if (!cam.evidence_dir.empty()) return;
    cam.evidence_stamp = std::to_string(wall_ms());
    cam.evidence_dir = std::filesystem::path(data_root) / "footage" / cam.id / cam.evidence_stamp;
    std::filesystem::create_directories(cam.evidence_dir);
    const auto done = completed_segments(cam.cache_dir);
    cam.evidence_first = done.empty() ? std::string() : done.back().filename().string();
    copy_new_segments(cam);
    cam.evidence_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, evidence_tick, new Ref{cam.id, cam.generation, 0, "", false},
                                            [](gpointer p) { delete static_cast<Ref*>(p); });
    emit({{"ev", "evidence"}, {"cam", cam.id}, {"stamp", cam.evidence_stamp}, {"phase", "started"}});
}

// Red: the clip is finished after the 10 s segment holding the Red moment is complete (a post-roll of up to one
// segment), then reported once. Another Red in the same episode within 20 s is covered by the same clip.
void save_clip(const std::string& id) {
    auto it = cameras.find(id);
    if (it == cameras.end()) {
        emit_error(id, "camera not open");
        return;
    }
    Camera& cam = it->second;
    const gint64 now = wall_ms();
    if (cam.evidence_saved && now - cam.last_red_ms > 20000) clear_evidence(cam); // a new Red: a new clip
    begin_evidence(cam);
    if (cam.evidence_pending || cam.evidence_saved) return;
    cam.evidence_pending = true;
    cam.red_wall_ms = cam.last_red_ms = now;
}

// Episode over. Without Red nothing is kept; a pending Red clip is finished first (force: now, with what exists).
void end_evidence(Camera& cam, bool force) {
    if (cam.evidence_pending) {
        if (!force) {
            cam.evidence_closing = true; // the tick finishes the clip, then clears the episode
            return;
        }
        finish_clip(cam);
    }
    if (!cam.evidence_saved && !cam.evidence_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(cam.evidence_dir, ec);
    }
    clear_evidence(cam);
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
        open_camera(cam, command.value("uri", std::string()), command.value("user", std::string()),
                    command.value("pass", std::string()));
    } else if (cmd == "offer") {
        auto it = cameras.find(cam);
        if (it == cameras.end()) emit_error(cam, "camera not open");
        else attach_peer(it->second, command.value("sdp", std::string()));
    } else if (cmd == "detach") {
        auto it = cameras.find(cam);
        if (it != cameras.end()) detach_peer(it->second);
    } else if (cmd == "status") {
        for (const auto& entry : cameras) emit_status(entry.second);
    } else if (cmd == "close") {
        close_camera(cam);
    } else if (cmd == "clip") {
        save_clip(cam);
    } else if (cmd == "overlay") {
        // {"cmd":"overlay","cam":"CAM-01","ppe":true,"idss":true}: which models are burned into the video (default both).
        if (inference) inference->set_overlay(command.value("ppe", true), command.value("idss", true));
    } else if (cmd == "facts") {
        // {"cmd":"facts","cam":"CAM-01","machine_present":true,...}: unknown keys are ignored, missing keys are false.
        // "auto":false keeps the detections from overwriting these; "auto":true hands control back to them.
        if (command.contains("auto")) auto_facts = command.value("auto", true);
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
#ifdef _WIN32
    // The NVIDIA runtime (TensorRT, CUDA, cuDNN) sits in <exe>\gpu; the ONNX Runtime GPU providers find their DLLs through PATH.
    // RIGMIND_GPU_DIR overrides the folder for development runs. Without it inference runs on the CPU.
    {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char* override_dir = std::getenv("RIGMIND_GPU_DIR");
        std::filesystem::path gpu = override_dir ? std::filesystem::path(override_dir) : std::filesystem::path(exe).parent_path() / "gpu";
        if (std::filesystem::exists(gpu)) {
            const char* old_path = std::getenv("PATH");
            SetEnvironmentVariableA("PATH", (gpu.make_preferred().string() + ";" + (old_path ? old_path : "")).c_str());
        }
    }
#endif
    gst_init(&argc, &argv);
    if (argc > 1) data_root = argv[1];
    // argv[2]: path to the rigmindews plugin module. Without it, the GStreamer plugin registry must already know it.
    if (argc > 2 && !gst_plugin_load_file(argv[2], nullptr)) {
        emit({{"ev", "error"}, {"message", std::string("cannot load plugin ") + argv[2]}});
    }
    if (argc > 3) models_dir = argv[3];
    if (!models_dir.empty()) {
        try {
            const json cfg = rigmind::load_inference_cfg(std::filesystem::path(models_dir) / "inference.cfg");
            const double fps = cfg.value("stream", json::object()).value("output_fps", 12.5);
            if (fps >= 1 && fps <= 30) output_fps = fps;
        } catch (const std::exception&) {
            // Unreadable settings: the default stands; the inference workers report the same problem.
        }
    }
    live_branches = live_candidates();
    emit({{"ev", "ready"}, {"encoders", [] {
              json names = json::array();
              for (const auto& branch : live_branches) names.push_back(branch.first);
              return names;
          }()}});
    if (!models_dir.empty()) {
        inference = std::make_unique<rigmind::Inference>(models_dir, emit_inference_event, on_derived_facts);
        inference->set_engine_cache(std::filesystem::path(data_root) / "trt-cache");
        inference->start();
    } else {
        emit({{"ev", "error"}, {"message", "no models folder given: detection is off"}});
    }
    loop = g_main_loop_new(nullptr, FALSE);
    g_timeout_add(1000, on_tick, nullptr);

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

    inference.reset();
    g_io_channel_unref(input);
    g_main_loop_unref(loop);
    return 0;
}
