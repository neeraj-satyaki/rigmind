#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backend/bridge.h"
#include "slm/scene_text.h"
#include "slm/slm_client.h"
#include "stream/pipeline_process.h"

// Sends describe requests to the SLM and every other stream command to the camera pipeline.
class StreamRouter : public rigmind::StreamSink {
public:
    using OnLine = std::function<void(const std::string&)>;

    StreamRouter(PipelineProcess* pipeline, SlmClient* slm, OnLine on_line)
        : pipeline_(pipeline), slm_(slm), on_line_(std::move(on_line)) {}

    // Latest "scene" event per camera (objects, mask outlines, centroid paths); kept for the summary prompt only.
    void note_scene(const nlohmann::json& scene) {
        std::lock_guard<std::mutex> lock(scene_mutex_);
        scenes_[scene.value("cam", std::string())] = scene;
    }

    // For the MCP module: the latest scene of a camera ("" = the only/any camera), and the cameras that have one.
    nlohmann::json scene(const std::string& cam) {
        std::lock_guard<std::mutex> lock(scene_mutex_);
        const auto it = cam.empty() ? scenes_.begin() : scenes_.find(cam);
        return it == scenes_.end() ? nlohmann::json(nullptr) : it->second;
    }
    std::vector<std::string> scene_cameras() {
        std::lock_guard<std::mutex> lock(scene_mutex_);
        std::vector<std::string> out;
        for (const auto& [cam, scene] : scenes_) out.push_back(cam);
        return out;
    }

    void send(const nlohmann::json& command) override {
        if (command.value("cmd", std::string()) == "describe") {
            if (slm_) {
                nlohmann::json with_scene = command;
                nlohmann::json scene;
                const std::string cam = command.value("camId", std::string());
                const std::int64_t scene_ts = command.value("sceneTs", std::int64_t{0}); // 0 = no scene for this alert
                if (!cam.empty() && scene_ts > 0) {
                    std::lock_guard<std::mutex> lock(scene_mutex_);
                    const auto it = scenes_.find(cam); // this alert's own camera only
                    if (it != scenes_.end()) scene = it->second;
                }
                // The scene must be from the moment of the alert (within 15 s of it), else it would describe something else.
                if (scene.is_object() && std::llabs(scene_ts - scene.value("ts", std::int64_t{0})) < 15000) {
                    const std::string text = rigmind::scene_prompt_text(scene);
                    if (!text.empty()) with_scene["prompt"] = command.value("prompt", std::string()) + "\n" + text;
                }
                slm_->describe(with_scene);
            } else {
                on_line_(nlohmann::json{{"ev", "description"}, {"alertId", command.value("alertId", std::string())},
                                        {"text", nullptr}, {"source", "template"}}.dump());
            }
            return;
        }
        if (pipeline_) {
            pipeline_->send(command);
            return;
        }
        on_line_(nlohmann::json{{"ev", "error"}, {"cam", command.value("cam", std::string())},
                                {"message", "pipeline not running"}}.dump());
    }

    // Describe commands only reach a model when the SLM runs; otherwise the page keeps the template text.
    bool can_describe() const override { return slm_ != nullptr && slm_->running(); }

private:
    static std::int64_t now_unix_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::mutex scene_mutex_;
    std::map<std::string, nlohmann::json> scenes_;
    PipelineProcess* pipeline_;
    SlmClient* slm_;
    OnLine on_line_;
};
