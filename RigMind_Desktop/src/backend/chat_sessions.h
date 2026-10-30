// Evidence chat sessions kept as small compressed blobs.
//
// A session is written as TOON (Token-Oriented Object Notation: a header plus a table of rows, far fewer tokens than JSON) and that
// text is LZ4 block compressed; the blob is stored base64 in the app_records table (keys chat:v1:<id>, with a tiny chat:meta:v1:<id>
// row for listing). Reading a session back is one decompress and one parse, and the TOON text itself is what goes into a model
// prompt or an MCP answer, so earlier turns cost few tokens.
//
//   id: chat-1793350001000-1
//   title: Latest Orange evidence
//   started: 1793350001000
//   turns[2]{t,role,text,ev}:
//     1793350001000,you,Latest Orange evidence,
//     1793350002000,assistant,"Found 2 Orange image sets, in today.",CAM-01/1793349677358|CAM-01/1793349000000
//
// Header-only apart from the store access; the codecs are pure and unit tested.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "event_store.h"

namespace rigmind {

namespace chat_detail {

using Bytes = std::vector<std::uint8_t>;

// ---- LZ4 block format (compress: greedy matcher with a 4096-entry hash table; decompress: fully bounds checked) ----------------

inline Bytes lz4_compress(const Bytes& in) {
    Bytes out;
    const std::size_t n = in.size();
    out.reserve(n / 2 + 16);
    std::vector<std::uint32_t> table(4096, 0xFFFFFFFFu);
    auto read32 = [&](std::size_t i) { return static_cast<std::uint32_t>(in[i]) | (static_cast<std::uint32_t>(in[i + 1]) << 8) | (static_cast<std::uint32_t>(in[i + 2]) << 16) | (static_cast<std::uint32_t>(in[i + 3]) << 24); };
    auto put_len = [&](std::size_t len) { // the 255-run continuation of a length field
        while (len >= 255) { out.push_back(255); len -= 255; }
        out.push_back(static_cast<std::uint8_t>(len));
    };
    std::size_t anchor = 0, i = 0;
    // The last 5 bytes are always literals and a match must start at least 12 bytes before the end (LZ4 block rules).
    const std::size_t match_limit = n >= 12 ? n - 12 : 0;
    while (n >= 13 && i < match_limit) {
        const std::uint32_t v = read32(i);
        const std::uint32_t h = (v * 2654435761u) >> 20;
        const std::uint32_t ref = table[h];
        table[h] = static_cast<std::uint32_t>(i);
        if (ref != 0xFFFFFFFFu && i - ref <= 65535 && read32(ref) == v) {
            std::size_t len = 4;
            while (i + len < n - 5 && in[ref + len] == in[i + len]) ++len;
            const std::size_t lit = i - anchor;
            const std::size_t token_at = out.size();
            out.push_back(0);
            if (lit >= 15) put_len(lit - 15);
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(anchor), in.begin() + static_cast<std::ptrdiff_t>(i));
            const std::size_t offset = i - ref;
            out.push_back(static_cast<std::uint8_t>(offset & 255));
            out.push_back(static_cast<std::uint8_t>(offset >> 8));
            if (len - 4 >= 15) put_len(len - 4 - 15);
            out[token_at] = static_cast<std::uint8_t>((std::min<std::size_t>(lit, 15) << 4) | std::min<std::size_t>(len - 4, 15));
            i += len;
            anchor = i;
        } else {
            ++i;
        }
    }
    // final literals
    const std::size_t lit = n - anchor;
    const std::size_t token_at = out.size();
    out.push_back(0);
    if (lit >= 15) put_len(lit - 15);
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(anchor), in.end());
    out[token_at] = static_cast<std::uint8_t>(std::min<std::size_t>(lit, 15) << 4);
    return out;
}

// max_out bounds the result (a hostile or damaged blob cannot grow without limit). Throws std::runtime_error on bad input.
inline Bytes lz4_decompress(const Bytes& in, std::size_t max_out) {
    Bytes out;
    std::size_t i = 0;
    auto need = [&](std::size_t k) { if (i + k > in.size()) throw std::runtime_error("damaged chat blob"); };
    auto read_len = [&](std::size_t base) {
        std::size_t len = base;
        if (base == 15) {
            std::uint8_t b;
            do { need(1); b = in[i++]; len += b; if (len > max_out) throw std::runtime_error("chat blob too large"); } while (b == 255);
        }
        return len;
    };
    while (i < in.size()) {
        const std::uint8_t token = in[i++];
        const std::size_t lit = read_len(token >> 4);
        need(lit);
        if (out.size() + lit > max_out) throw std::runtime_error("chat blob too large");
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(i + lit));
        i += lit;
        if (i >= in.size()) break; // the last sequence has no match
        need(2);
        const std::size_t offset = static_cast<std::size_t>(in[i]) | (static_cast<std::size_t>(in[i + 1]) << 8);
        i += 2;
        if (offset == 0 || offset > out.size()) throw std::runtime_error("damaged chat blob");
        const std::size_t len = read_len(token & 15) + 4;
        if (out.size() + len > max_out) throw std::runtime_error("chat blob too large");
        std::size_t from = out.size() - offset;
        for (std::size_t k = 0; k < len; ++k) out.push_back(out[from + k]); // byte by byte: matches may overlap
    }
    return out;
}

// ---- base64 --------------------------------------------------------------------------------------------------------------------

inline std::string b64_encode(const Bytes& b) {
    static const char* d = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        const unsigned x = b[i], y = i + 1 < b.size() ? b[i + 1] : 0, z = i + 2 < b.size() ? b[i + 2] : 0;
        o += d[x >> 2];
        o += d[((x & 3) << 4) | (y >> 4)];
        o += i + 1 < b.size() ? d[((y & 15) << 2) | (z >> 6)] : '=';
        o += i + 2 < b.size() ? d[z & 63] : '=';
    }
    return o;
}

inline Bytes b64_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    Bytes o;
    int acc = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) throw std::runtime_error("damaged chat blob");
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back(static_cast<std::uint8_t>((acc >> bits) & 255)); }
    }
    return o;
}

// Cuts at `max` bytes or earlier, never inside a multi-byte character: the result is always valid UTF-8.
inline std::string cut_utf8(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s;
}

// ---- TOON ---------------------------------------------------------------------------------------------------------------------

// A value is quoted when it holds a comma, quote, backslash, line break, colon, or leading/trailing space (or is empty and meant as text).
inline std::string toon_cell(const std::string& s) {
    bool quote = s.find_first_of(",\"\\\n\r:") != std::string::npos || (!s.empty() && (s.front() == ' ' || s.back() == ' '));
    if (!quote) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o + "\"";
}

// Splits one row on commas outside quotes and unescapes quoted cells.
inline std::vector<std::string> toon_row(const std::string& line) {
    std::vector<std::string> cells;
    std::string cur;
    bool in_quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quote) {
            if (c == '\\' && i + 1 < line.size()) {
                const char n = line[++i];
                cur += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
            } else if (c == '"') in_quote = false;
            else cur += c;
        } else if (c == '"') in_quote = true;
        else if (c == ',') { cells.push_back(cur); cur.clear(); }
        else cur += c;
    }
    cells.push_back(cur);
    return cells;
}

} // namespace chat_detail

// {id,title,started,turns:[{t,role,text,ev:[path,...]}]} -> TOON text
inline std::string toon_encode_session(const nlohmann::json& s) {
    using chat_detail::toon_cell;
    std::string out = "id: " + toon_cell(s.value("id", std::string())) + "\ntitle: " + toon_cell(s.value("title", std::string())) +
                      "\nstarted: " + std::to_string(s.value("started", std::int64_t{0})) + "\n";
    const nlohmann::json turns = s.value("turns", nlohmann::json::array());
    out += "turns[" + std::to_string(turns.size()) + "]{t,role,text,ev}:\n";
    for (const auto& t : turns) {
        std::string ev;
        if (t.contains("ev") && t["ev"].is_array())
            for (const auto& p : t["ev"]) ev += (ev.empty() ? "" : "|") + p.get<std::string>();
        out += "  " + std::to_string(t.value("t", std::int64_t{0})) + "," + toon_cell(t.value("role", std::string())) + "," + toon_cell(t.value("text", std::string())) + "," + toon_cell(ev) + "\n";
    }
    return out;
}

inline nlohmann::json toon_decode_session(const std::string& text) {
    using chat_detail::toon_row;
    nlohmann::json s = {{"id", ""}, {"title", ""}, {"started", 0}, {"turns", nlohmann::json::array()}};
    std::size_t pos = 0;
    int expected = -1;
    bool in_table = false;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("turns[", 0) == 0) {
            expected = std::atoi(line.c_str() + 6);
            in_table = true;
            continue;
        }
        if (in_table) {
            const std::size_t first = line.find_first_not_of(' ');
            if (first == std::string::npos) continue;
            const auto cells = toon_row(line.substr(first));
            if (cells.size() != 4) throw std::runtime_error("damaged chat session");
            nlohmann::json t = {{"t", std::atoll(cells[0].c_str())}, {"role", cells[1]}, {"text", cells[2]}, {"ev", nlohmann::json::array()}};
            std::size_t from = 0;
            while (!cells[3].empty() && from <= cells[3].size()) {
                const std::size_t bar = cells[3].find('|', from);
                t["ev"].push_back(cells[3].substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
            s["turns"].push_back(std::move(t));
            continue;
        }
        const std::size_t colon = line.find(": ");
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const auto cell = toon_row(line.substr(colon + 2));
        const std::string value = cell.empty() ? std::string() : cell[0];
        if (key == "id") s["id"] = value;
        else if (key == "title") s["title"] = value;
        else if (key == "started") s["started"] = std::atoll(value.c_str());
    }
    if (expected >= 0 && static_cast<int>(s["turns"].size()) != expected) throw std::runtime_error("damaged chat session");
    return s;
}

// A few earlier turns as a compact table for a model prompt (no id/title header): "role,text" rows, each text capped.
inline std::string toon_history(const nlohmann::json& turns, std::size_t max_turns = 6, std::size_t max_text = 220, std::size_t max_chars = 900) {
    using chat_detail::toon_cell;
    std::vector<nlohmann::json> pick;
    for (const auto& t : turns) pick.push_back(t);
    if (pick.size() > max_turns) pick.erase(pick.begin(), pick.end() - static_cast<std::ptrdiff_t>(max_turns));
    std::string out = "history[" + std::to_string(pick.size()) + "]{role,text}:\n";
    for (const auto& t : pick) {
        std::string text = t.value("text", std::string());
        if (text.size() > max_text) {
            std::size_t cut = max_text;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            text.resize(cut);
        }
        const std::string role = t.value("role", std::string()) == "you" ? "user" : "assistant";
        out += "  " + role + "," + toon_cell(text) + "\n";
    }
    if (out.size() > max_chars) out.resize(out.find_last_of('\n', max_chars) == std::string::npos ? max_chars : out.find_last_of('\n', max_chars) + 1);
    return pick.empty() ? std::string() : out;
}

// The store: sessions live in app_records (chat:v1:<id> = base64 of LZ4 of the TOON text; chat:meta:v1:<id> = list row).
class ChatSessions {
public:
    static constexpr std::size_t kMaxTurns = 200, kMaxText = 2000, kMaxSessions = 50, kMaxBlob = 1 << 20;
    explicit ChatSessions(EventStore& store) : store_(store) {}

    // session: {id, turns:[{t,role,text,ev}]}; stores it (replacing the same id) and trims the oldest sessions. Returns the list row.
    nlohmann::json save(nlohmann::json session, std::int64_t now_ms) {
        const std::string id = session.value("id", std::string());
        check_id(id);
        nlohmann::json turns = nlohmann::json::array();
        if (session.contains("turns") && session["turns"].is_array()) {
            for (const auto& t : session["turns"]) {
                if (!t.is_object()) continue;
                const std::string role = t.value("role", std::string());
                if (role != "you" && role != "assistant") continue;
                std::string text = t.contains("text") && t["text"].is_string() ? t["text"].get<std::string>() : std::string();
                text = chat_detail::cut_utf8(std::move(text), kMaxText);
                nlohmann::json ev = nlohmann::json::array();
                if (t.contains("ev") && t["ev"].is_array())
                    for (const auto& p : t["ev"]) if (p.is_string() && !p.get<std::string>().empty() && p.get<std::string>().size() <= 96 && p.get<std::string>().find_first_of(",|\n\"") == std::string::npos) ev.push_back(p);
                turns.push_back({{"t", t.value("t", std::int64_t{0})}, {"role", role}, {"text", text}, {"ev", ev}});
            }
        }
        if (turns.size() > kMaxTurns) turns.erase(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(turns.size() - kMaxTurns));
        std::string title;
        for (const auto& t : turns) if (t["role"] == "you") { title = t["text"].get<std::string>(); break; }
        title = chat_detail::cut_utf8(std::move(title), 60);
        const std::int64_t started = turns.empty() ? now_ms : turns.front().value("t", now_ms);
        const std::string toon = toon_encode_session({{"id", id}, {"title", title}, {"started", started}, {"turns", turns}});
        const auto blob = chat_detail::lz4_compress(chat_detail::Bytes(toon.begin(), toon.end()));
        store_.put_record("chat:v1:" + id, nlohmann::json(chat_detail::b64_encode(blob)).dump());
        const nlohmann::json meta = {{"id", id}, {"title", title}, {"startedMs", started}, {"updatedMs", now_ms}, {"turns", turns.size()}, {"bytes", blob.size()}, {"textBytes", toon.size()}};
        store_.put_record("chat:meta:v1:" + id, meta.dump());
        trim();
        return meta;
    }

    nlohmann::json list() const { // newest first
        std::vector<nlohmann::json> rows;
        for (const auto& rec : store_.list_records("chat:meta:v1:")) if (rec["value"].is_object()) rows.push_back(rec["value"]); // removed sessions leave null
        std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a.value("updatedMs", std::int64_t{0}) > b.value("updatedMs", std::int64_t{0}); });
        return nlohmann::json(rows);
    }

    std::string toon(const std::string& id) const {
        check_id(id);
        const auto raw = store_.get_record("chat:v1:" + id);
        if (!raw) throw std::runtime_error("no such chat");
        const auto bytes = chat_detail::lz4_decompress(chat_detail::b64_decode(nlohmann::json::parse(*raw).get<std::string>()), kMaxBlob);
        return std::string(bytes.begin(), bytes.end());
    }

    nlohmann::json load(const std::string& id) const { return toon_decode_session(toon(id)); }

    void remove(const std::string& id) {
        check_id(id);
        store_.put_record("chat:v1:" + id, "null");
        store_.put_record("chat:meta:v1:" + id, "null");
    }

private:
    static void check_id(const std::string& id) {
        if (id.empty() || id.size() > 48) throw std::runtime_error("invalid chat id");
        for (char c : id) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) throw std::runtime_error("invalid chat id");
    }
    void trim() {
        std::vector<nlohmann::json> rows;
        for (const auto& rec : store_.list_records("chat:meta:v1:")) if (rec["value"].is_object()) rows.push_back(rec["value"]);
        if (rows.size() <= kMaxSessions) return;
        std::sort(rows.begin(), rows.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a.value("updatedMs", std::int64_t{0}) < b.value("updatedMs", std::int64_t{0}); });
        for (std::size_t i = 0; i + kMaxSessions < rows.size(); ++i) remove(rows[i].value("id", std::string()));
    }

    EventStore& store_;
};

} // namespace rigmind
