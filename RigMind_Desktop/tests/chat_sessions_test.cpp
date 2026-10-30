// Evidence chat sessions: LZ4 block codec, TOON text, history table and the compressed store (backend/chat_sessions.h).
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>

#include "backend/chat_sessions.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
using json = nlohmann::json;
using rigmind::chat_detail::Bytes;
Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
bool roundtrip(const Bytes& in) { return rigmind::chat_detail::lz4_decompress(rigmind::chat_detail::lz4_compress(in), in.size() + 16) == in; }
bool throws(const std::function<void()>& fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}
} // namespace

int main() {
    using namespace rigmind;

    // ---- LZ4: every small size, repetitive and random data
    bool small_ok = true;
    for (std::size_t n = 0; n <= 70; ++n) small_ok = small_ok && roundtrip(Bytes(n, 'a')) && roundtrip(bytes(std::string("abcdefghij").substr(0, n % 11)));
    check(small_ok, "tiny inputs round trip");
    Bytes pattern;
    for (int i = 0; i < 5000; ++i) { for (char c : std::string("Orange image set, CAM-01, 30 Oct 14:22, 3 files\n")) pattern.push_back(static_cast<std::uint8_t>(c)); }
    check(roundtrip(pattern), "repetitive text round trips");
    const Bytes packed = chat_detail::lz4_compress(pattern);
    check(packed.size() * 20 < pattern.size(), "repetitive text compresses at least 20x");
    std::mt19937 rng(7);
    Bytes noise(20000);
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng());
    check(roundtrip(noise), "random bytes round trip");
    Bytes mixed;
    for (int i = 0; i < 3000; ++i) { mixed.push_back(static_cast<std::uint8_t>(rng() % 7 + 'a')); if (i % 97 == 0) for (int k = 0; k < 300; ++k) mixed.push_back('z'); }
    check(roundtrip(mixed), "mixed data with long runs round trips");

    // ---- LZ4: hostile blobs are refused, never crash
    check(throws([&] { chat_detail::lz4_decompress(Bytes{0xF0, 0xFF, 0xFF}, 100); }), "truncated length field");
    check(throws([&] { chat_detail::lz4_decompress(Bytes{0x10, 'a', 0x05, 0x00}, 100); }), "match offset beyond the output");
    check(throws([&] { chat_detail::lz4_decompress(Bytes{0x1F, 'a', 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 100); }), "a match that would exceed the limit");
    check(throws([&] { chat_detail::lz4_decompress(chat_detail::lz4_compress(Bytes(5000, 'x')), 100); }), "output limit is enforced");
    check(throws([&] { chat_detail::lz4_decompress(Bytes{0x20, 'a'}, 100); }), "literals longer than the input");

    // ---- base64
    check(chat_detail::b64_decode(chat_detail::b64_encode(noise)) == noise, "base64 round trip");
    check(throws([] { chat_detail::b64_decode("ab$d"); }), "bad base64 refused");

    // ---- TOON
    const json session = {{"id", "chat-1793350001000-1"}, {"title", "Latest Orange evidence"}, {"started", 1793350001000LL},
        {"turns", json::array({
            {{"t", 1793350001000LL}, {"role", "you"}, {"text", "Latest Orange evidence"}, {"ev", json::array()}},
            {{"t", 1793350002000LL}, {"role", "assistant"}, {"text", "Found 2 Orange image sets, in today."}, {"ev", json::array({"CAM-01/1793349677358", "CAM-01/1793349000000"})}},
            {{"t", 1793350003000LL}, {"role", "you"}, {"text", "tricky: \"quotes\", commas,\nnew line, back\\slash, colon: ok, caf\xC3\xA9 \xE2\x9C\x93 "}, {"ev", json::array()}},
        })}};
    const std::string toon = toon_encode_session(session);
    check(toon.find("turns[3]{t,role,text,ev}:") != std::string::npos, "TOON header with row count and columns");
    check(toon.find("CAM-01/1793349677358|CAM-01/1793349000000") != std::string::npos, "evidence paths are one cell");
    check(toon.size() < session.dump().size(), "TOON is shorter than the same session as JSON");
    const json back = toon_decode_session(toon);
    check(back["id"] == session["id"] && back["title"] == session["title"] && back["started"] == session["started"], "header fields round trip");
    check(back["turns"] == session["turns"], "every turn round trips, including quotes, commas, newline, backslash, colon, unicode and trailing space");
    check(toon_decode_session(toon_encode_session({{"id", "x"}, {"title", ""}, {"started", 0}, {"turns", json::array()}}))["turns"].empty(), "empty session");
    check(throws([&] { toon_decode_session("id: x\nturns[2]{t,role,text,ev}:\n  1,you,hi,\n"); }), "row count mismatch is refused");
    check(throws([&] { toon_decode_session("id: x\nturns[1]{t,role,text,ev}:\n  1,you,hi\n"); }), "short row is refused");

    // ---- history for a prompt
    json turns = json::array();
    for (int i = 0; i < 10; ++i) turns.push_back({{"role", i % 2 ? "assistant" : "you"}, {"text", "turn " + std::to_string(i)}});
    const std::string hist = toon_history(turns, 4);
    check(hist.find("history[4]{role,text}:") == 0 && hist.find("turn 9") != std::string::npos && hist.find("turn 5") == std::string::npos, "history keeps the newest turns");
    check(hist.find("user,turn 8") != std::string::npos && hist.find("assistant,turn 9") != std::string::npos, "roles are user and assistant");
    check(toon_history(json::array()).empty(), "no history gives no text");
    check(toon_history(json::array({{{"role", "you"}, {"text", std::string(500, 'x')}}}), 6, 100).size() < 200, "each turn is capped");

    // ---- the store
    const auto dir = std::filesystem::temp_directory_path() / ("rigmind-chat-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    {
        EventStore store((dir / "rigmind.db").string());
        ChatSessions chats(store);
        const json meta = chats.save(session, 5000);
        check(meta["turns"] == 3 && meta["title"] == "Latest Orange evidence" && meta["bytes"].get<int>() > 0, "save returns the list row");
        check(chats.load("chat-1793350001000-1")["turns"] == session["turns"], "load returns the same turns");
        check(chats.toon("chat-1793350001000-1") == toon, "toon() gives the stored TOON text");
        check(chats.list().size() == 1 && chats.list()[0]["id"] == "chat-1793350001000-1", "list shows it");
        // the stored value is a compressed blob, not the text
        const auto raw = store.get_record("chat:v1:chat-1793350001000-1");
        check(raw && raw->find("Orange") == std::string::npos, "the stored record is not readable text");

        // saving again replaces; bad turns are dropped; long text is capped; bad evidence paths are dropped
        json messy = {{"id", "chat-b"}, {"turns", json::array({
            {{"role", "system"}, {"text", "ignored"}},
            {{"role", "you"}, {"text", std::string(5000, 'q')}, {"ev", json::array({"CAM-01/1", "bad|path", 5})}},
            "not an object"})}};
        const json m2 = chats.save(messy, 6000);
        check(m2["turns"] == 1, "only valid turns are kept");
        const json l2 = chats.load("chat-b");
        check(l2["turns"][0]["text"].get<std::string>().size() == 2000, "text capped at 2000");
        check(l2["turns"][0]["ev"] == json::array({"CAM-01/1"}), "only well formed evidence paths are kept");
        check(chats.list()[0]["id"] == "chat-b", "newest first");

        // a cut at the length limit never lands inside a multi-byte character (this once produced invalid UTF-8)
        std::string accents, wide_title;
        for (int i = 0; i < 1100; ++i) accents += "\xC3\xA9";                 // 2200 bytes of e-acute
        for (int i = 0; i < 40; ++i) wide_title += "\xE6\x97\xA5";            // 120 bytes of CJK
        chats.save({{"id", "chat-utf8"}, {"turns", json::array({{{"role", "you"}, {"text", wide_title}, {"t", 1}}, {{"role", "assistant"}, {"text", accents}, {"t", 2}}})}}, 7000);
        const json u = chats.load("chat-utf8");
        check(u["turns"][1]["text"].get<std::string>().size() == 2000 && u["turns"][1]["text"].get<std::string>().size() % 2 == 0, "text is cut on a character boundary (even number of bytes for 2-byte characters)");
        check(u["title"].get<std::string>().size() == 60 && u["title"].get<std::string>().size() % 3 == 0, "title is cut on a character boundary (3-byte characters)");
        check(u.dump().size() > 0, "the loaded session serialises");

        // ids are validated
        for (const char* bad : {"", "../x", "a b", "chat/1"}) check(throws([&] { chats.load(bad); }), "invalid chat id refused");
        check(throws([&] { chats.load("chat-missing"); }), "unknown chat refused");

        // remove
        chats.remove("chat-b");
        check(throws([&] { chats.load("chat-b"); }) || chats.load("chat-b")["turns"].empty(), "a removed session cannot be loaded");
        bool still_listed = false;
        for (const auto& row : chats.list()) still_listed = still_listed || row["id"] == "chat-b";
        check(!still_listed, "list no longer shows the removed chat");

        // trim: only the newest 50 are kept
        for (int i = 0; i < 55; ++i) chats.save({{"id", "chat-n" + std::to_string(i)}, {"turns", json::array({{{"role", "you"}, {"text", "q"}, {"t", i}}})}}, 10000 + i);
        const json all = chats.list();
        check(all.size() == ChatSessions::kMaxSessions && all[0]["id"] == "chat-n54", "the oldest sessions are trimmed");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    if (failures) return 1;
    std::cout << "chat_sessions tests passed\n";
    return 0;
}
