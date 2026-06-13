#pragma once
// ─── Replay.h ──────────────────────────────────────────────────────────────
//
// Header-only duel replay format. Stores enough metadata to identify a duel
// and enough recorded events to display a step-by-step timeline. Full
// engine playback (re-feeding responses to ocgcore) is deferred to a future
// round — the goal here is to capture *what happened* so duels can be
// browsed, audited and copied for bug reports.
//
// File layout: assets/replays/<filename>.json — one duel per file. The
// JSON writer is hand-rolled (no external dependency) and emits a stable
// human-readable shape; the reader is permissive about whitespace, comma
// trailing, and unknown keys so older replays survive future extensions.
//
// Top-level shape:
//   { "version": 1,
//     "app": "EdoPro+",
//     "timestamp": "YYYY-MM-DD HH:MM:SS",
//     "seed": "<u64>",
//     "rule_flags": "0x...",
//     "lp": 8000, "hand_count": 5, "draw_count": 1,
//     "deck1": { "name": "...", "path": "...",
//                "main": [N,N,...], "extra": [...], "side": [...] },
//     "deck2": { ... },
//     "winner": -1|0|1,
//     "turns": N,
//     "duration_sec": F,
//     "final_lp": [N, N],
//     "card_db": "<primary path>",
//     "script_count": N,
//     "responses": [ { "t": F, "hex": "..." }, ... ],
//     "events":    [ { "t": F, "text": "..." }, ... ] }
//
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace edo {

struct ReplayDeck {
    std::string                name;
    std::string                path;
    std::vector<uint32_t>      main;
    std::vector<uint32_t>      extra;
    std::vector<uint32_t>      side;
};

struct ReplayResponse {
    double                     t = 0.0;
    std::vector<uint8_t>       data;
};

struct ReplayEvent {
    double                     t = 0.0;
    std::string                text;
};

struct Replay {
    // ── Metadata ─────────────────────────────────────────────────────────
    int          version       = 1;
    std::string  app           = "EdoPro+";
    std::string  timestamp;          // local time, "YYYY-MM-DD HH:MM:SS"
    uint64_t     seed          = 0;
    uint64_t     ruleFlags     = 0;
    uint32_t     lp            = 8000;
    uint32_t     handCount     = 5;
    uint32_t     drawCount     = 1;
    ReplayDeck   deck1;
    ReplayDeck   deck2;
    int          winner        = -2;     // -2 unknown, -1 draw, 0/1 player idx
    int          turns         = 0;
    double       durationSec   = 0.0;
    uint32_t     finalLP[2]    = {0, 0};
    std::string  cardDb;
    int          scriptCount   = 0;
    std::vector<ReplayResponse> responses;
    std::vector<ReplayEvent>    events;

    // Canonical replays directory. Created on first save.
    static std::string defaultDir() { return "assets/replays"; }

    // Format the current local time as "YYYY-MM-DD HH:MM:SS".
    static std::string nowTimestamp() {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[24];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        return buf;
    }

    // Build a default filename like "2026-06-04_103015_VSK9_vs_Salamangreat.json".
    std::string suggestedFilename() const {
        std::string ts = timestamp;
        for (char& c : ts) if (c == ' ' || c == ':' || c == '-') c = '_';
        auto deckPart = [](const std::string& s) {
            std::string r = s;
            // Strip directory prefix and ".ydk" suffix; sanitise.
            auto sl = r.find_last_of("/\\");
            if (sl != std::string::npos) r = r.substr(sl + 1);
            if (r.size() > 4 && r.substr(r.size() - 4) == ".ydk")
                r = r.substr(0, r.size() - 4);
            for (char& c : r) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9'))) c = '_';
            }
            if (r.empty()) r = "deck";
            return r;
        };
        std::string a = deckPart(deck1.path.empty() ? deck1.name : deck1.path);
        std::string b = deckPart(deck2.path.empty() ? deck2.name : deck2.path);
        return ts + "_" + a + "_vs_" + b + ".json";
    }

    // ── Save ─────────────────────────────────────────────────────────────
    bool save(const std::string& path) const {
        std::error_code ec;
        auto dir = std::filesystem::path(path).parent_path();
        if (!dir.empty())
            std::filesystem::create_directories(dir, ec);
        std::ofstream f(path);
        if (!f) return false;
        f << "{\n";
        f << "  \"version\": " << version << ",\n";
        f << "  \"app\": \""        << escape(app)       << "\",\n";
        f << "  \"timestamp\": \""  << escape(timestamp) << "\",\n";
        f << "  \"seed\": \""       << seed              << "\",\n";
        char fb[24];
        std::snprintf(fb, sizeof(fb), "0x%llx",
                      (unsigned long long)ruleFlags);
        f << "  \"rule_flags\": \"" << fb << "\",\n";
        f << "  \"lp\": "           << lp        << ",\n";
        f << "  \"hand_count\": "   << handCount << ",\n";
        f << "  \"draw_count\": "   << drawCount << ",\n";
        writeDeck(f, "deck1", deck1, true);
        writeDeck(f, "deck2", deck2, true);
        f << "  \"winner\": "         << winner          << ",\n";
        f << "  \"turns\": "          << turns           << ",\n";
        f << "  \"duration_sec\": "   << durationSec     << ",\n";
        f << "  \"final_lp\": ["
          << finalLP[0] << ", " << finalLP[1] << "],\n";
        f << "  \"card_db\": \""      << escape(cardDb)  << "\",\n";
        f << "  \"script_count\": "   << scriptCount     << ",\n";
        // Responses array — one short object per response. Hex-encoded so
        // arbitrary byte payloads can survive a text round-trip.
        f << "  \"responses\": [";
        for (size_t i = 0; i < responses.size(); ++i) {
            const auto& r = responses[i];
            f << (i ? ",\n    " : "\n    ");
            f << "{\"t\": " << r.t << ", \"hex\": \"" << toHex(r.data) << "\"}";
        }
        f << (responses.empty() ? "],\n" : "\n  ],\n");
        // Events array — readable game-log lines with timestamps.
        f << "  \"events\": [";
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            f << (i ? ",\n    " : "\n    ");
            f << "{\"t\": " << e.t << ", \"text\": \"" << escape(e.text) << "\"}";
        }
        f << (events.empty() ? "]\n}" : "\n  ]\n}");
        return f.good();
    }

    // ── Load ─────────────────────────────────────────────────────────────
    // Permissive parser: handles standard JSON without nested arrays of
    // objects (except the explicit responses/events arrays we emit), tolerates
    // whitespace, and skips unknown keys. Returns false on read failure;
    // partial parse results are still applied to the struct so a corrupt
    // tail doesn't lose all earlier metadata.
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        Parser p(s);
        return p.parseObject(*this);
    }

    // ── List replays in a folder ─────────────────────────────────────────
    // Returns a list of file paths sorted newest-first (by mtime).
    static std::vector<std::string> list(const std::string& dir = defaultDir()) {
        std::vector<std::string> out;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return out;
        struct Entry { std::string path; std::filesystem::file_time_type t; };
        std::vector<Entry> tmp;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".json") continue;
            Entry x; x.path = e.path().string();
            std::error_code ec2;
            x.t = std::filesystem::last_write_time(e.path(), ec2);
            tmp.push_back(std::move(x));
        }
        std::sort(tmp.begin(), tmp.end(),
                  [](const Entry& a, const Entry& b){ return a.t > b.t; });
        for (auto& x : tmp) out.push_back(std::move(x.path));
        return out;
    }

private:
    // ── Helpers ──────────────────────────────────────────────────────────
    static std::string escape(const std::string& s) {
        std::string out; out.reserve(s.size() + 4);
        for (char c : s) {
            switch (c) {
                case '"' : out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf),
                                      "\\u%04x", (unsigned)(unsigned char)c);
                        out += buf;
                    } else out += c;
            }
        }
        return out;
    }
    static std::string toHex(const std::vector<uint8_t>& data) {
        static const char* H = "0123456789abcdef";
        std::string out(data.size() * 2, '0');
        for (size_t i = 0; i < data.size(); ++i) {
            out[i * 2    ] = H[(data[i] >> 4) & 0xF];
            out[i * 2 + 1] = H[ data[i]       & 0xF];
        }
        return out;
    }
    static std::vector<uint8_t> fromHex(const std::string& s) {
        std::vector<uint8_t> out;
        out.reserve(s.size() / 2);
        for (size_t i = 0; i + 1 < s.size(); i += 2) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out.push_back((uint8_t)((hex(s[i]) << 4) | hex(s[i + 1])));
        }
        return out;
    }
    static void writeDeck(std::ostream& f, const char* key,
                          const ReplayDeck& d, bool /*trailingComma*/) {
        f << "  \"" << key << "\": {\n";
        f << "    \"name\": \"" << escape(d.name) << "\",\n";
        f << "    \"path\": \"" << escape(d.path) << "\",\n";
        auto writeArr = [&](const char* k,
                            const std::vector<uint32_t>& v, bool comma) {
            f << "    \"" << k << "\": [";
            for (size_t i = 0; i < v.size(); ++i)
                f << (i ? ", " : "") << v[i];
            f << (comma ? "],\n" : "]\n");
        };
        writeArr("main",  d.main,  true);
        writeArr("extra", d.extra, true);
        writeArr("side",  d.side,  false);
        f << "  },\n";
    }

    // ── Minimal JSON parser ──────────────────────────────────────────────
    struct Parser {
        const std::string& s;
        size_t pos = 0;
        explicit Parser(const std::string& str) : s(str) {}
        void skipWS() {
            while (pos < s.size()) {
                char c = s[pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
                else break;
            }
        }
        bool match(char c) {
            skipWS();
            if (pos < s.size() && s[pos] == c) { ++pos; return true; }
            return false;
        }
        bool parseString(std::string& out) {
            skipWS();
            if (pos >= s.size() || s[pos] != '"') return false;
            ++pos;
            out.clear();
            while (pos < s.size() && s[pos] != '"') {
                if (s[pos] == '\\' && pos + 1 < s.size()) {
                    char n = s[pos + 1];
                    if      (n == '"')  out += '"';
                    else if (n == '\\') out += '\\';
                    else if (n == 'n')  out += '\n';
                    else if (n == 'r')  out += '\r';
                    else if (n == 't')  out += '\t';
                    else                out += n;
                    pos += 2;
                } else out += s[pos++];
            }
            if (pos < s.size()) ++pos;
            return true;
        }
        bool parseDouble(double& out) {
            skipWS();
            size_t start = pos;
            if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
            while (pos < s.size() &&
                   ((s[pos] >= '0' && s[pos] <= '9') ||
                    s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' ||
                    s[pos] == '-' || s[pos] == '+'))
                ++pos;
            if (pos == start) return false;
            try { out = std::stod(s.substr(start, pos - start)); }
            catch (...) { return false; }
            return true;
        }
        bool parseInt(long long& out) {
            double d; if (!parseDouble(d)) return false;
            out = (long long)d;
            return true;
        }
        bool parseU64FromAny(uint64_t& out) {
            // Accept either a quoted-string number or a bare number.
            skipWS();
            if (pos < s.size() && s[pos] == '"') {
                std::string tmp; if (!parseString(tmp)) return false;
                try { out = std::stoull(tmp); } catch (...) { return false; }
                return true;
            }
            double d; if (!parseDouble(d)) return false;
            out = (uint64_t)d;
            return true;
        }
        bool parseHexU64(uint64_t& out) {
            std::string tmp; if (!parseString(tmp)) return false;
            try { out = std::stoull(tmp, nullptr, 0); }
            catch (...) { out = 0; }
            return true;
        }
        bool parseUIntArr(std::vector<uint32_t>& v) {
            skipWS();
            if (!match('[')) return false;
            v.clear();
            skipWS();
            if (match(']')) return true;
            while (true) {
                long long n; if (!parseInt(n)) return false;
                v.push_back((uint32_t)n);
                skipWS();
                if (match(',')) continue;
                if (match(']')) return true;
                return false;
            }
        }
        bool skipValue() {
            skipWS();
            if (pos >= s.size()) return false;
            char c = s[pos];
            if (c == '"') { std::string tmp; return parseString(tmp); }
            if (c == '{') { return skipObject(); }
            if (c == '[') {
                ++pos;
                while (true) {
                    skipWS();
                    if (match(']')) return true;
                    if (!skipValue()) return false;
                    if (match(',')) continue;
                    if (match(']')) return true;
                    return false;
                }
            }
            // number / true / false / null — read until comma or close.
            while (pos < s.size() &&
                   s[pos] != ',' && s[pos] != '}' && s[pos] != ']')
                ++pos;
            return true;
        }
        bool skipObject() {
            if (!match('{')) return false;
            while (true) {
                skipWS();
                if (match('}')) return true;
                std::string k; if (!parseString(k)) return false;
                if (!match(':')) return false;
                if (!skipValue()) return false;
                if (match(',')) continue;
                if (match('}')) return true;
                return false;
            }
        }
        bool parseDeck(ReplayDeck& d) {
            if (!match('{')) return false;
            while (true) {
                skipWS();
                if (match('}')) return true;
                std::string k; if (!parseString(k)) return false;
                if (!match(':')) return false;
                if      (k == "name") parseString(d.name);
                else if (k == "path") parseString(d.path);
                else if (k == "main") parseUIntArr(d.main);
                else if (k == "extra")parseUIntArr(d.extra);
                else if (k == "side") parseUIntArr(d.side);
                else                  skipValue();
                if (match(',')) continue;
                if (match('}')) return true;
                return false;
            }
        }
        bool parseResponses(std::vector<ReplayResponse>& out) {
            if (!match('[')) return false;
            skipWS();
            if (match(']')) return true;
            while (true) {
                if (!match('{')) return false;
                ReplayResponse r;
                while (true) {
                    std::string k; if (!parseString(k)) return false;
                    if (!match(':')) return false;
                    if      (k == "t")   parseDouble(r.t);
                    else if (k == "hex") {
                        std::string hex; parseString(hex);
                        r.data = fromHex(hex);
                    }
                    else skipValue();
                    if (match(',')) continue;
                    if (match('}')) break;
                    return false;
                }
                out.push_back(std::move(r));
                if (match(',')) continue;
                if (match(']')) return true;
                return false;
            }
        }
        bool parseEvents(std::vector<ReplayEvent>& out) {
            if (!match('[')) return false;
            skipWS();
            if (match(']')) return true;
            while (true) {
                if (!match('{')) return false;
                ReplayEvent e;
                while (true) {
                    std::string k; if (!parseString(k)) return false;
                    if (!match(':')) return false;
                    if      (k == "t")    parseDouble(e.t);
                    else if (k == "text") parseString(e.text);
                    else skipValue();
                    if (match(',')) continue;
                    if (match('}')) break;
                    return false;
                }
                out.push_back(std::move(e));
                if (match(',')) continue;
                if (match(']')) return true;
                return false;
            }
        }
        bool parseObject(Replay& r) {
            skipWS();
            if (!match('{')) return false;
            while (true) {
                skipWS();
                if (match('}')) return true;
                std::string k; if (!parseString(k)) return false;
                if (!match(':')) return false;
                if      (k == "version")      { long long v; parseInt(v); r.version = (int)v; }
                else if (k == "app")          parseString(r.app);
                else if (k == "timestamp")    parseString(r.timestamp);
                else if (k == "seed")         parseU64FromAny(r.seed);
                else if (k == "rule_flags")   parseHexU64(r.ruleFlags);
                else if (k == "lp")           { long long v; parseInt(v); r.lp = (uint32_t)v; }
                else if (k == "hand_count")   { long long v; parseInt(v); r.handCount = (uint32_t)v; }
                else if (k == "draw_count")   { long long v; parseInt(v); r.drawCount = (uint32_t)v; }
                else if (k == "deck1")        parseDeck(r.deck1);
                else if (k == "deck2")        parseDeck(r.deck2);
                else if (k == "winner")       { long long v; parseInt(v); r.winner = (int)v; }
                else if (k == "turns")        { long long v; parseInt(v); r.turns = (int)v; }
                else if (k == "duration_sec") parseDouble(r.durationSec);
                else if (k == "final_lp") {
                    std::vector<uint32_t> arr; parseUIntArr(arr);
                    if (arr.size() >= 1) r.finalLP[0] = arr[0];
                    if (arr.size() >= 2) r.finalLP[1] = arr[1];
                }
                else if (k == "card_db")      parseString(r.cardDb);
                else if (k == "script_count") { long long v; parseInt(v); r.scriptCount = (int)v; }
                else if (k == "responses")    parseResponses(r.responses);
                else if (k == "events")       parseEvents(r.events);
                else                          skipValue();
                if (match(',')) continue;
                if (match('}')) return true;
                return false;
            }
        }
    };
};

} // namespace edo
