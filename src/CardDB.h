#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "ocgapi_types.h"

struct CardInfo {
    uint32_t    id        = 0;
    std::string name;
    std::string desc;
    uint32_t    type      = 0;
    int32_t     atk       = 0;
    int32_t     def       = 0;
    uint32_t    level     = 0;
    uint32_t    lscale    = 0;  // pendulum left scale
    uint32_t    rscale    = 0;  // pendulum right scale
    uint64_t    race      = 0;
    uint32_t    attribute = 0;
    uint32_t    alias     = 0;
    uint32_t    ot        = 0;
    uint64_t    setcode   = 0;  // packed archetype set codes: 4 x 16-bit.
                                // Feeds ocgcore's OCG_CardData.setcodes, which
                                // every Card.IsSetCard() / archetype check
                                // depends on. 0 means "belongs to no series".
    std::string source;        // which .cdb file the card was found in (debug)
};

// CardDB can hold several SQLite card databases at once: a primary plus any
// number of fallbacks (e.g. the BabelCDB-master collection). Every lookup
// searches the primary first, then the fallbacks — so cards missing from a
// stale runtime cards.cdb are still resolved from the newer databases.
class CardDB {
public:
    CardDB() = default;
    ~CardDB();

    // Open a single database as the primary (replaces any open databases).
    bool open(const std::string& path);
    // Scan standard locations (runtime assets, project-root assets and any
    // BabelCDB-master folder) and open every .cdb found. First opened = primary.
    bool openAuto();
    // Open one more database and append it as a fallback.
    bool addDatabase(const std::string& path);
    void close();

    bool isOpen()        const { return !m_dbs.empty(); }
    int  databaseCount() const { return (int)m_dbs.size(); }
    const std::string& dbPath() const;   // primary database path (or empty)

    // Look up a card by code. Searches the primary then every fallback;
    // returns the first hit (CardInfo::source names the database).
    CardInfo getCard(uint32_t code) const;
    // Bulk id -> setcode map across every database (primary wins on clashes).
    // One table scan per database — vastly faster than per-code getCard calls
    // when a caller needs thousands of setcodes (e.g. Arcade's pull tables).
    std::unordered_map<uint32_t, uint64_t> allSetcodes() const;
    // Fetch one indexed effect string for a card. The index matches Lua's
    // aux.Stringid(code,idx): idx 0 -> texts.str1, idx 1 -> texts.str2, etc.
    // Searches the primary then every fallback; empty string if not found.
    std::string cardString(uint32_t code, int index) const;
    // EDOPro-style system/hint strings (strings.conf "!system <id> <text>").
    // Used by DuelManager::decodeDesc for engine description values that are
    // NOT card-bound. loadStrings is additive and tolerant: a missing file is
    // not an error (systemString just returns empty strings then).
    bool loadStrings(const std::string& path);
    // Try the standard location (assets/config/strings.conf). Called from
    // openAuto(); safe to call repeatedly.
    void loadStringsAuto();
    std::string systemString(uint32_t id) const {
        auto it = m_sysStrings.find(id);
        return it == m_sysStrings.end() ? std::string() : it->second;
    }
    size_t systemStringCount() const { return m_sysStrings.size(); }

    // Case-insensitive partial search of name/description/code across all
    // databases, de-duplicated by card code. Applies Kewl<->Killer aliasing.
    std::vector<CardInfo> search(const std::string& query, int limit = 80) const;
    // Like search() but matches ONLY the effect/description text — for finding
    // every card that "mentions" a term (e.g. "banish", an archetype name).
    std::vector<CardInfo> searchText(const std::string& query, int limit = 80) const;
    std::vector<CardInfo> filter(uint32_t typeMask, uint32_t attrMask,
                                  int level, int limit = 100) const;

    // ocgcore cardReader callback — fill OCG_CardData from the databases.
    static void cardReaderCb(void* payload, uint32_t code, OCG_CardData* out);
    static void cardReaderDoneCb(void* payload, OCG_CardData* data);

private:
    struct Database {
        void*       handle = nullptr;   // sqlite3*
        std::string path;               // absolute path
    };
    std::vector<Database> m_dbs;        // [0] = primary, rest = fallbacks
    // "!system" strings from strings.conf, keyed by id.
    std::unordered_map<uint32_t, std::string> m_sysStrings;

    // Read a single card from one database handle (no fallback).
    CardInfo readOne(void* handle, uint32_t code) const;
};
