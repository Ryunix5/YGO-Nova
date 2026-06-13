#pragma once
// ─── Settings.h ────────────────────────────────────────────────────────────
//
// Persistent user-preference store. Header-only so adding it doesn't require
// CMake changes. Format is one key=value pair per line, lightweight and
// human-readable. Unknown keys are ignored (forward compatibility); a
// missing or unreadable file falls back to defaults without crashing.
//
// Lifecycle:
//   * Game::init()        → Settings::load(defaultPath())
//   * settings change     → Settings::save(defaultPath())   (auto)
//   * Game::shutdown()    → Settings::save(defaultPath())   (defensive)
//
// The struct is plain data — no behaviour beyond load/save. Callers are
// expected to apply the values to their respective subsystems (AudioManager,
// UI toggles, etc.) themselves, so the settings file stays decoupled from
// the rest of the codebase.
//
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <system_error>
#include <cstdio>

namespace edo {

struct Settings {
    // ── Audio ────────────────────────────────────────────────────────────
    bool   sfxMuted          = false;
    float  sfxVolume         = 0.70f;

    // ── Visual toggles ───────────────────────────────────────────────────
    bool   showFieldNames    = false;
    bool   largePreview      = false;
    bool   showZoneLabels    = true;
    bool   showLegalGlow     = true;
    bool   animationsEnabled = true;

    // ── Animation presentation (Stage A) ─────────────────────────────────
    bool   animBigSummons    = true;     // boss/large-monster centre entrance
    bool   animPhaseBanners  = true;     // centre phase banner on phase change
    bool   animScreenShake   = true;     // brief board jitter on big events
    bool   animReduceMotion  = false;    // accessibility: short fades, no shake
    float  animSpeed         = 1.0f;     // 0.5 / 1 / 2 / 0(=instant)
    float  animPhaseDelay    = 0.45f;    // cosmetic hold between phases (s)

    // ── Logs ─────────────────────────────────────────────────────────────
    bool   logCollapsed      = true;     // collapsed by default — quiet UI
    int    selectedLogTab    = 0;        // 0 = Game, 1 = Debug
    bool   debugLog          = false;
    bool   verboseLog        = false;    // engine-level technical traces

    // ── Gameplay UI ──────────────────────────────────────────────────────
    bool   confirmRestart    = false;
    bool   clickFirstHints   = true;

    // ── Replays ──────────────────────────────────────────────────────────
    // Auto-save a JSON replay to assets/replays/ when the duel ends. On by
    // default — recording is cheap and the user can clean the folder later.
    bool   autoSaveReplays   = true;

    // ── Multiplayer (foundation — LAN sockets are not implemented yet) ──
    // Persisted so Host/Join fields remember their last value between
    // launches. mpMode is a string ("offline"/"host"/"client"/"lan") so
    // future modes can extend it without a settings migration.
    std::string mpDisplayName = "Player";
    std::string mpHostIP      = "127.0.0.1";
    int         mpPort        = 7878;
    std::string mpMode        = "offline";

    // ── Last-used decks (deck builder + lobby setup) ─────────────────────
    std::string lastDeckP1;
    std::string lastDeckP2;
    std::string lastDeckEditor;

    // Last warning produced by save/load — populated by load() when the
    // file existed but had a parsing problem.
    std::string lastLoadWarning;

    // Canonical settings path. Lives under assets/config/ so it ships with
    // the rest of the runtime data; the directory is created on save.
    static std::string defaultPath() {
        return "assets/config/settings.cfg";
    }

    // ── Load / Save ──────────────────────────────────────────────────────
    // Returns false if the file did not exist OR could not be read. A file
    // that exists but contains a malformed line is treated as best-effort:
    // valid keys apply, malformed lines are skipped and lastLoadWarning is
    // populated. Returning false from a corrupt-but-existing case would
    // wipe valid settings on next save, which we explicitly avoid.
    bool load(const std::string& path = defaultPath()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return false;
        std::ifstream f(path);
        if (!f) {
            lastLoadWarning = "could not open settings file";
            return false;
        }
        std::string line;
        int lineNo = 0;
        int badLines = 0;
        while (std::getline(f, line)) {
            ++lineNo;
            // Strip CR (Windows line endings) and trim ends.
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == ' ' ||
                    line.back() == '\t'))
                line.pop_back();
            size_t i = 0;
            while (i < line.size() &&
                   (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i) line.erase(0, i);
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) { ++badLines; continue; }
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            applyKV(k, v);
        }
        if (badLines > 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                "%d malformed line%s in settings — defaults used for those",
                badLines, badLines == 1 ? "" : "s");
            lastLoadWarning = buf;
        }
        return true;
    }

    bool save(const std::string& path = defaultPath()) const {
        std::error_code ec;
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::ofstream f(path);
        if (!f) return false;
        f << "# EdoPro+ settings — auto-generated. One key=value per line.\n";
        f << "# Lines starting with # are comments.\n";
        f << "\n# Audio\n";
        f << "sfxMuted="          << (sfxMuted          ? "1" : "0") << "\n";
        f << "sfxVolume="         << sfxVolume                       << "\n";
        f << "\n# Visual\n";
        f << "showFieldNames="    << (showFieldNames    ? "1" : "0") << "\n";
        f << "largePreview="      << (largePreview      ? "1" : "0") << "\n";
        f << "showZoneLabels="    << (showZoneLabels    ? "1" : "0") << "\n";
        f << "showLegalGlow="     << (showLegalGlow     ? "1" : "0") << "\n";
        f << "animationsEnabled=" << (animationsEnabled ? "1" : "0") << "\n";
        f << "animBigSummons="    << (animBigSummons    ? "1" : "0") << "\n";
        f << "animPhaseBanners="  << (animPhaseBanners  ? "1" : "0") << "\n";
        f << "animScreenShake="   << (animScreenShake   ? "1" : "0") << "\n";
        f << "animReduceMotion="  << (animReduceMotion  ? "1" : "0") << "\n";
        f << "animSpeed="         << animSpeed                       << "\n";
        f << "animPhaseDelay="    << animPhaseDelay                  << "\n";
        f << "\n# Logs\n";
        f << "logCollapsed="      << (logCollapsed      ? "1" : "0") << "\n";
        f << "selectedLogTab="    << selectedLogTab                   << "\n";
        f << "debugLog="          << (debugLog          ? "1" : "0") << "\n";
        f << "verboseLog="        << (verboseLog        ? "1" : "0") << "\n";
        f << "\n# Gameplay UI\n";
        f << "confirmRestart="    << (confirmRestart    ? "1" : "0") << "\n";
        f << "clickFirstHints="   << (clickFirstHints   ? "1" : "0") << "\n";
        f << "\n# Replays\n";
        f << "autoSaveReplays="   << (autoSaveReplays   ? "1" : "0") << "\n";
        f << "\n# Multiplayer\n";
        f << "mpDisplayName="     << mpDisplayName << "\n";
        f << "mpHostIP="          << mpHostIP      << "\n";
        f << "mpPort="            << mpPort        << "\n";
        f << "mpMode="            << mpMode        << "\n";
        f << "\n# Last-used decks\n";
        f << "lastDeckP1="        << lastDeckP1     << "\n";
        f << "lastDeckP2="        << lastDeckP2     << "\n";
        f << "lastDeckEditor="    << lastDeckEditor << "\n";
        return f.good();
    }

private:
    static bool boolFromStr(const std::string& v) {
        if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
        return false;
    }
    static float floatFromStr(const std::string& v, float fallback) {
        try { return std::stof(v); } catch (...) { return fallback; }
    }
    static int   intFromStr(const std::string& v, int fallback) {
        try { return std::stoi(v); } catch (...) { return fallback; }
    }

    void applyKV(const std::string& k, const std::string& v) {
        if      (k == "sfxMuted")          sfxMuted          = boolFromStr(v);
        else if (k == "sfxVolume")         sfxVolume         = floatFromStr(v, sfxVolume);
        else if (k == "showFieldNames")    showFieldNames    = boolFromStr(v);
        else if (k == "largePreview")      largePreview      = boolFromStr(v);
        else if (k == "showZoneLabels")    showZoneLabels    = boolFromStr(v);
        else if (k == "showLegalGlow")     showLegalGlow     = boolFromStr(v);
        else if (k == "animationsEnabled") animationsEnabled = boolFromStr(v);
        else if (k == "animBigSummons")    animBigSummons    = boolFromStr(v);
        else if (k == "animPhaseBanners")  animPhaseBanners  = boolFromStr(v);
        else if (k == "animScreenShake")   animScreenShake   = boolFromStr(v);
        else if (k == "animReduceMotion")  animReduceMotion  = boolFromStr(v);
        else if (k == "animSpeed")         animSpeed         = floatFromStr(v, animSpeed);
        else if (k == "animPhaseDelay")    animPhaseDelay    = floatFromStr(v, animPhaseDelay);
        else if (k == "logCollapsed")      logCollapsed      = boolFromStr(v);
        else if (k == "selectedLogTab")    selectedLogTab    = intFromStr(v, selectedLogTab);
        else if (k == "debugLog")          debugLog          = boolFromStr(v);
        else if (k == "verboseLog")        verboseLog        = boolFromStr(v);
        else if (k == "confirmRestart")    confirmRestart    = boolFromStr(v);
        else if (k == "clickFirstHints")   clickFirstHints   = boolFromStr(v);
        else if (k == "autoSaveReplays")   autoSaveReplays   = boolFromStr(v);
        else if (k == "mpDisplayName")     mpDisplayName     = v;
        else if (k == "mpHostIP")          mpHostIP          = v;
        else if (k == "mpPort")            mpPort            = intFromStr(v, mpPort);
        else if (k == "mpMode")            mpMode            = v;
        else if (k == "lastDeckP1")        lastDeckP1        = v;
        else if (k == "lastDeckP2")        lastDeckP2        = v;
        else if (k == "lastDeckEditor")    lastDeckEditor    = v;
        // unknown key → ignore (forward compatibility)
    }
};

} // namespace edo
