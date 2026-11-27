#include "database.h"
#include "debug_log.h"
#include "artist_search.h"
#include <nlohmann/json.hpp>

namespace {

// Text the artist table search looks through, taken from the stored MusicBrainz record: five
// sections separated by 0x1F -- names (sort name, aliases and their sort names), disambiguation,
// areas, genres, tags -- each folded (see artist_search.h) with lines separated by '\n'.
// A record without usable JSON gets the five empty sections, so it is not rebuilt again and again.
const char* const kEmptySearch = "\x1f\x1f\x1f\x1f";

std::string make_search_text(const std::string& json_text) {
    if (json_text.empty()) return kEmptySearch;
    nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return kEmptySearch;

    auto add = [](std::string& dst, const nlohmann::json& v) {
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            if (!s.empty()) { dst += s; dst += '\n'; }
        }
    };
    std::string names, extra, areas, genres, tags;
    if (j.contains("sort-name")) add(names, j["sort-name"]);
    if (j.contains("aliases") && j["aliases"].is_array()) {
        for (const auto& a : j["aliases"]) {
            if (!a.is_object()) continue;
            if (a.contains("name")) add(names, a["name"]);
            if (a.contains("sort-name")) add(names, a["sort-name"]);
        }
    }
    if (j.contains("disambiguation")) add(extra, j["disambiguation"]);
    for (const char* key : {"area", "begin-area", "end-area", "begin_area", "end_area"}) {
        if (j.contains(key) && j[key].is_object() && j[key].contains("name")) add(areas, j[key]["name"]);
    }
    for (const char* key : {"genres", "tags"}) {
        if (!j.contains(key) || !j[key].is_array()) continue;
        std::string& dst = std::string(key) == "genres" ? genres : tags;
        for (const auto& g : j[key]) {
            if (g.is_object() && g.contains("name")) add(dst, g["name"]);
        }
    }
    const char sep = '\x1f';
    return mbdb_search::fold(names) + sep + mbdb_search::fold(extra) + sep + mbdb_search::fold(areas) + sep +
           mbdb_search::fold(genres) + sep + mbdb_search::fold(tags);
}

// Fills the `search` column for rows that do not have it yet or have the older, shorter format
// (fewer than the four section separators of the current one).
void backfill_search_column(sqlite3* db) {
    if (!db) return;
    sqlite3_stmt* select = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT mbid,json FROM artists WHERE search IS NULL OR "
        "(length(search) - length(replace(search, char(31), ''))) < 4", -1, &select, nullptr) != SQLITE_OK) return;
    std::vector<std::pair<std::string, std::string>> todo;
    while (sqlite3_step(select) == SQLITE_ROW) {
        const unsigned char* id = sqlite3_column_text(select, 0);
        const unsigned char* js = sqlite3_column_text(select, 1);
        todo.emplace_back(id ? reinterpret_cast<const char*>(id) : "",
                          js ? reinterpret_cast<const char*>(js) : "");
    }
    sqlite3_finalize(select);
    if (todo.empty()) return;

    MBDB_LOG("sqlite: building the search text of %zu artist(s)", todo.size());
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE artists SET search=? WHERE mbid=?", -1, &update, nullptr) == SQLITE_OK) {
        for (const auto& row : todo) {
            const std::string text = make_search_text(row.second); // empty when there is no record yet
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
            sqlite3_bind_text(update, 1, text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(update, 2, row.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(update);
        }
        sqlite3_finalize(update);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

} // namespace

musicbrainz_database::~musicbrainz_database() { close(); }

bool musicbrainz_database::open(const std::string& filename) {
    close();
    const int rc = sqlite3_open(filename.c_str(), &m_db);
    if (rc != SQLITE_OK) MBDB_LOG("sqlite: open failed (%d): %s", rc, m_db ? sqlite3_errmsg(m_db) : "no handle");
    else {
        sqlite3_busy_timeout(m_db, 5000);
        MBDB_LOG("sqlite: opened %s (sqlite %s)", filename.c_str(), sqlite3_libversion());
    }
    return rc == SQLITE_OK;
}
void musicbrainz_database::close() {
    if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
}
bool musicbrainz_database::initialize() {
    if (!m_db) return false;
    const char* sql = R"SQL(
        PRAGMA journal_mode=WAL;
        PRAGMA foreign_keys=ON;
        CREATE TABLE IF NOT EXISTS artists(
            mbid TEXT PRIMARY KEY, name TEXT NOT NULL, type TEXT,
            country TEXT, area TEXT, begin_area TEXT, end_area TEXT,
            begin_date TEXT, end_date TEXT, disambiguation TEXT,
            json TEXT,
            search TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_artist_name ON artists(name);
        -- Recordings are never stored; drop tables left by earlier versions.
        DROP TABLE IF EXISTS recording_artists;
        DROP TABLE IF EXISTS recordings;
    )SQL";
    char* err = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) { MBDB_LOG("sqlite: schema init failed (%d): %s", rc, err ? err : "?"); sqlite3_free(err); }
    else MBDB_LOG("sqlite: schema ready");
    if (rc != SQLITE_OK) return false;
    // Databases created before the full-JSON change lack the json columns.
    for (const char* table : {"artists"}) {
        const std::string probeSql = std::string("SELECT json FROM ") + table + " LIMIT 0";
        sqlite3_stmt* probe = nullptr;
        if (sqlite3_prepare_v2(m_db, probeSql.c_str(), -1, &probe, nullptr) == SQLITE_OK) {
            sqlite3_finalize(probe);
            continue;
        }
        MBDB_LOG("sqlite: migrating %s table (adding json column)", table);
        const std::string alter = std::string("ALTER TABLE ") + table + " ADD COLUMN json TEXT";
        if (sqlite3_exec(m_db, alter.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
            MBDB_LOG("sqlite: migration of %s failed: %s", table, sqlite3_errmsg(m_db));
            return false;
        }
    }
    // Databases created before the table search lack the search column.
    {
        sqlite3_stmt* probe = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT search FROM artists LIMIT 0", -1, &probe, nullptr) == SQLITE_OK) {
            sqlite3_finalize(probe);
        } else {
            MBDB_LOG("sqlite: migrating artists table (adding search column)");
            if (sqlite3_exec(m_db, "ALTER TABLE artists ADD COLUMN search TEXT", nullptr, nullptr, nullptr) != SQLITE_OK) {
                MBDB_LOG("sqlite: adding the search column failed: %s", sqlite3_errmsg(m_db));
                return false;
            }
        }
    }
    return true;
}
bool musicbrainz_database::save_artist(const artist_record& a) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || a.mbid.empty()) return false;
    const char* sql = R"SQL(
        INSERT INTO artists(mbid,name,type,country,area,begin_area,end_area,
                            begin_date,end_date,disambiguation,json,search)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(mbid) DO UPDATE SET
        name=excluded.name,type=excluded.type,country=excluded.country,
        area=excluded.area,begin_area=excluded.begin_area,
        end_area=excluded.end_area,begin_date=excluded.begin_date,
        end_date=excluded.end_date,disambiguation=excluded.disambiguation,
        json=excluded.json,search=excluded.search;
    )SQL";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &s, nullptr) != SQLITE_OK) return false;
    const std::string* v[] = {&a.mbid,&a.name,&a.type,&a.country,&a.area,
        &a.begin_area,&a.end_area,&a.begin_date,&a.end_date,&a.disambiguation,&a.raw_json};
    for (int i=0;i<11;++i)
        sqlite3_bind_text(s,i+1,v[i]->c_str(),-1,SQLITE_TRANSIENT);
    const std::string search = make_search_text(a.raw_json);
    sqlite3_bind_text(s,12,search.c_str(),-1,SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (!ok) MBDB_LOG("sqlite: save_artist failed: %s", sqlite3_errmsg(m_db));
    else MBDB_LOG("sqlite: saved artist %s", a.mbid.c_str());
    sqlite3_finalize(s);
    return ok;
}
bool musicbrainz_database::get_artist(const std::string& mbid, artist_record& a) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    const char* sql = "SELECT mbid,name,type,country,area,begin_area,end_area,"
                      "begin_date,end_date,disambiguation,json FROM artists WHERE mbid=?";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(m_db,sql,-1,&s,nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_text(s,1,mbid.c_str(),-1,SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(s)==SQLITE_ROW) {
        auto get=[&](int i)->std::string {
            auto p=sqlite3_column_text(s,i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        a.mbid=get(0); a.name=get(1); a.type=get(2); a.country=get(3);
        a.area=get(4); a.begin_area=get(5); a.end_area=get(6);
        a.begin_date=get(7); a.end_date=get(8); a.disambiguation=get(9);
        a.raw_json=get(10);
        // Rows saved before full JSON was stored count as a miss so they get refetched.
        found=!a.raw_json.empty();
        if(!found) MBDB_LOG("sqlite: artist %s cached without full json, will refetch", mbid.c_str());
    }
    sqlite3_finalize(s);
    return found;
}
long long musicbrainz_database::count_artists() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return -1;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM artists", -1, &s, nullptr) != SQLITE_OK) return -1;
    long long n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}
bool musicbrainz_database::list_artists(std::vector<artist_list_item>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    out.clear();
    if (!m_db) return false;
    backfill_search_column(m_db); // one-time work for databases made before the search text existed
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT mbid,name,country,type,search,area,begin_date,end_date FROM artists ORDER BY name COLLATE NOCASE",
        -1, &s, nullptr) != SQLITE_OK) return false;
    auto get = [&](int i) -> std::string {
        auto p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    while (sqlite3_step(s) == SQLITE_ROW) {
        artist_list_item item;
        item.mbid = get(0); item.name = get(1); item.country = get(2); item.type = get(3);
        item.search = get(4);
        item.area = get(5); item.begin = get(6); item.end = get(7);
        out.push_back(std::move(item));
    }
    sqlite3_finalize(s);
    return true;
}

size_t musicbrainz_database::delete_artists(const std::vector<std::string>& mbids) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || mbids.empty()) return 0;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        MBDB_LOG("sqlite: delete_artists could not start a transaction: %s", sqlite3_errmsg(m_db));
        return 0;
    }
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM artists WHERE mbid=?", -1, &s, nullptr) != SQLITE_OK) {
        MBDB_LOG("sqlite: delete_artists prepare failed: %s", sqlite3_errmsg(m_db));
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        return 0;
    }
    size_t deleted = 0;
    for (const auto& id : mbids) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_DONE) deleted += static_cast<size_t>(sqlite3_changes(m_db));
    }
    sqlite3_finalize(s);
    if (sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        MBDB_LOG("sqlite: delete_artists commit failed: %s", sqlite3_errmsg(m_db));
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        return 0;
    }
    MBDB_LOG("sqlite: deleted %zu of %zu requested artist(s)", deleted, mbids.size());
    return deleted;
}

bool musicbrainz_database::get_artist_jsons(const std::vector<std::string>& mbids,
        std::vector<std::pair<std::string, std::string>>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    out.clear();
    if (!m_db) return false;
    sqlite3_stmt* s = nullptr;
    auto text = [&](int i) -> std::string {
        auto p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    if (mbids.empty()) {
        if (sqlite3_prepare_v2(m_db, "SELECT mbid,json FROM artists WHERE json IS NOT NULL AND json<>'' "
            "ORDER BY name COLLATE NOCASE", -1, &s, nullptr) != SQLITE_OK) return false;
        while (sqlite3_step(s) == SQLITE_ROW) out.emplace_back(text(0), text(1));
        sqlite3_finalize(s);
        return true;
    }
    if (sqlite3_prepare_v2(m_db, "SELECT json FROM artists WHERE mbid=?", -1, &s, nullptr) != SQLITE_OK) return false;
    for (const auto& id : mbids) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_text(s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            std::string j = text(0);
            if (!j.empty()) out.emplace_back(id, std::move(j));
        }
    }
    sqlite3_finalize(s);
    return true;
}

int musicbrainz_database::find_artists_by_name(const std::string& name, std::string& mbid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    mbid.clear();
    if (!m_db || name.empty()) return 0;
    sqlite3_stmt* s = nullptr;
    // Only artists whose full record is stored count: they are what a lookup would return anyway.
    if (sqlite3_prepare_v2(m_db,
        "SELECT mbid FROM artists WHERE name = ? COLLATE NOCASE AND json IS NOT NULL AND json <> '' LIMIT 2",
        -1, &s, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (count == 0) {
            const unsigned char* p = sqlite3_column_text(s, 0);
            if (p) mbid = reinterpret_cast<const char*>(p);
        }
        ++count;
    }
    sqlite3_finalize(s);
    return count;
}
