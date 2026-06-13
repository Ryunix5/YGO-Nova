#include "CardDB.h"
#include <sqlite3.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;

// Standard column projection used by every query. d.setcode MUST be selected
// — ocgcore's archetype system (Card.IsSetCard) is fed entirely from it.
// d.ot carries the legality/status (1 OCG, 2 TCG, 3 both, higher = custom).
const char* kCols =
    "d.id,d.alias,d.type,d.atk,d.def,d.level,d.race,d.attribute,t.name,t.desc,"
    "d.setcode,d.ot";

// Fill a CardInfo from a result row of the kCols SELECT.
void readRow(sqlite3_stmt* st, CardInfo& info) {
    info.id        = (uint32_t)sqlite3_column_int(st, 0);
    info.alias     = (uint32_t)sqlite3_column_int(st, 1);
    info.type      = (uint32_t)sqlite3_column_int(st, 2);
    info.atk       = sqlite3_column_int(st, 3);
    info.def       = sqlite3_column_int(st, 4);
    uint32_t rawLv = (uint32_t)sqlite3_column_int(st, 5);
    info.level     = rawLv & 0xFF;          // bits 0-7 = level/rank
    info.lscale    = (rawLv >> 24) & 0xFF;  // bits 24-31 = left scale
    info.rscale    = (rawLv >> 16) & 0xFF;  // bits 16-23 = right scale
    info.race      = (uint64_t)sqlite3_column_int64(st, 6);
    info.attribute = (uint32_t)sqlite3_column_int(st, 7);
    auto name = (const char*)sqlite3_column_text(st, 8);
    auto desc = (const char*)sqlite3_column_text(st, 9);
    if (name) info.name = name;
    if (desc) info.desc = desc;
    info.setcode   = (uint64_t)sqlite3_column_int64(st, 10);
    info.ot        = (uint32_t)sqlite3_column_int(st, 11);
}
} // namespace

CardDB::~CardDB() { close(); }

void CardDB::close() {
    for (auto& db : m_dbs)
        if (db.handle) sqlite3_close((sqlite3*)db.handle);
    m_dbs.clear();
}

bool CardDB::open(const std::string& path) {
    close();
    return addDatabase(path);
}

bool CardDB::addDatabase(const std::string& path) {
    sqlite3* h = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &h, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK || !h) {
        if (h) sqlite3_close(h);
        printf("[CardDB] FAILED to open: %s\n", path.c_str());
        return false;
    }
    std::error_code ec;
    std::string abs = fs::absolute(path, ec).lexically_normal().string();
    if (ec) abs = path;

    auto count = [&](const char* tbl) -> long {
        long n = -1;
        std::string q = std::string("SELECT COUNT(*) FROM ") + tbl;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h, q.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        return n;
    };
    long datas = count("datas"), texts = count("texts");
    uintmax_t sz = fs::file_size(abs, ec); if (ec) sz = 0;
    printf("[CardDB] %s: %s  (size=%llu, datas=%ld, texts=%ld)\n",
           m_dbs.empty() ? "PRIMARY " : "fallback",
           abs.c_str(), (unsigned long long)sz, datas, texts);
    if (datas < 0 || texts < 0) {
        printf("[CardDB]   WARNING: unexpected schema (no datas/texts) — skipped\n");
        sqlite3_close(h);
        return false;
    }
    m_dbs.push_back({ (void*)h, abs });
    return true;
}

bool CardDB::openAuto() {
    close();
    // Probe the working directory and a few parents — the app runs from
    // build/windows/Release so the project root is several levels up.
    const std::vector<std::string> bases = {
        ".", "..", "../..", "../../..", "../../../..", "../../../../.."
    };
    std::vector<std::string> found;
    auto consider = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return;
        std::string a = fs::absolute(p, ec).lexically_normal().string();
        if (ec) return;
        for (auto& f : found) if (f == a) return;   // de-dupe
        found.push_back(a);
    };
    // (1) runtime + project-root assets/cards.cdb (these become the primary).
    for (auto& b : bases) consider(fs::path(b) / "assets" / "cards.cdb");
    // (2) every .cdb inside a BabelCDB-master folder (newer / extra cards).
    for (auto& b : bases) {
        fs::path dir = fs::path(b) / "BabelCDB-master";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto& ent : fs::directory_iterator(dir, ec)) {
            std::string e = ent.path().extension().string();
            for (char& c : e) c = (char)tolower((unsigned char)c);
            if (e == ".cdb") consider(ent.path());
        }
    }
    printf("[CardDB] scan: %u card database file(s) detected\n",
           (unsigned)found.size());
    for (auto& f : found) addDatabase(f);
    if (m_dbs.empty())
        printf("[CardDB] ERROR: no usable card database found.\n");
    else
        printf("[CardDB] primary=%s  (+%d fallback database(s))\n",
               m_dbs[0].path.c_str(), (int)m_dbs.size() - 1);
    // System/hint strings ride along with the card databases — they feed
    // DuelManager::decodeDesc for non-card-bound description values.
    loadStringsAuto();
    return isOpen();
}

// Parse an EDOPro-style strings.conf. We only consume "!system <id> <text>"
// lines; !victory/!counter/!setname entries are ignored. Additive: calling
// twice merges (later files win on duplicate ids).
bool CardDB::loadStrings(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    size_t before = m_sysStrings.size();
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() < 9 || line.compare(0, 8, "!system ") != 0) continue;
        size_t idStart = 8;
        size_t sp = line.find(' ', idStart);
        std::string idStr = (sp == std::string::npos)
            ? line.substr(idStart) : line.substr(idStart, sp - idStart);
        uint32_t id = 0;
        try { id = (uint32_t)std::stoul(idStr); } catch (...) { continue; }
        std::string text = (sp == std::string::npos)
            ? std::string() : line.substr(sp + 1);
        // Trim trailing CR / whitespace (file may be CRLF).
        while (!text.empty() &&
               (text.back() == '\r' || text.back() == ' ' ||
                text.back() == '\t'))
            text.pop_back();
        m_sysStrings[id] = std::move(text);
    }
    printf("[CardDB] strings: %s  (+%u system string(s), total %u)\n",
           path.c_str(),
           (unsigned)(m_sysStrings.size() - before),
           (unsigned)m_sysStrings.size());
    return true;
}

void CardDB::loadStringsAuto() {
    const std::vector<std::string> bases = {
        ".", "..", "../..", "../../..", "../../../..", "../../../../.."
    };
    for (auto& b : bases) {
        fs::path p = fs::path(b) / "assets" / "config" / "strings.conf";
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        if (loadStrings(p.string())) return;
    }
    printf("[CardDB] strings: assets/config/strings.conf not found — "
           "system/hint descriptions will fall back to generic labels\n");
}

const std::string& CardDB::dbPath() const {
    static const std::string empty;
    return m_dbs.empty() ? empty : m_dbs[0].path;
}

CardInfo CardDB::readOne(void* handle, uint32_t code) const {
    CardInfo info;
    if (!handle) return info;
    // Prefer an exact id match over an alias (alternate-art) match.
    std::string sql = std::string("SELECT ") + kCols +
        " FROM datas d JOIN texts t ON d.id=t.id "
        "WHERE d.id=? OR d.alias=? ORDER BY (d.id=?) DESC LIMIT 1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return info;
    sqlite3_bind_int(st, 1, (int)code);
    sqlite3_bind_int(st, 2, (int)code);
    sqlite3_bind_int(st, 3, (int)code);
    if (sqlite3_step(st) == SQLITE_ROW) readRow(st, info);
    sqlite3_finalize(st);
    return info;
}

CardInfo CardDB::getCard(uint32_t code) const {
    for (const auto& db : m_dbs) {
        CardInfo ci = readOne(db.handle, code);
        if (ci.id != 0) { ci.source = db.path; return ci; }
    }
    return CardInfo{};
}

// Fetch texts.str{index+1} for a card. ocgcore effect descriptions encode a
// card-specific string as aux.Stringid(code,idx) == code*16+idx; idx then
// maps onto the texts table's str1..str16 columns (idx 0 -> str1). This lets
// the duel log / UI print the actual effect text for an offered effect rather
// than only the card name — e.g. "Search 1 non-Warrior 'Vanquish Soul'
// monster" vs "Activate 1 effect according to Attribute(s)".
std::string CardDB::cardString(uint32_t code, int index) const {
    if (index < 0 || index > 15) return std::string();
    const std::string sql =
        "SELECT str" + std::to_string(index + 1) +
        " FROM texts WHERE id=? LIMIT 1";
    for (const auto& db : m_dbs) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2((sqlite3*)db.handle, sql.c_str(), -1,
                               &st, nullptr) != SQLITE_OK)
            continue;
        sqlite3_bind_int(st, 1, (int)code);
        std::string out;
        if (sqlite3_step(st) == SQLITE_ROW) {
            auto s = (const char*)sqlite3_column_text(st, 0);
            if (s) out = s;
        }
        sqlite3_finalize(st);
        if (!out.empty()) return out;
    }
    return std::string();
}

std::vector<CardInfo> CardDB::search(const std::string& query, int limit) const {
    std::vector<CardInfo> results;
    if (m_dbs.empty()) return results;

    // Search terms: the query plus a local "Kewl Tune" <-> "Killer Tune" alias
    // (search-only — the databases are never modified).
    std::vector<std::string> terms{ query };
    {
        std::string lo = query;
        for (char& c : lo) c = (char)tolower((unsigned char)c);
        size_t pos;
        if ((pos = lo.find("kewl tune")) != std::string::npos) {
            std::string a = query; a.replace(pos, 9, "Killer Tune");
            terms.push_back(a);
        }
        if ((pos = lo.find("killer tune")) != std::string::npos) {
            std::string a = query; a.replace(pos, 11, "Kewl Tune");
            terms.push_back(a);
        }
    }
    const std::string sql = std::string("SELECT ") + kCols +
        " FROM datas d JOIN texts t ON d.id=t.id "
        "WHERE t.name LIKE ?1 OR t.desc LIKE ?1 OR CAST(d.id AS TEXT) LIKE ?1 "
        "LIMIT ?2";

    std::vector<uint32_t> seen;
    for (const auto& db : m_dbs) {
        for (const std::string& term : terms) {
            if ((int)results.size() >= limit) return results;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2((sqlite3*)db.handle, sql.c_str(), -1,
                                   &st, nullptr) != SQLITE_OK)
                continue;
            std::string pat = "%" + term + "%";
            sqlite3_bind_text(st, 1, pat.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (st, 2, limit);
            while (sqlite3_step(st) == SQLITE_ROW) {
                uint32_t id = (uint32_t)sqlite3_column_int(st, 0);
                bool dup = false;
                for (uint32_t s : seen) if (s == id) { dup = true; break; }
                if (dup) continue;
                seen.push_back(id);
                CardInfo info;
                readRow(st, info);
                info.source = db.path;
                results.push_back(std::move(info));
            }
            sqlite3_finalize(st);
        }
    }
    return results;
}

std::vector<CardInfo> CardDB::filter(uint32_t typeMask, uint32_t attrMask,
                                      int level, int limit) const {
    std::vector<CardInfo> results;
    const std::string sql = std::string("SELECT ") + kCols +
        " FROM datas d JOIN texts t ON d.id=t.id "
        "WHERE (? = 0 OR (d.type & ?) != 0) "
        "  AND (? = 0 OR (d.attribute & ?) != 0) "
        "  AND (? < 0 OR (d.level & 255) = ?) "
        "LIMIT ?";

    std::vector<uint32_t> seen;
    for (const auto& db : m_dbs) {
        if ((int)results.size() >= limit) break;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2((sqlite3*)db.handle, sql.c_str(), -1,
                               &st, nullptr) != SQLITE_OK)
            continue;
        sqlite3_bind_int(st, 1, (int)typeMask);
        sqlite3_bind_int(st, 2, (int)typeMask);
        sqlite3_bind_int(st, 3, (int)attrMask);
        sqlite3_bind_int(st, 4, (int)attrMask);
        sqlite3_bind_int(st, 5, level);
        sqlite3_bind_int(st, 6, level);
        sqlite3_bind_int(st, 7, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            uint32_t id = (uint32_t)sqlite3_column_int(st, 0);
            bool dup = false;
            for (uint32_t s : seen) if (s == id) { dup = true; break; }
            if (dup) continue;
            seen.push_back(id);
            CardInfo info;
            readRow(st, info);
            info.source = db.path;
            results.push_back(std::move(info));
        }
        sqlite3_finalize(st);
    }
    return results;
}

// ─── ocgcore callbacks ────────────────────────────────────────────────────────

void CardDB::cardReaderCb(void* payload, uint32_t code, OCG_CardData* out) {
    auto* db = static_cast<CardDB*>(payload);
    CardInfo info = db->getCard(code);   // searches primary + all fallbacks

    out->code       = info.id ? info.id : code;
    out->alias      = info.alias;
    out->type       = info.type;
    out->level      = info.level;
    out->attribute  = info.attribute;
    out->race       = info.race;
    out->attack     = info.atk;
    out->defense    = info.def;
    out->lscale     = info.lscale;
    out->rscale     = info.rscale;
    out->link_marker= 0;

    // setcodes: ocgcore expects a 0-terminated uint16_t array, and EVERY
    // Card.IsSetCard() / archetype check reads it. cards.cdb packs up to four
    // 16-bit set codes into the 64-bit datas.setcode column. The previous
    // code hard-coded an empty array ({0}), so every card in the game
    // belonged to no archetype — which is exactly why Vanquish Soul Razen's
    // on-summon search trigger (filter: c:IsSetCard(SET_VANQUISH_SOUL)) found
    // 0 legal targets and the engine offered 0 options in the post-summon
    // chain window. Razen's Quick Effect still worked because its filter
    // checks IsAttribute()/IsPublic(), not set code.
    uint16_t parts[4];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        uint16_t s = (uint16_t)((info.setcode >> (i * 16)) & 0xFFFF);
        if (s) parts[n++] = s;
    }
    out->setcodes = new uint16_t[n + 1];
    for (int i = 0; i < n; ++i) out->setcodes[i] = parts[i];
    out->setcodes[n] = 0;                       // 0 terminator
}

void CardDB::cardReaderDoneCb(void* /*payload*/, OCG_CardData* data) {
    delete[] data->setcodes;
    data->setcodes = nullptr;
}
