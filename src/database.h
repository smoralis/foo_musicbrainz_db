#pragma once
#include <sqlite3.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
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
    // Artists already stored (with their full record) under this name, compared
    // case-insensitively. Returns how many match, capped at 2; `mbid` is the first.
    int find_artists_by_name(const std::string& name, std::string& mbid);
    long long count_artists(); // -1 on error
    // Used by the panel's artist table (UI thread). All public calls are serialized.
    bool list_artists(std::vector<artist_list_item>& out);                 // ordered by name
    size_t delete_artists(const std::vector<std::string>& mbids);          // returns rows deleted
    // (mbid, full json) pairs; an empty `mbids` means every artist.
    bool get_artist_jsons(const std::vector<std::string>& mbids,
                          std::vector<std::pair<std::string, std::string>>& out);
private:
    std::recursive_mutex m_mutex;
    sqlite3* m_db = nullptr;
};
