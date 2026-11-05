#pragma once
#include <string>
#include "../include/musicbrainz_db_types.h"

// Wakes any lookup waiting on the rate limiter so shutdown is not delayed.
void musicbrainz_client_abort();

class musicbrainz_client {
public:
    // Title search used only to identify the artist; nothing about the
    // recording itself is kept. Returns the matching artist's MBID.
    bool search_artist_title(const std::string& artist, const std::string& title,
                             std::string& artist_mbid);
    // Last-resort search by artist name only. Returns the top hit's MBID if the
    // name matches or MusicBrainz's score is very high.
    bool search_artist_name(const std::string& artist, std::string& artist_mbid);
    bool get_artist(const std::string&, artist_record&);
private:
    bool http_get(const std::wstring&, std::string&);
};
