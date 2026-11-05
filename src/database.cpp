#include "database.h"
#include "debug_log.h"

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
            json TEXT
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
    // Schema version for other components/tools reading this file
    // (PRAGMA user_version). Bump it on any breaking schema change.
    if (sqlite3_exec(m_db, "PRAGMA user_version=1;", nullptr, nullptr, nullptr) != SQLITE_OK)
        MBDB_LOG("sqlite: could not set schema version: %s", sqlite3_errmsg(m_db));
    return true;
}
bool musicbrainz_database::save_artist(const artist_record& a) {
    if (!m_db || a.mbid.empty()) return false;
    const char* sql = R"SQL(
        INSERT INTO artists(mbid,name,type,country,area,begin_area,end_area,
                            begin_date,end_date,disambiguation,json)
        VALUES(?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(mbid) DO UPDATE SET
        name=excluded.name,type=excluded.type,country=excluded.country,
        area=excluded.area,begin_area=excluded.begin_area,
        end_area=excluded.end_area,begin_date=excluded.begin_date,
        end_date=excluded.end_date,disambiguation=excluded.disambiguation,
        json=excluded.json;
    )SQL";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &s, nullptr) != SQLITE_OK) return false;
    const std::string* v[] = {&a.mbid,&a.name,&a.type,&a.country,&a.area,
        &a.begin_area,&a.end_area,&a.begin_date,&a.end_date,&a.disambiguation,&a.raw_json};
    for (int i=0;i<11;++i)
        sqlite3_bind_text(s,i+1,v[i]->c_str(),-1,SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (!ok) MBDB_LOG("sqlite: save_artist failed: %s", sqlite3_errmsg(m_db));
    else MBDB_LOG("sqlite: saved artist %s", a.mbid.c_str());
    sqlite3_finalize(s);
    return ok;
}
bool musicbrainz_database::get_artist(const std::string& mbid, artist_record& a) {
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
