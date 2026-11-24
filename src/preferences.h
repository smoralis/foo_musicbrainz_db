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

// Whether the Now playing browser opens the playing artist's MusicBrainz page by itself
// when the artist changes (off by default).
bool auto_load_artist_page();
void set_auto_load_artist_page(bool enabled); // toggled from the Now playing tab

// Whether the Now playing browser is forced to dark mode (colour scheme and automatic page
// darkening) instead of following the panel's light/dark colours. Off by default; the page
// darkening part is applied when the browser starts.
bool force_dark_browser();

// Whether the Now playing browser blocks requests to known ad and tracker hosts and
// pop-ups that the page opens by itself. Off by default; takes effect immediately.
bool block_ads_in_browser();

// Whether the embedded browser is not used at all: the Now playing tab then shows only the
// artist information over its full width, and links open in the default browser.
bool embedded_browser_disabled();

}

// ListenBrainz Labs Similar Artists algorithm identifier.
namespace mbdb_prefs {
std::string listenbrainz_similar_artists_algorithm();
}
