#pragma once
#include <memory>
#include "database.h"
#include "musicbrainz_client.h"
#include "json_exporter.h"
#include "../include/musicbrainz_db_interface.h"

class musicbrainz_db_provider : public musicbrainz_db_interface {
public:
    bool initialize(const std::string& database_path,
                    const std::string& repository_path);
    bool search_artist_title(const char*,const char*,std::string&) override;
    // Caches the artist for an MBID taken from the file's tags (no title search).
    bool ensure_artist(const char* artist_mbid);
    bool get_artist_country(const char*,std::string&) override;
    bool get_artist_json(const char*,std::string&) override;
private:
    bool load_artist(const std::string& mbid, artist_record& artist);
    musicbrainz_database m_database;
    musicbrainz_client m_client;
    std::unique_ptr<json_exporter> m_exporter;
};
