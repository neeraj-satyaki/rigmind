// rigmind-bridge-cli: runs the C++ backend (alarm state machine, SQLite store, JSON bridge)
// for the Linux host. One request per stdin line, one reply per stdout line.
//
// With --stream, stream commands are written to stdout as {"forward": {...}} lines before the reply.
// host.py relays them to the rigmind-pipeline child and does not treat them as replies.
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "backend/audit_log.h"
#include "backend/bridge.h"
#include "backend/event_store.h"

namespace {

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

class StdoutStreamSink : public rigmind::StreamSink {
public:
    void send(const nlohmann::json& command) override {
        std::cout << nlohmann::json{{"forward", command}}.dump() << '\n' << std::flush;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool stream = false, fast_auth = false;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--stream") stream = true;
        else if (flag == "--fast-auth") fast_auth = true; // generated test cases: 1000 password rounds instead of 600000
        else argc = 0;
    }
    if (argc < 2) {
        std::cerr << "usage: rigmind-bridge-cli <db-path> [--stream] [--fast-auth]\n";
        return 2;
    }
    try {
        rigmind::EventStore store(argv[1]);
        const auto data_dir = std::filesystem::path(argv[1]).parent_path();
        store.set_footage_root((data_dir / "footage").string());
        store.prune_footage(now_ms()); // 7-day clearance at every start
        rigmind::AuditLog audit(argv[1]);
        StdoutStreamSink sink;
        rigmind::Bridge bridge(store, stream ? &sink : nullptr, &audit);
        if (fast_auth) bridge.set_auth_rounds(1000);
        audit.record("backend", "app.start", "", "bridge-cli", now_ms());
        std::string line;
        while (std::getline(std::cin, line)) {
            std::cout << bridge.handle(line) << '\n' << std::flush;
        }
        // stdin closed: the host exited. AuditLog's destructor drains the queue after this.
        audit.record("backend", "app.stop", "", "bridge-cli", now_ms());
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
