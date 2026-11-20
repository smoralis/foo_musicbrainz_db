#pragma once
#include <string>

// Settings edited on Preferences > Tools > MusicBrainz DB.
// All getters are thread-safe and may be called from worker threads.
namespace mbdb_prefs {

// Whether debug lines are also written to the foobar2000 console.
bool debug_logging();

// User-defined titleformatting patterns that replace the plain %artist% and
// %title% tags when a track is looked up. Empty means "use the tag".
std::string artist_pattern();
std::string title_pattern();

// Seconds to wait after a stream's metadata changes before the lookup is
// started (0 = immediately). A newer change restarts the wait.
unsigned dynamic_delay_seconds();

// Minimum time between two MusicBrainz requests, in seconds. Never below 1
// (MusicBrainz allows an average of one request per second).
unsigned rate_limit_seconds();

}
