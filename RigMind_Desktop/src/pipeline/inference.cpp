#include "pipeline/inference.h"

#include <onnxruntime_cxx_api.h>

#include "pipeline/bytetrack.h"
#include "pipeline/ews_facts.h"
#include "pipeline/ppe_check.h"
#include "pipeline/platform_overlay.h"

#include <opencv2/core.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace rigmind {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ---- inference.cfg ------------------------------------------------------------------------------------------------
std::string base64_decode(const std::string& text) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') break;
        const auto at = alphabet.find(c);
        if (at == std::string::npos) continue; // line breaks and spaces
        buffer = (buffer << 6) | static_cast<unsigned>(at);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

json load_inference_cfg(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::string header;
    std::getline(in, header);
    while (!header.empty() && (header.back() == '\r' || header.back() == ' ')) header.pop_back();
    if (header != "RIGMIND-INFERENCE-CFG 1") throw std::runtime_error("not a RigMind inference settings file");
    std::stringstream body;
    body << in.rdbuf();
    const json cfg = json::parse(base64_decode(body.str()), nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object() || cfg.value("version", 0) != 1 || !cfg.contains("models"))
        throw std::runtime_error("inference settings are malformed");
    return cfg;
}

json model_settings(const json& cfg, const std::string& model) {
    const auto it = cfg["models"].find(model);
    if (it == cfg["models"].end() || !it->is_object()) return nullptr;
    json merged = cfg.value("defaults", json::object());
    for (const auto& [key, value] : it->items()) merged[key] = value;
    return merged;
}

namespace {

struct Settings {
    float conf = 0.25f, iou = 0.45f;
    int imgsz = 640, max_det = 100;
    bool agnostic = false;
    std::vector<int> classes; // empty: all
    std::string file;
};

Settings parse_settings(const json& s) {
    Settings out;
    out.conf = s.value("conf", out.conf);
    out.iou = s.value("iou", out.iou);
    out.imgsz = s.value("imgsz", out.imgsz);
    out.max_det = s.value("max_det", out.max_det);
    out.agnostic = s.value("agnostic_nms", false);
    out.file = s.value("file", std::string());
    if (s.contains("classes") && s["classes"].is_array()) {
        for (const auto& c : s["classes"]) {
            if (c.is_number_integer()) out.classes.push_back(c.get<int>());
        }
    }
    return out;
}

RoiConfig parse_roi(const json& cfg) {
    RoiConfig roi;
    const auto it = cfg.find("roi");
    if (it == cfg.end() || !it->is_object()) return roi;
    roi.enabled = it->value("enabled", roi.enabled);
    roi.reference_class = it->value("reference_class", roi.reference_class);
    roi.scale = it->value("scale", roi.scale);
    roi.hold_frames = it->value("hold_frames", roi.hold_frames);
    return roi;
}

AutoScaleConfig parse_auto_scale(const json& cfg) {
    AutoScaleConfig out;
    const auto it = cfg.find("auto_scale");
    if (it == cfg.end() || !it->is_object()) return out;
    out.enabled = it->value("enabled", out.enabled);
    out.wide = it->value("wide", out.wide);
    out.zoomed = it->value("zoomed", out.zoomed);
    out.width_fraction = it->value("zoomed_if_platform_width_fraction_at_least", out.width_fraction);
    out.hysteresis = it->value("hysteresis", out.hysteresis);
    out.median_frames = it->value("median_frames", out.median_frames);
    if (it->contains("force") && (*it)["force"].is_string()) out.force = (*it)["force"].get<std::string>();
    return out;
}

TrackConfig parse_tracking(const json& cfg) {
    TrackConfig out;
    const auto it = cfg.find("tracking");
    if (it != cfg.end() && it->is_object()) {
        out.enabled = it->value("enabled", out.enabled) && it->value("algorithm", std::string("bytetrack")) == "bytetrack";
        out.high_thresh = it->value("high_thresh", out.high_thresh);
        out.low_thresh = it->value("low_thresh", out.low_thresh);
        out.new_track_thresh = it->value("new_track_thresh", out.new_track_thresh);
        out.match_thresh = it->value("match_thresh", out.match_thresh);
        out.track_buffer_s = it->value("track_buffer_s", out.track_buffer_s);
        out.min_hits = it->value("min_hits", out.min_hits);
    }
    if (const char* env = std::getenv("RIGMIND_TRACKING"); env && std::string(env) == "off") out.enabled = false; // A/B testing
    return out;
}

PpeCheckConfig parse_ppe_check(const json& cfg) {
    PpeCheckConfig out;
    const auto it = cfg.find("ppe_check");
    if (it == cfg.end() || !it->is_object()) return out;
    out.enabled = it->value("enabled", out.enabled);
    out.person_class = it->value("person_class", out.person_class);
    out.person_box_pad = it->value("person_box_pad", out.person_box_pad);
    out.missing_ms = static_cast<int>(std::lround(1000.0 * it->value("missing_seconds", out.missing_ms / 1000.0)));
    out.clear_ms = static_cast<int>(std::lround(1000.0 * it->value("clear_seconds", out.clear_ms / 1000.0)));
    if (it->contains("required") && (*it)["required"].is_array()) {
        std::vector<std::string> req;
        for (const auto& r : (*it)["required"]) if (r.is_string()) req.push_back(r.get<std::string>());
        if (!req.empty()) out.required = req;
    }
    return out;
}

// ultralytics stores the class names as a Python dict string: {0: 'person', 1: "it's"}.
std::map<int, std::string> parse_names(const std::string& text) {
    std::map<int, std::string> names;
    static const std::regex entry(R"re((\d+)\s*:\s*(?:'([^']*)'|"([^"]*)"))re");
    for (std::sregex_iterator it(text.begin(), text.end(), entry), end; it != end; ++it) {
        names[std::stoi((*it)[1])] = (*it)[2].matched ? (*it)[2].str() : (*it)[3].str();
    }
    return names;
}

// Letterbox like ultralytics: scale to fit, centre, pad with 114 grey. Bilinear with half-pixel centres (cv2 INTER_LINEAR).
struct Letterbox {
    float scale = 1;
    int left = 0, top = 0;
};

Letterbox letterbox_to_tensor(const Frame& frame, int dst_w, int dst_h, std::vector<float>& tensor) {
    Letterbox lb;
    lb.scale = std::min(static_cast<float>(dst_w) / frame.width, static_cast<float>(dst_h) / frame.height);
    const int new_w = static_cast<int>(std::round(frame.width * lb.scale));
    const int new_h = static_cast<int>(std::round(frame.height * lb.scale));
    const float pad_w = (dst_w - new_w) / 2.0f, pad_h = (dst_h - new_h) / 2.0f;
    lb.left = static_cast<int>(std::round(pad_w - 0.1f));
    lb.top = static_cast<int>(std::round(pad_h - 0.1f));

    const std::size_t plane = static_cast<std::size_t>(dst_w) * dst_h;
    tensor.assign(plane * 3, 114.0f / 255.0f);

    std::vector<int> x0(new_w), x1(new_w);
    std::vector<float> wx(new_w);
    for (int x = 0; x < new_w; ++x) {
        const float sx = std::max(0.0f, (x + 0.5f) * frame.width / new_w - 0.5f);
        x0[x] = std::min(static_cast<int>(sx), frame.width - 1);
        x1[x] = std::min(x0[x] + 1, frame.width - 1);
        wx[x] = sx - x0[x];
    }
    const std::uint8_t* src = frame.rgb.data();
    const std::size_t stride = static_cast<std::size_t>(frame.width) * 3;
    for (int y = 0; y < new_h; ++y) {
        const float sy = std::max(0.0f, (y + 0.5f) * frame.height / new_h - 0.5f);
        const int y0 = std::min(static_cast<int>(sy), frame.height - 1);
        const int y1 = std::min(y0 + 1, frame.height - 1);
        const float wy = sy - y0;
        const std::uint8_t* row0 = src + y0 * stride;
        const std::uint8_t* row1 = src + y1 * stride;
        const std::size_t out_row = static_cast<std::size_t>(lb.top + y) * dst_w + lb.left;
        for (int x = 0; x < new_w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float top = row0[x0[x] * 3 + c] * (1 - wx[x]) + row0[x1[x] * 3 + c] * wx[x];
                const float bottom = row1[x0[x] * 3 + c] * (1 - wx[x]) + row1[x1[x] * 3 + c] * wx[x];
                tensor[c * plane + out_row + x] = (top * (1 - wy) + bottom * wy) / 255.0f;
            }
        }
    }
    return lb;
}

float iou_of(const float* a, const float* b) {
    const float ix = std::max(0.0f, std::min(a[2], b[2]) - std::max(a[0], b[0]));
    const float iy = std::max(0.0f, std::min(a[3], b[3]) - std::max(a[1], b[1]));
    const float inter = ix * iy;
    const float uni = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter;
    return uni > 0 ? inter / uni : 0.0f;
}

std::int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "rigmind");
    return env;
}

// ONNX Runtime's own worker threads run at the lowest priority: on a busy machine the live decode/encode must win.
OrtCustomThreadHandle low_priority_thread(void*, OrtThreadWorkerFn work, void* param) {
    auto* thread = new std::thread([work, param] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
        work(param);
    });
    return reinterpret_cast<OrtCustomThreadHandle>(thread);
}

void join_low_priority_thread(OrtCustomThreadHandle handle) {
    auto* thread = reinterpret_cast<std::thread*>(const_cast<OrtCustomHandleType*>(handle));
    if (thread->joinable()) thread->join();
    delete thread;
}

// One loaded ONNX model (YOLOv8 detect or segment, batch 1).
struct Detection {
    int cls = 0;
    float conf = 0;
    float box[4] = {0, 0, 0, 0}; // x1, y1, x2, y2 in frame pixels
    int anchor = 0;              // column in the model output (mask coefficients live there)
    cv::Mat mask;                // box-sized binary mask (segment models only)
    int track_id = 0;            // ByteTrack id (PPE with tracking on), 0 otherwise
    bool held = false;           // drawn from the tracker prediction: no detection on this frame
    int lost_frames = 0;         // consecutive frames the track went unmatched
    bool has_raw = false;        // raw[] = this frame's own detection box (matched tracks); the display uses it, not the Kalman state
    float raw[4] = {0, 0, 0, 0};
};

class Detector {
public:
    // Provider preference: CUDA, then CPU. The first that loads and builds a session is used. TensorRT (fp16, engine cached
    // in cache_dir) is opt-in with RIGMIND_PROVIDER=tensorrt: it stalled for minutes without producing detections where
    // nvinfer.dll was present. RIGMIND_PROVIDER=cpu starts the list lower (testing, or a machine with a bad driver).
    Detector(const fs::path& file, int intra_threads, const fs::path& cache_dir) : session_(nullptr) {
        std::vector<std::string> order = {"cuda", "cpu"};
        if (const char* forced = std::getenv("RIGMIND_PROVIDER")) {
            if (std::string(forced) == "tensorrt") order.insert(order.begin(), "tensorrt");
            const auto at = std::find(order.begin(), order.end(), std::string(forced));
            if (at != order.end()) order.erase(order.begin(), at);
        }
        std::string failures;
        const auto started = std::chrono::steady_clock::now();
        for (const std::string& kind : order) {
            try {
                session_ = make_session(file, intra_threads, cache_dir, kind);
                provider_ = kind;
                break;
            } catch (const std::exception& e) {
                failures += kind + ": " + e.what() + "; ";
                if (kind == "cpu") throw std::runtime_error(failures);
            }
        }
        load_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        failures_ = failures;
        init_from_session();
    }

    const std::string& provider() const { return provider_; }
    double load_seconds() const { return load_seconds_; }
    const std::string& failures() const { return failures_; }

private:
    static Ort::Session make_session(const fs::path& file, int intra_threads, const fs::path& cache_dir, const std::string& kind) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(intra_threads);
        options.SetInterOpNumThreads(1);
        options.SetCustomCreateThreadFn(low_priority_thread);
        options.SetCustomJoinThreadFn(join_low_priority_thread);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0"); // idle threads sleep: the live encode needs the CPU
        ort_env(); // the TensorRT provider needs the environment (default logger) before its options are made
        const OrtApi& api = Ort::GetApi();
        if (kind == "tensorrt") {
            OrtTensorRTProviderOptionsV2* trt = nullptr;
            Ort::ThrowOnError(api.CreateTensorRTProviderOptions(&trt));
            std::error_code ec;
            fs::create_directories(cache_dir, ec);
            const std::string cache = cache_dir.string();
            const char* keys[] = {"device_id", "trt_fp16_enable", "trt_engine_cache_enable", "trt_engine_cache_path", "trt_max_workspace_size"};
            const char* values[] = {"0", "1", "1", cache.c_str(), "1073741824"};
            OrtStatus* status = api.UpdateTensorRTProviderOptions(trt, keys, values, 5);
            if (!status) {
                try { options.AppendExecutionProvider_TensorRT_V2(*trt); } catch (...) { api.ReleaseTensorRTProviderOptions(trt); throw; }
            }
            api.ReleaseTensorRTProviderOptions(trt);
            Ort::ThrowOnError(status);
            // TensorRT takes the graph partitions it supports; the rest goes to CUDA, then CPU.
            OrtCUDAProviderOptionsV2* cuda = nullptr;
            Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda));
            try { options.AppendExecutionProvider_CUDA_V2(*cuda); } catch (...) { api.ReleaseCUDAProviderOptions(cuda); throw; }
            api.ReleaseCUDAProviderOptions(cuda);
        } else if (kind == "cuda") {
            OrtCUDAProviderOptionsV2* cuda = nullptr;
            Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda));
            try { options.AppendExecutionProvider_CUDA_V2(*cuda); } catch (...) { api.ReleaseCUDAProviderOptions(cuda); throw; }
            api.ReleaseCUDAProviderOptions(cuda);
        }
        return Ort::Session(ort_env(), file.c_str(), options);
    }

    void init_from_session() {
        Ort::AllocatorWithDefaultOptions allocator;
        input_name_ = session_.GetInputNameAllocated(0, allocator).get();
        output_name_ = session_.GetOutputNameAllocated(0, allocator).get(); // output0 only: the mask prototypes are not computed
        segment_ = session_.GetOutputCount() > 1;
        if (segment_) proto_name_ = session_.GetOutputNameAllocated(1, allocator).get();
        const auto shape = session_.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 4 && shape[2] > 0 && shape[3] > 0) {
            height_ = static_cast<int>(shape[2]);
            width_ = static_cast<int>(shape[3]);
        }
        const auto meta = session_.GetModelMetadata();
        if (auto names = meta.LookupCustomMetadataMapAllocated("names", allocator)) names_ = parse_names(names.get());
    }

public:
    bool fixed_size() const { return width_ > 0; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool segment() const { return segment_; }
    std::string label(int cls) const {
        const auto it = names_.find(cls);
        return it != names_.end() ? it->second : "class" + std::to_string(cls);
    }

    // Boxes in frame pixels.
    std::vector<Detection> detect(const Frame& frame, const Settings& cfg, int imgsz) {
        const int in_w = width_ > 0 ? width_ : imgsz, in_h = height_ > 0 ? height_ : imgsz;
        const Letterbox lb = letterbox_to_tensor(frame, in_w, in_h, tensor_);
        const std::array<int64_t, 4> shape{1, 3, in_h, in_w};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(memory, tensor_.data(), tensor_.size(), shape.data(), shape.size());
        const char* in_names[] = {input_name_.c_str()};
        // Detect models: output0 only. Segment models also return the mask prototypes (output1).
        const char* out_names[] = {output_name_.c_str(), proto_name_.c_str()};
        auto outputs = session_.Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, segment_ ? 2 : 1);

        const auto dims = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); // [1, 4 + nc (+ 32), anchors]
        if (dims.size() != 3) return {};
        const int channels = static_cast<int>(dims[1]), anchors = static_cast<int>(dims[2]);
        const int classes = channels - 4 - (segment_ ? 32 : 0);
        if (classes < 1) return {};
        const float* data = outputs[0].GetTensorData<float>();

        std::vector<Detection> found;
        for (int i = 0; i < anchors; ++i) {
            int best = 0;
            float best_score = data[4 * anchors + i];
            for (int c = 1; c < classes; ++c) {
                const float score = data[(4 + c) * anchors + i];
                if (score > best_score) {
                    best_score = score;
                    best = c;
                }
            }
            if (best_score < cfg.conf) continue;
            if (!cfg.classes.empty() && std::find(cfg.classes.begin(), cfg.classes.end(), best) == cfg.classes.end()) continue;
            const float cx = data[i], cy = data[anchors + i], w = data[2 * anchors + i], h = data[3 * anchors + i];
            Detection d;
            d.cls = best;
            d.anchor = i;
            d.conf = best_score;
            d.box[0] = std::clamp((cx - w / 2 - lb.left) / lb.scale, 0.0f, static_cast<float>(frame.width));
            d.box[1] = std::clamp((cy - h / 2 - lb.top) / lb.scale, 0.0f, static_cast<float>(frame.height));
            d.box[2] = std::clamp((cx + w / 2 - lb.left) / lb.scale, 0.0f, static_cast<float>(frame.width));
            d.box[3] = std::clamp((cy + h / 2 - lb.top) / lb.scale, 0.0f, static_cast<float>(frame.height));
            found.push_back(d);
        }

        // Greedy NMS, per class unless agnostic.
        std::sort(found.begin(), found.end(), [](const Detection& a, const Detection& b) { return a.conf > b.conf; });
        std::vector<Detection> kept;
        for (const Detection& d : found) {
            bool suppressed = false;
            for (const Detection& k : kept) {
                if ((cfg.agnostic || k.cls == d.cls) && iou_of(k.box, d.box) > cfg.iou) {
                    suppressed = true;
                    break;
                }
            }
            if (suppressed) continue;
            kept.push_back(d);
            if (static_cast<int>(kept.size()) >= cfg.max_det) break;
        }
        if (segment_ && outputs.size() > 1) {
            const auto pd = outputs[1].GetTensorTypeAndShapeInfo().GetShape(); // [1, 32, seg_h, seg_w]
            if (pd.size() == 4) {
                const float* proto = outputs[1].GetTensorData<float>();
                const int seg_ch = static_cast<int>(pd[1]), seg_h = static_cast<int>(pd[2]), seg_w = static_cast<int>(pd[3]);
                std::vector<float> coeffs(seg_ch);
                for (Detection& d : kept) {
                    for (int k = 0; k < seg_ch; ++k) coeffs[k] = data[(4 + classes + k) * anchors + d.anchor];
                    const cv::Rect box(static_cast<int>(d.box[0]), static_cast<int>(d.box[1]), static_cast<int>(d.box[2]) - static_cast<int>(d.box[0]),
                                       static_cast<int>(d.box[3]) - static_cast<int>(d.box[1]));
                    d.mask = decode_box_mask(proto, seg_ch, seg_h, seg_w, coeffs.data(), in_w, lb.scale, lb.left, lb.top, frame.width, frame.height, box);
                }
            }
        }
        return kept;
    }

private:
    Ort::Session session_;
    std::string input_name_, output_name_, proto_name_;
    std::map<int, std::string> names_;
    std::vector<float> tensor_;
    int width_ = 0, height_ = 0;
    bool segment_ = false;
    std::string provider_ = "cpu", failures_;
    double load_seconds_ = 0;
};

} // namespace

// ---- Inference ------------------------------------------------------------------------------------------------------
constexpr int kHeldDrawFrames = 3; // a lost PPE track is drawn (dashed) for this many frames only

struct Inference::Worker {
    std::string model;
    std::chrono::milliseconds min_interval;
    int threads;
    std::thread thread;
    std::atomic<std::uint64_t> count{0}, total_us{0};
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
    std::mutex mutex; // guards `settings` (read by take_stats)
    Settings settings;
    bool running = false;
    int dumped = 0;
    std::atomic<bool> loaded{false};
    std::atomic<bool> warm{false};    // first frame done (the first run on a GPU takes seconds): only then does the overlay wait for it
    std::atomic<bool> pending{false}; // a frame was handed over and its result is not published yet
    float scale = 0;
    std::string scale_mode; // IDSS only
    std::string provider = "cpu", provider_note;
    double load_seconds = 0;
};

namespace {
// Window counters for the "facts" diagnostic event.
const std::array<const char*, 9> kDiagNames = {"machine_present", "link_tilt_present", "link_tilt_angled", "elevator_present",
                                                "elevator_near_platform", "platform_elevator_collision",
                                                "derrickman_out_of_safe_zone", "machine_past_right_edge", "link_tilt_near_derrickman"};
struct FactsDiag {
    int frames = 0;
    std::array<int, 9> fed{}, all{}, old_roi{};
    std::vector<float> angles;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    void add(std::array<int, 9>& c, const FrameFacts& f) {
        const bool v[9] = {f.machine_present, f.link_tilt_present, f.link_tilt_angled, f.elevator_present, f.elevator_near_platform,
                           f.platform_elevator_collision, f.derrickman_out_of_safe_zone, f.machine_past_right_edge, f.link_tilt_near_derrickman};
        for (int i = 0; i < 9; ++i) c[i] += v[i] ? 1 : 0;
    }
};
} // namespace

Inference::Inference(fs::path models_dir, Emit emit, FactsSink facts)
    : dir_(std::move(models_dir)), emit_(std::move(emit)), facts_(std::move(facts)) {
    // PPE is the light model and runs at the higher rate; IDSS (YOLOv8m-seg) is about ten times heavier.
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    for (const auto& [name, interval, threads] : {std::make_tuple("ppe", 0, 1), std::make_tuple("idss", 0, cpus >= 6 ? 2 : 1)}) {
        auto worker = std::make_unique<Worker>();
        worker->model = name;
        worker->min_interval = std::chrono::milliseconds(interval);
        worker->threads = threads;
        workers_.push_back(std::move(worker));
    }
}

Inference::~Inference() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
}

void Inference::start() {
    for (auto& worker : workers_) worker->thread = std::thread([this, w = worker.get()] { run(*w); });
}

void Inference::push_frame(std::shared_ptr<const Frame> frame) {
    {
        std::lock_guard<std::mutex> lock(draw_mutex_);
        arrivals_.emplace_back(frame->pts_key, std::chrono::steady_clock::now());
        if (arrivals_.size() > 32) arrivals_.erase(arrivals_.begin());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(frame);
        ++sequence_;
        for (auto& worker : workers_) worker->pending = true;
    }
    wake_.notify_all();
}

void Inference::publish(DrawResult result) {
    {
        std::lock_guard<std::mutex> lock(draw_mutex_);
        auto& history = history_[result.model == "idss" ? 1 : 0];
        history.push_back(std::move(result));
        if (history.size() > 8) history.erase(history.begin());
    }
    draw_cv_.notify_all();
}

bool Inference::ready_for_frame() const {
    const bool wanted[2] = {overlay_ppe_.load(), overlay_idss_.load()};
    for (int m = 0; m < 2; ++m)
        if (wanted[m] && workers_[static_cast<std::size_t>(m)]->warm.load() && workers_[static_cast<std::size_t>(m)]->pending.load()) return false;
    return true;
}

nlohmann::json Inference::take_sync() {
    std::lock_guard<std::mutex> lock(draw_mutex_);
    auto stat = [](std::vector<double>& v) -> json {
        if (v.empty()) return nullptr;
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) { return std::round(v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))] * 10) / 10; };
        return {{"p50", pct(0.5)}, {"p95", pct(0.95)}, {"max", std::round(v.back() * 10) / 10}};
    };
    json out = json::object();
    const char* names[2] = {"ppe", "idss"};
    for (int m = 0; m < 2; ++m) {
        SyncWindow& w = sync_[m];
        if (!w.n) continue;
        out[names[m]] = {{"n", w.n}, {"exact_pct", std::round(1000.0 * static_cast<double>(w.exact) / static_cast<double>(w.n)) / 10.0},
                         {"missing", w.missing}, {"lag_frames", stat(w.frames)}, {"lag_ms", stat(w.ms)}};
        w = SyncWindow{};
    }
    return out;
}

// A result older than this many frames is not drawn on a newer frame: no box is better than a box that lags the person.
constexpr int kMaxStaleFrames = 2;

std::vector<DrawResult> Inference::collect(std::int64_t key, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(draw_mutex_);
    auto deadline = std::chrono::steady_clock::now() + budget;
    for (const auto& arrival : arrivals_) {
        if (arrival.first == key) deadline = arrival.second + budget;
    }
    const bool wanted[2] = {overlay_ppe_.load(), overlay_idss_.load()};
    bool loaded[2] = {false, false};
    for (int m = 0; m < 2; ++m) loaded[m] = workers_[m]->warm.load();
    auto exact = [&](int m) {
        for (const DrawResult& r : history_[m])
            if (r.key == key) return true;
        return false;
    };
    draw_cv_.wait_until(lock, deadline, [&] {
        for (int m = 0; m < 2; ++m)
            if (wanted[m] && loaded[m] && !exact(m)) return false;
        return true;
    });
    // Position of a frame in the recent arrival list: frame distance between two keys.
    auto index_of = [&](std::int64_t k) -> int {
        for (std::size_t i = 0; i < arrivals_.size(); ++i)
            if (arrivals_[i].first == k) return static_cast<int>(i);
        return -1;
    };
    const int cur_index = index_of(key);
    std::vector<DrawResult> out;
    for (int m = 0; m < 2; ++m) {
        if (!wanted[m]) continue;
        const DrawResult* pick = nullptr;
        for (const DrawResult& r : history_[m]) {
            if (r.key == key) {
                pick = &r;
                break;
            }
            if (r.key < key && key - r.key < 400000000LL) { // newest earlier one, if it is at most kMaxStaleFrames frames old
                const int idx = index_of(r.key);
                const int lag = (cur_index >= 0 && idx >= 0) ? cur_index - idx : 99;
                if (lag <= kMaxStaleFrames) pick = &r;
            }
        }
        if (loaded[m]) {
            SyncWindow& w = sync_[m];
            ++w.n;
            if (!pick) {
                ++w.missing;
            } else if (pick->key == key) {
                ++w.exact;
                w.frames.push_back(0);
                w.ms.push_back(0);
            } else {
                const int idx = index_of(pick->key);
                w.frames.push_back((cur_index >= 0 && idx >= 0) ? cur_index - idx : 99);
                w.ms.push_back(static_cast<double>(key - pick->key) / 1e6);
            }
        }
        if (pick) out.push_back(*pick);
    }
    return out;
}

std::vector<ModelStats> Inference::take_stats() {
    std::vector<ModelStats> out;
    const auto now = std::chrono::steady_clock::now();
    for (auto& worker : workers_) {
        ModelStats s;
        s.model = worker->model;
        const double seconds = std::max(0.001, std::chrono::duration<double>(now - worker->window_start).count());
        const std::uint64_t count = worker->count.exchange(0), total_us = worker->total_us.exchange(0);
        worker->window_start = now;
        s.fps = count / seconds;
        s.avg_ms = count ? total_us / 1000.0 / count : 0;
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            s.conf = worker->settings.conf;
            s.iou = worker->settings.iou;
            s.running = worker->running;
            s.provider = worker->provider;
            s.provider_note = worker->provider_note;
            s.load_seconds = worker->load_seconds;
            s.scale = worker->scale;
            s.scale_mode = worker->scale_mode;
        }
        out.push_back(s);
    }
    return out;
}

void Inference::run(Worker& worker) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // the live encode comes first
#endif
    const fs::path cfg_file = dir_ / "inference.cfg";
    std::unique_ptr<Detector> detector;
    Settings settings;
    RoiConfig roi_cfg;
    FactsDiag diag;
    RoiState roi_state; // IDSS only
    AutoScaleConfig scale_cfg;
    ScaleSelector scale_selector;
    TrackConfig track_cfg;
    PpeCheckConfig ppe_cfg;
    ByteTracker ppe_tracker, person_tracker;
    PpeChecker ppe_checker;
    struct PpeWindow {
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        std::uint64_t track_us = 0, track_n = 0;
    } ppe_window;
    ScaleSelector threshold_selector; // decides the EWS threshold set from the Platform width, even when the mask scale is pinned
    fs::file_time_type cfg_time{};
    try {
        const json cfg = load_inference_cfg(cfg_file);
        cfg_time = fs::last_write_time(cfg_file);
        const json model = model_settings(cfg, worker.model);
        if (model.is_null()) throw std::runtime_error("no settings for model " + worker.model);
        settings = parse_settings(model);
        roi_cfg = parse_roi(cfg);
        scale_cfg = parse_auto_scale(cfg);
        track_cfg = parse_tracking(cfg);
        ppe_cfg = parse_ppe_check(cfg);
        ppe_checker.configure(ppe_cfg);
        detector = std::make_unique<Detector>(dir_ / fs::u8path(settings.file), worker.threads, cache_dir_);
    } catch (const std::exception& e) {
        emit_({{"ev", "error"}, {"message", "inference " + worker.model + ": " + e.what()}});
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.settings = settings;
        worker.provider = detector->provider();
        worker.provider_note = detector->failures();
        worker.load_seconds = detector->load_seconds();
        worker.running = true;
        worker.loaded = true;
    }

    std::uint64_t seen = 0;
    auto next_allowed = std::chrono::steady_clock::now();
    auto next_cfg_check = next_allowed + std::chrono::seconds(2);
    while (true) {
        std::shared_ptr<const Frame> frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || sequence_ != seen; });
            if (stopping_) return;
            // A fresh frame is waiting: hold it back until this model's rate allows another run, then take the newest.
            wake_.wait_until(lock, next_allowed, [&] { return stopping_; });
            if (stopping_) return;
            frame = latest_;
            seen = sequence_;
        }
        const auto started = std::chrono::steady_clock::now();
        next_allowed = started + worker.min_interval;

        // Thresholds can be edited while the app runs: pick up a changed inference.cfg (a bad edit keeps the old values).
        if (started >= next_cfg_check) {
            next_cfg_check = started + std::chrono::seconds(2);
            try {
                const auto stamp = fs::last_write_time(cfg_file);
                if (stamp != cfg_time) {
                    cfg_time = stamp;
                    const json reloaded = load_inference_cfg(cfg_file);
                    const json model = model_settings(reloaded, worker.model);
                    roi_cfg = parse_roi(reloaded);
                    scale_cfg = parse_auto_scale(reloaded);
                    track_cfg = parse_tracking(reloaded);
                    ppe_cfg = parse_ppe_check(reloaded);
                    ppe_checker.configure(ppe_cfg);
                    if (!model.is_null()) {
                        const Settings next = parse_settings(model);
                        settings.conf = next.conf;
                        settings.iou = next.iou;
                        settings.max_det = next.max_det;
                        settings.agnostic = next.agnostic;
                        settings.classes = next.classes;
                        settings.imgsz = next.imgsz;
                        std::lock_guard<std::mutex> lock(worker.mutex);
                        worker.settings = settings;
                    }
                }
            } catch (const std::exception&) {
            }
        }

        std::vector<Detection> found;
        try {
            // PPE with tracking: the model runs down to low_thresh so ByteTrack's second stage sees the weak boxes.
            Settings run_settings = settings;
            if (worker.model == "ppe" && track_cfg.enabled) run_settings.conf = std::min(settings.conf, track_cfg.low_thresh);
            found = detector->detect(*frame, run_settings, settings.imgsz);
        } catch (const std::exception& e) {
            worker.pending = false;
            emit_({{"ev", "error"}, {"message", "inference " + worker.model + ": " + e.what()}});
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        // Test hook: RIGMIND_DUMP_FRAMES=<dir> saves the first frames each model sees (PPM, named by event ts) so the
        // detections can be checked against ultralytics on the same pixels.
        if (const char* dump = std::getenv("RIGMIND_DUMP_FRAMES"); dump && worker.dumped < 6) {
            ++worker.dumped;
            std::ofstream out(fs::path(dump) / ("frame_" + worker.model + "_" + std::to_string(frame->ts_ms) + ".ppm"), std::ios::binary);
            out << "P6 " << frame->width << " " << frame->height << " 255" << '\n';
            out.write(reinterpret_cast<const char*>(frame->rgb.data()), static_cast<std::streamsize>(frame->rgb.size()));
        }
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        worker.count.fetch_add(1);
        worker.total_us.fetch_add(static_cast<std::uint64_t>(elapsed_us));

        // PPE: ByteTrack over the detections (display, the detections event and the presence check all use its output).
        if (worker.model == "ppe") {
            if (track_cfg.enabled) {
                const auto t0 = std::chrono::steady_clock::now();
                TrackConfig tc = track_cfg;
                tc.new_track_thresh = std::max(tc.new_track_thresh, settings.conf); // the model's own conf starts new tracks
                std::vector<TrackDet> dets;
                for (const Detection& d : found) dets.push_back({d.cls, detector->label(d.cls), d.conf, {d.box[0], d.box[1], d.box[2], d.box[3]}});
                std::vector<Detection> tracked;
                for (const TrackOut& o : ppe_tracker.update(frame->ts_ms, dets, tc)) {
                    Detection d;
                    d.cls = o.cls;
                    d.conf = o.conf;
                    d.box[0] = o.box.x1; d.box[1] = o.box.y1; d.box[2] = o.box.x2; d.box[3] = o.box.y2;
                    d.track_id = o.id;
                    d.held = o.held;
                    d.lost_frames = o.lost_frames;
                    if (!o.held) {
                        d.has_raw = true;
                        d.raw[0] = o.det.x1; d.raw[1] = o.det.y1; d.raw[2] = o.det.x2; d.raw[3] = o.det.y2;
                    }
                    tracked.push_back(std::move(d));
                }
                found = std::move(tracked);
                ppe_window.track_us += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                ++ppe_window.track_n;
            } else {
                ppe_tracker.reset();
            }
        }
        // IDSS: Platform-based ROI. Every detection is reported with in_roi; the EWS facts see only the filtered ones.
        const bool is_idss = worker.model == "idss";
        std::optional<Box> roi;
        std::vector<NamedBox> named;
        for (const Detection& d : found) {
            NamedBox nb{detector->label(d.cls), d.conf, {d.box[0], d.box[1], d.box[2], d.box[3]}, {}};
            // The collision rule needs the Platform and Elevator outlines (IDSS tests mask contour points).
            if (is_idss && !d.mask.empty() && (same_name(nb.label, "platform") || same_name(nb.label, "elevators") || same_name(nb.label, "elevator"))) {
                for (const cv::Point& p : mask_polygon(d.mask, cv::Point(static_cast<int>(d.box[0]), static_cast<int>(d.box[1]))))
                    nb.contour.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
            }
            named.push_back(std::move(nb));
        }
        // IDSS 'derrickman' ids for the PPE check. The EWS facts below still use `named` untouched.
        if (is_idss && ppe_cfg.enabled) {
            std::vector<TrackDet> person_dets;
            for (std::size_t i = 0; i < named.size(); ++i)
                if (same_name(named[i].label, ppe_cfg.person_class)) person_dets.push_back({static_cast<int>(i), named[i].label, named[i].conf, named[i].box});
            TrackConfig pc = track_cfg;
            pc.new_track_thresh = std::max(pc.new_track_thresh, settings.conf); // the IDSS model's own conf: nothing weaker reaches the tracker
            std::vector<PpePerson> people;
            if (pc.enabled) {
                for (const TrackOut& t : person_tracker.update(frame->ts_ms, person_dets, pc)) people.push_back({t.id, t.box});
            } else {
                int index = 0;
                for (const TrackDet& d : person_dets) people.push_back({++index, d.box});
            }
            std::lock_guard<std::mutex> lock(persons_mutex_);
            persons_ = std::move(people);
            persons_ts_ = frame->ts_ms;
            persons_cam_ = frame->cam;
        }
        if (is_idss) {
            // One scale for the ROI and the platform overlay: pinned by auto_scale.force, or from the Platform's width.
            std::optional<float> platform_fraction;
            float best_conf = -1;
            for (const NamedBox& d : named) {
                if (same_name(d.label, roi_cfg.reference_class) && d.conf > best_conf) {
                    best_conf = d.conf;
                    platform_fraction = d.box.width() / static_cast<float>(frame->width);
                }
            }
            roi_cfg.scale = scale_selector.update(platform_fraction, scale_cfg);
            // IDSS ties the whole threshold set to its zoom flag (ews.cpp setThresholds). Pinning the mask scale (auto_scale.force)
            // must not pick the wrong set for the picture, so the set always follows the Platform's measured width.
            AutoScaleConfig threshold_cfg = scale_cfg;
            threshold_cfg.force.clear();
            threshold_selector.update(platform_fraction, threshold_cfg);
            std::lock_guard<std::mutex> lock(worker.mutex);
            worker.scale = roi_cfg.scale;
            worker.scale_mode = scale_selector.mode();
        }
        if (is_idss && roi_cfg.enabled) {
            roi = update_roi(roi_state, compute_platform_roi(named, roi_cfg, frame->width, frame->height), roi_cfg);
        } else {
            roi_state = {};
        }
        if (is_idss) {
            FactsDetail detail_all;
            const bool zoomed = threshold_selector.mode().find("zoomed") != std::string::npos; // ews.cpp setThresholds(zoomFlag)
            const FactThresholds thresholds = FactThresholds::for_zoom(zoomed);
            const FrameFacts fed = derive_facts_with_roi(named, roi, roi_cfg, thresholds, &detail_all);
            const FrameFacts all = derive_facts(named, thresholds);                                      // diagnostic: no ROI at all
            const FrameFacts old_roi = derive_facts(filter_detections(named, roi, roi_cfg), thresholds); // diagnostic: ROI on every rule (previous behaviour)
            if (facts_) facts_(fed, zoomed);
            // Low-rate diagnostic ("facts", ~2 Hz): the facts the EWS sees (ROI applied) and the same without ROI, with
            // the percentage of frames in the last window on which each was true.
            ++diag.frames;
            diag.add(diag.fed, fed);
            diag.add(diag.all, all);
            diag.add(diag.old_roi, old_roi);
            if (!std::isnan(detail_all.angle_deg)) diag.angles.push_back(detail_all.angle_deg);
            const auto now = std::chrono::steady_clock::now();
            if (now - diag.last >= std::chrono::milliseconds(500)) {
                auto pct = [&](const std::array<int, 9>& c) {
                    json o = json::object();
                    for (std::size_t i = 0; i < kDiagNames.size(); ++i) o[kDiagNames[i]] = std::round(1000.0 * c[i] / diag.frames) / 10.0;
                    return o;
                };
                auto num = [](float v) { return std::isnan(v) ? json(nullptr) : json(std::round(v * 10.0f) / 10.0f); };
                json ev = {{"ev", "facts"}, {"cam", frame->cam}, {"zoomed", zoomed}, {"scale_mode", scale_selector.mode()}, {"frames", diag.frames}, {"w", frame->width}, {"h", frame->height},
                           {"machine_present", fed.machine_present}, {"link_tilt_present", fed.link_tilt_present},
                           {"link_tilt_angled", fed.link_tilt_angled}, {"elevator_present", fed.elevator_present},
                           {"elevator_near_platform", fed.elevator_near_platform}, {"angle_deg", num(detail_all.angle_deg)},
                           {"derrickman_cx", num(detail_all.derrickman_cx)}, {"machine_cx", num(detail_all.machine_cx)},
                           {"pct", pct(diag.fed)}, {"pct_no_roi", pct(diag.all)}, {"pct_old_roi", pct(diag.old_roi)}, {"angles", diag.angles},
                           {"roi", roi ? json::array({std::lround(roi->x1), std::lround(roi->y1), std::lround(roi->x2), std::lround(roi->y2)}) : json(nullptr)}};
                emit_(ev);
                diag = FactsDiag{};
            }
        }

        json items = json::array();
        DrawResult draw;
        draw.model = worker.model;
        draw.key = frame->pts_key;
        const Detection* platform = nullptr; // the Platform the overlay is drawn for: highest confidence
        for (const Detection& d : found) {
            if (is_idss && same_name(detector->label(d.cls), roi_cfg.reference_class) && (!platform || d.conf > platform->conf)) platform = &d;
        }
        for (const Detection& d : found) {
            DrawItem draw_item;
            draw_item.label = detector->label(d.cls);
            // Burned-in PPE boxes: the model's own box for this frame when the track matched (the Kalman state lags a moving
            // person); a lost track is shown at its prediction, dashed, for at most kHeldDrawFrames frames, then not at all
            // (the presence check still uses it for the full track buffer).
            const bool show_raw = worker.model == "ppe" && track_cfg.enabled && d.has_raw;
            for (int k = 0; k < 4; ++k) draw_item.box[k] = static_cast<int>(std::lround(show_raw ? d.raw[k] : d.box[k]));
            draw_item.held = worker.model == "ppe" && track_cfg.enabled && d.held;
            const bool draw_it = !draw_item.held || d.lost_frames <= kHeldDrawFrames;
            json item = {{"cls", d.cls}, {"label", detector->label(d.cls)},
                         {"conf", std::round(static_cast<double>(d.conf) * 1000.0) / 1000.0},
                         {"box", {std::lround(d.box[0]), std::lround(d.box[1]), std::lround(d.box[2]), std::lround(d.box[3])}}};
            if (is_idss) {
                const Box b{d.box[0], d.box[1], d.box[2], d.box[3]};
                if (!d.mask.empty()) {
                    json polygon = json::array();
                    for (const cv::Point& p : mask_polygon(d.mask, cv::Point(static_cast<int>(d.box[0]), static_cast<int>(d.box[1])))) {
                        polygon.push_back({p.x, p.y});
                        draw_item.polygon.push_back({p.x, p.y});
                    }
                    item["polygon"] = polygon;
                }
                item["in_roi"] = !roi || box_centroid_in(b, *roi) || same_name(detector->label(d.cls), roi_cfg.reference_class);
                draw_item.in_roi = item["in_roi"].get<bool>();
            }
            if (worker.model == "ppe" && track_cfg.enabled) {
                item["track_id"] = d.track_id;
                if (d.held) item["held"] = true;
            }
            if (draw_it) draw.items.push_back(std::move(draw_item));
            items.push_back(item);
        }
        json event = {{"ev", "detections"}, {"cam", frame->cam}, {"model", worker.model}, {"ts", frame->ts_ms},
                      {"pts_ms", frame->pts_ms}, {"rtp", frame->rtp}, {"latency_ms", wall_ms() - frame->ts_ms}, {"w", frame->width}, {"h", frame->height}, {"ms", std::round(elapsed_us / 100.0) / 10.0}, {"items", items}};
        if (is_idss) {
            // Platform mask overlay, as IDSS draws it: the box, then the outline of the horizontally scaled mask.
            event["platform"] = nullptr;
            if (platform) {
                const cv::Rect rect(static_cast<int>(platform->box[0]), static_cast<int>(platform->box[1]),
                                    static_cast<int>(platform->box[2]) - static_cast<int>(platform->box[0]),
                                    static_cast<int>(platform->box[3]) - static_cast<int>(platform->box[1]));
                const PlatformOverlay overlay = platform_overlay(platform->mask, rect, frame->width, frame->height, roi_cfg.scale);
                json outline = json::array();
                draw.has_platform = true;
                draw.platform_box[0] = overlay.box.x;
                draw.platform_box[1] = overlay.box.y;
                draw.platform_box[2] = overlay.box.x + overlay.box.width;
                draw.platform_box[3] = overlay.box.y + overlay.box.height;
                for (const auto& contour : overlay.outline) {
                    json points = json::array();
                    std::vector<DrawPoint> drawn;
                    for (const cv::Point& p : contour) {
                        points.push_back({p.x, p.y});
                        drawn.push_back({p.x, p.y});
                    }
                    outline.push_back(points);
                    draw.outline.push_back(std::move(drawn));
                }
                event["platform"] = {{"box", {overlay.box.x, overlay.box.y, overlay.box.x + overlay.box.width, overlay.box.y + overlay.box.height}},
                                     {"outline", outline}};
            }
            if (roi) {
                draw.has_roi = true;
                draw.roi[0] = static_cast<int>(std::lround(roi->x1));
                draw.roi[1] = static_cast<int>(std::lround(roi->y1));
                draw.roi[2] = static_cast<int>(std::lround(roi->x2));
                draw.roi[3] = static_cast<int>(std::lround(roi->y2));
            }
            event["roi"] = roi ? json::array({std::lround(roi->x1), std::lround(roi->y1), std::lround(roi->x2), std::lround(roi->y2)}) : json(nullptr);
        }
        if (worker.model == "ppe" && ppe_cfg.enabled) {
            std::vector<NamedBox> present;
            for (const Detection& d : found) present.push_back({detector->label(d.cls), d.conf, {d.box[0], d.box[1], d.box[2], d.box[3]}, {}});
            std::vector<PpePerson> people;
            {
                std::lock_guard<std::mutex> lock(persons_mutex_);
                // Person boxes older than 1 s are not used: no person visible means no evaluation.
                if (persons_cam_ == frame->cam && std::llabs(frame->ts_ms - persons_ts_) < 1000) people = persons_;
            }
            if (const auto change = ppe_checker.update(frame->ts_ms, people, present)) {
                emit_({{"ev", "ppe"}, {"cam", frame->cam}, {"state", change->state}, {"missing", change->missing},
                       {"persons", change->persons}, {"since_ms", change->since_ms}, {"ts", frame->ts_ms}});
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - ppe_window.last >= std::chrono::seconds(4)) {
                const PpeStatus st = ppe_checker.take_status();
                auto pct = [](int n, int d) { return d > 0 ? std::round(1000.0 * n / d) / 10.0 : 0.0; };
                json present_pct = json::object(), detected_pct = json::object();
                for (const std::string& need : ppe_checker.config().required) {
                    const auto a = st.present_evals.find(need);
                    present_pct[need] = pct(a == st.present_evals.end() ? 0 : a->second, st.evals);
                }
                for (const auto& [name, n] : st.detected_frames) detected_pct[name] = pct(n, st.frames);
                emit_({{"ev", "ppe_status"}, {"cam", frame->cam}, {"frames", st.frames}, {"evals", st.evals}, {"persons_max", st.persons_max},
                       {"persons_seen_pct", pct(st.evals, st.frames)}, {"present_pct", present_pct}, {"detected_pct", detected_pct},
                       {"raised", ppe_checker.raised()}, {"tracking", track_cfg.enabled},
                       {"track_us_avg", ppe_window.track_n ? static_cast<double>(ppe_window.track_us) / static_cast<double>(ppe_window.track_n) : 0.0}});
                ppe_window.last = now;
                ppe_window.track_us = 0;
                ppe_window.track_n = 0;
            }
        }
        publish(std::move(draw)); // the video overlay waits for this: before the (slower) event goes out
        worker.warm = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sequence_ == seen) worker.pending = false; // no newer frame was handed over meanwhile
        }
        emit_(event);
    }
}

} // namespace rigmind
