#pragma once
#include <sqlite3.h>
#include <string>
#include "../include/musicbrainz_db_types.h"

class musicbrainz_database {
public:
    musicbrainz_database() = default;
    ~musicbrainz_database();

    bool open(const std::string& filename);
    void close();
    bool initialize();
    bool save_artist(const artist_record& artist);
    bool get_artist(const std::string& mbid, artist_record& artist);
private:
    sqlite3* m_db = nullptr;
};
