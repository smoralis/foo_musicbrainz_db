#pragma once
#include <string>

// Conceptual interface contract. For a real foobar2000 component service,
// replace std::string at the DLL boundary with SDK-compatible conventions
// and register/discover the service through the official SDK.
class musicbrainz_db_interface {
public:
    virtual ~musicbrainz_db_interface() = default;

    virtual bool search_artist_title(
        const char* artist_name,
        const char* track_title,
        std::string& artist_mbid) = 0;

    virtual bool get_artist_country(
        const char* artist_mbid,
        std::string& country_code) = 0;

    virtual bool get_artist_json(
        const char* artist_mbid,
        std::string& json) = 0;
};
