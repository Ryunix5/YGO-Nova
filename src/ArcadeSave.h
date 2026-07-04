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
//   * The CYCLE: open 10 Master Packs + 10 Secret Packs → duel → BOTH
//     players spin the wheel → winner +5 leaderboard points → repeat
//     (packs restock to 10/10, keys reset).
//   * A key = a 16-bit archetype setcode, granted by pulling an SR/UR that
//     belongs to that archetype (Master Duel's unlock rule). Keys reset
//     each cycle; last cycle's keys become the CRAFT pool.
//   * The wheel pays out craft credits (crSR / crNR) or bonus secret
//     packs. Credits buy exact cards from last cycle's unlocked packs.
//   * The leaderboard (member → points) lives in every member's copy of
//     the save and merges via ArcadeSync whenever two members connect —
//     points only grow, so per-name MAX is a safe merge.
//
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
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
    int spins = 0;                          // unspent wheel spins (1/duel, ALL)
    int crSR = 0;                           // SR craft credits (wheel reward)
    int crNR = 0;                           // N/R craft credits (wheel reward)
    std::set<uint16_t> keys;                // unlocked secret packs (setcodes)
    std::set<uint16_t> craftKeys;           // LAST cycle's packs = craft pool
    std::map<std::string, int> board;       // leaderboard: member -> points
    std::unordered_map<uint32_t, int> pool; // owned cards: code -> copies

    // Merge another member's leaderboard: union of names, per-name MAX
    // points (points only ever increase, so max never loses progress).
    void mergeBoard(const std::map<std::string, int>& other) {
        for (auto& kv : other) {
            int& p = board[kv.first];
            if (kv.second > p) p = kv.second;
        }
    }

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
                else if (k == "spins")      spins      = std::stoi(v);
                else if (k == "crSR")       crSR       = std::stoi(v);
                else if (k == "crNR")       crNR       = std::stoi(v);
                else if (k == "key")        keys.insert((uint16_t)std::stoul(v));
                else if (k == "ckey")  craftKeys.insert((uint16_t)std::stoul(v));
                else if (k == "member") {   // "points name" (name has spaces)
                    std::istringstream ss(v);
                    int pts = 0; ss >> pts;
                    std::string nm2;
                    std::getline(ss, nm2);
                    while (!nm2.empty() && nm2.front() == ' ')
                        nm2.erase(nm2.begin());
                    if (!nm2.empty()) board[nm2] = pts;
                }
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
        f << "spins="      << spins      << "\n";
        f << "crSR="       << crSR       << "\n";
        f << "crNR="       << crNR       << "\n";
        for (uint16_t k : keys)      f << "key="  << k << "\n";
        for (uint16_t k : craftKeys) f << "ckey=" << k << "\n";
        for (auto& kv : board)   f << "member=" << kv.second << " "
                                   << kv.first << "\n";
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
