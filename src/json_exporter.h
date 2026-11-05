#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>
#include "../include/musicbrainz_db_types.h"

// Writes single-file stores under <repository>\artists\. Every file is one JSON
// object keyed by MBID, and every record also carries its "mbid" (and, except in
// all_artists.json, the artist "name"):
//   all_artists.json        full MusicBrainz artist response
//   artists_basic.json      name, sort-name, type, gender, disambiguation, life-span, isnis, ipis
//   artists_country.json    name, country, area, begin-area, end-area
//   artists_genres.json     name, genres
//   artists_relations.json  name, relations (url-rels)
//   artists_tags.json       name, tags
class json_exporter {
public:
    explicit json_exporter(const std::filesystem::path& root);
    bool export_artist(const artist_record& artist);
private:
    enum store_id { kAll, kBasic, kCountry, kGenres, kRelations, kTags, kStoreCount };
    struct store {
        std::filesystem::path file;
        nlohmann::json data = nlohmann::json::object();
        bool loaded = false;
    };
    std::filesystem::path m_root;
    std::mutex m_mutex;
    store m_stores[kStoreCount];

    // Caller holds m_mutex.
    bool update_store(store& s, const std::string& mbid, nlohmann::json value);
    bool write_atomic(const std::filesystem::path&, const std::string&);
    static bool valid_mbid(const std::string&);
};
