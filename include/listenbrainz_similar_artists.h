#pragma once
#include <string>
#include <vector>

// Results from the public ListenBrainz Labs Similar Artists endpoint.
struct listenbrainz_similar_artist {
    std::string mbid;
    std::string name;
    std::string comment;
    std::string type;
    std::string gender;
    std::string reference_mbid;
    double score = 0.0;
};

// Call fetch() from a worker thread; it performs synchronous WinHTTP I/O.
// Successful results are cached in-process for the configured TTL.
class listenbrainz_similar_artists {
public:
    bool fetch(const std::string& artist_mbid,
               const std::string& algorithm,
               std::vector<listenbrainz_similar_artist>& out,
               std::string* error = nullptr);
    static const char* default_algorithm();
};

// Fetch the currently published algorithm choices from the Labs viewer page.
// This scrapes the server-rendered select list because Labs does not expose a separate
// public algorithms JSON endpoint.
bool listenbrainz_fetch_available_algorithms(std::vector<std::string>& out, std::string* error = nullptr);
