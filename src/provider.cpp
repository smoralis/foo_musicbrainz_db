#include "provider.h"
#include "debug_log.h"

bool musicbrainz_db_provider::initialize(const std::string& db,
        const std::string& repo) {
    MBDB_LOG("provider: initializing (db=%s, repo=%s)", db.c_str(), repo.c_str());
    if(!m_database.open(db)||!m_database.initialize()) {
        MBDB_LOG("provider: database open/initialize failed");
        return false;
    }
    m_exporter=std::make_unique<json_exporter>(repo);
    MBDB_LOG("provider: ready");
    return true;
}
// Loads an artist from the cache, or fetches the full record from MusicBrainz
// and stores it (SQLite + artists_all.json).
bool musicbrainz_db_provider::load_artist(const std::string& mbid,artist_record& a) {
    if(m_database.get_artist(mbid,a)) {
        MBDB_LOG("artist cache HIT (%s)", mbid.c_str());
        return true;
    }
    MBDB_LOG("artist cache MISS (%s), querying MusicBrainz", mbid.c_str());
    if(!m_client.get_artist(mbid,a)||!m_database.save_artist(a)) {
        MBDB_LOG("artist fetch/save FAILED (%s)", mbid.c_str());
        return false;
    }
    if(m_exporter) {
        const bool ok=m_exporter->export_artist(a);
        MBDB_LOG("export artist json %s", ok?"ok":"FAILED");
    }
    return true;
}
// Search order (the MBID-tag case is handled by the caller via ensure_artist):
//   1. artist + title   2. artist name only
bool musicbrainz_db_provider::search_artist_title(const char* artist,
        const char* title,std::string& mbid) {
    mbid.clear();
    if(!artist||!*artist) return false;
    const bool haveTitle=title&&*title;
    bool found=false;
    if(haveTitle) {
        MBDB_LOG("search: artist+title '%s' - '%s'", artist, title);
        found=m_client.search_artist_title(artist,title,mbid);
        if(!found) MBDB_LOG("search: artist+title found nothing, falling back to artist name");
    }
    if(!found) {
        MBDB_LOG("search: artist name '%s'", artist);
        if(!m_client.search_artist_name(artist,mbid)) {
            MBDB_LOG("search: no artist found");
            mbid.clear();
            return false;
        }
    }
    artist_record a;
    if(!load_artist(mbid,a)) { mbid.clear(); return false; }
    MBDB_LOG("search: done, artist mbid %s", mbid.c_str());
    return true;
}
bool musicbrainz_db_provider::ensure_artist(const char* mbid) {
    if(!mbid||!*mbid) return false;
    MBDB_LOG("ensure_artist: using MBID from tags (%s), skipping title search", mbid);
    artist_record a;
    return load_artist(mbid,a);
}
bool musicbrainz_db_provider::get_artist_country(const char* mbid,
        std::string& country) {
    country.clear(); if(!mbid) return false;
    artist_record a;
    if(!load_artist(mbid,a)) return false;
    country=a.country;
    MBDB_LOG("get_artist_country: %s => '%s'", mbid, country.c_str());
    return !country.empty();
}
bool musicbrainz_db_provider::get_artist_json(const char* mbid,
        std::string& out) {
    out.clear(); if(!mbid) return false;
    artist_record a;
    if(!load_artist(mbid,a)) return false;
    out=a.raw_json; // full MusicBrainz response
    return !out.empty();
}
