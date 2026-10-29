#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "backend/bridge.h"
#include "slm/slm_client.h"
#include "stream/pipeline_process.h"

// Sends describe requests to the SLM and every other stream command to the camera pipeline.
class StreamRouter : public rigmind::StreamSink {
public:
    using OnLine = std::function<void(const std::string&)>;

    StreamRouter(PipelineProcess* pipeline, SlmClient* slm, OnLine on_line)
        : pipeline_(pipeline), slm_(slm), on_line_(std::move(on_line)) {}

    void send(const nlohmann::json& command) override {
        if (command.value("cmd", std::string()) == "describe") {
            if (slm_) {
                slm_->describe(command);
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
    PipelineProcess* pipeline_;
    SlmClient* slm_;
    OnLine on_line_;
};
