#pragma once
// ─── ArcadeSave.h ────────────────────────────────────────────────────────────
//
// Arcade (Master Saga) save files. One save per friend group, Lethal-Company
// style: it holds YOUR collection for that campaign — packs remaining, the
// secret-pack keys you've unlocked, every card you own, and your record.
// Plain key=value text (like Settings) so a save survives app updates and can
// be hand-inspected. Lives in assets/arcade/<name>.arcade; each player keeps
// their own save on their own machine (honor system, like real progression
// series between friends).
//
// Rules baked here (per the mode's design):
//   * 10 Master Packs, then up to 10 Secret Packs total, then the pool is
//     FIXED — no growth, no crafting (v1).
//   * A key = a 16-bit archetype setcode, granted by pulling an SR/UR that
//     belongs to that archetype (Master Duel's unlock rule).
//
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace edo {

struct ArcadeSave {
    std::string name;                       // save / friend-group name
    int masterLeft = 10;                    // master packs still unopened
    int secretLeft = 10;                    // secret packs still unopened
    int wins = 0, losses = 0;               // campaign record
    std::set<uint16_t> keys;                // unlocked secret packs (setcodes)
    std::unordered_map<uint32_t, int> pool; // owned cards: code -> copies

    int poolTotal() const {
        int n = 0;
        for (auto& kv : pool) n += kv.second;
        return n;
    }
    int owned(uint32_t code) const {
        auto it = pool.find(code);
        return it == pool.end() ? 0 : it->second;
    }

    static std::string dir() { return "assets/arcade"; }
    static std::string pathFor(const std::string& n) {
        return dir() + "/" + n + ".arcade";
    }

    // All existing save names (filenames without extension), sorted.
    static std::vector<std::string> list() {
        std::vector<std::string> out;
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(dir(), ec)) return out;
        for (auto& e : fs::directory_iterator(dir(), ec))
            if (e.path().extension() == ".arcade")
                out.push_back(e.path().stem().string());
        std::sort(out.begin(), out.end());
        return out;
    }

    bool load(const std::string& saveName) {
        std::ifstream f(pathFor(saveName));
        if (!f) return false;
        *this = ArcadeSave{};
        name = saveName;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            try {
                if      (k == "masterLeft") masterLeft = std::stoi(v);
                else if (k == "secretLeft") secretLeft = std::stoi(v);
                else if (k == "wins")       wins       = std::stoi(v);
                else if (k == "losses")     losses     = std::stoi(v);
                else if (k == "key")        keys.insert((uint16_t)std::stoul(v));
                else if (k == "card") {     // "code xN"
                    std::istringstream ss(v);
                    uint32_t code; int cnt = 1;
                    ss >> code >> cnt;
                    if (code) pool[code] = cnt;
                }
            } catch (...) {}
        }
        return true;
    }

    bool save() const {
        std::error_code ec;
        std::filesystem::create_directories(dir(), ec);
        std::ofstream f(pathFor(name), std::ios::trunc);
        if (!f) return false;
        f << "# YGO Nova Arcade save — Master Saga campaign '" << name << "'\n";
        f << "masterLeft=" << masterLeft << "\n";
        f << "secretLeft=" << secretLeft << "\n";
        f << "wins="       << wins       << "\n";
        f << "losses="     << losses     << "\n";
        for (uint16_t k : keys)  f << "key=" << k << "\n";
        for (auto& kv : pool)    f << "card=" << kv.first << " "
                                   << kv.second << "\n";
        return f.good();
    }

    static bool remove(const std::string& saveName) {
        std::error_code ec;
        return std::filesystem::remove(pathFor(saveName), ec);
    }
};

} // namespace edo
