// Detection models (PPE, IDSS) on the live frames: ONNX Runtime on the CPU, YOLOv8 output decoding, NMS.
//
// The pipeline pushes 1280x720 RGB frames (about 10 per second at most) into Inference::push_frame from a GStreamer
// streaming thread; that call only swaps a pointer. One worker thread per model takes the newest frame when it is
// done with the previous one, so a slow model skips frames and never holds up the live path.
//
// Settings (conf, iou, imgsz, max_det, classes, file) come from <models>/inference.cfg: the line "RIGMIND-INFERENCE-CFG 1"
// followed by base64 of JSON (see scripts/inference_cfg.py). The file is re-read when it changes.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/alarm_state.h"
#include "pipeline/draw_types.h"
#include "pipeline/multi_camera.h"
#include "pipeline/ppe_check.h"
#include "pipeline/scene_trail.h"
#include "pipeline/zone_filter.h"

namespace rigmind {

// Decoded inference.cfg: the "defaults" object overlaid with one model's own keys.
std::string base64_decode(const std::string& text);
nlohmann::json load_inference_cfg(const std::filesystem::path& file);            // throws std::runtime_error
nlohmann::json model_settings(const nlohmann::json& cfg, const std::string& model); // null when the model is absent

struct Frame {
    std::string cam;
    std::int64_t ts_ms = 0;  // unix ms when the frame reached the detection tap
    std::int64_t pts_key = -1; // raw buffer PTS in ns: the key the overlay matches results to frames with
    std::int64_t rtp = -1;    // uint32 RTP timestamp the live video carries for this frame (90 kHz of the running time); -1 unknown
    std::int64_t pts_ms = -1; // buffer running time in ms (the clock of the live video); -1 when unknown
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb; // width * height * 3, tightly packed
};

struct ModelStats {
    std::string model;
    double fps = 0, avg_ms = 0, conf = 0, iou = 0;
    bool running = false;
    std::string provider = "cpu"; // "tensorrt", "cuda" or "cpu": what the session really runs on
    std::string provider_note;    // why a better provider was skipped (empty when none was)
    double load_seconds = 0;      // model load / TensorRT engine build time
    float scale = 0;        // IDSS: ROI / platform overlay scale in use (0 for other models)
    std::string scale_mode; // "zoomed", "wide", "auto-zoomed" or "auto-wide"
};

class Inference {
public:
    // Called from a worker thread with a finished JSON event ("detections"); the sink must be thread safe.
    using Emit = std::function<void(const nlohmann::json&)>;
    // Called from the IDSS worker with the EWS facts derived from each frame's detections, and the camera they belong to.
    using FactsSink = std::function<void(const std::string& cam, const FrameFacts&, bool zoomed)>;

    Inference(std::filesystem::path models_dir, Emit emit, FactsSink facts = nullptr);
    ~Inference();

    void set_engine_cache(std::filesystem::path dir) { cache_dir_ = std::move(dir); } // TensorRT engines; before start()
    void start(); // loads the models in the background, then waits for frames
    void push_frame(std::shared_ptr<const Frame> frame);
    // Burned-in overlay: which models are drawn, and the results for one frame. collect() waits (until `budget` after the
    // frame reached the tap) for every drawn, loaded model to finish that exact frame; a model that did not (skipped the frame,
    // too slow) contributes its newest earlier result if that is under 400 ms old, else nothing. It never waits for a model
    // that is not loaded.
    void set_overlay(bool ppe, bool idss) { overlay_ppe_ = ppe; overlay_idss_ = idss; }
    // Zone based inference: with an enabled zone for a camera only objects whose centre is inside it count for the alarm facts,
    // the person tracking and the PPE check (the platform always counts). Safe to call from any thread.
    void set_zone(const std::string& cam, Zone zone) { zones_.set(cam, std::move(zone)); }
    std::vector<DrawResult> collect(const std::string& cam, std::int64_t key, std::chrono::milliseconds budget);
    // True when every drawn, loaded model has finished the frame(s) it was given. The live pacer releases the next frame only
    // when this holds, so each frame that is shown reaches the models and its result can be drawn on it (no stale boxes).
    bool ready_for_frame(const std::string& cam) const;
    // The newest detection frame and, for each drawn model, its newest result (within 1 s of that frame): the input for an
    // annotated still (alarm image evidence). Null when no frame has arrived yet.
    // `cam` empty = the newest frame of any camera.
    std::shared_ptr<const Frame> latest_annotated(std::vector<DrawResult>& results, const std::string& cam = std::string());
    // Overlay sync since the previous call: per model, how many frames / ms older than the drawn frame the result used was
    // (0 = the frame's own result), the share of exact results, and frames drawn with no result at all.
    nlohmann::json take_sync();
    // Rates and thresholds of one camera since the previous call for that camera (the "inference" event); resets its window.
    std::vector<ModelStats> take_stats(const std::string& cam);

private:
    struct Worker;
    void run(Worker& worker);

    std::filesystem::path dir_;
    Emit emit_;
    FactsSink facts_;
    std::filesystem::path cache_dir_;
    mutable std::mutex mutex_;                 // guards slots_, stopping_ and every worker's pending set
    std::condition_variable wake_;
    FrameSlots<Frame> slots_;                  // newest frame of each camera; each worker serves them round robin
    bool stopping_ = false;
    std::atomic<bool> overlay_ppe_{true}, overlay_idss_{true};
    struct SyncWindow {
        std::vector<double> frames, ms;
        std::uint64_t n = 0, exact = 0, missing = 0;
    };
    SyncWindow sync_[2];
    mutable std::mutex draw_mutex_;
    std::condition_variable draw_cv_;
    DrawHistory history_;                                             // per model, camera: results by (cam, PTS); arrivals
    void publish(DrawResult result);
    ZoneBook zones_;                           // per camera, set by the host
    struct SceneSlot {                         // latest objects, outlines and centroid paths of both models, for one camera
        SceneTrail trail;
        std::atomic<std::int64_t> emit_ms{0};
    };
    SceneSlot& scene_for(const std::string& cam);
    std::mutex scene_mutex_;
    std::map<std::string, std::unique_ptr<SceneSlot>> scene_;
    struct PersonsSlot {
        std::vector<PpePerson> persons;
        std::int64_t ts = 0;
    };
    std::mutex persons_mutex_;                 // IDSS worker -> PPE worker: tracked 'derrickman' boxes of each camera for the PPE check
    std::map<std::string, PersonsSlot> persons_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace rigmind
