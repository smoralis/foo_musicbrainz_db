#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Lookups run one at a time on a single background thread (MusicBrainz is
// rate limited), fed by this FIFO queue.

// Queues a lookup for every distinct artist among the currently selected tracks
// (foobar2000's selection, falling back to the active playlist's selection).
// Returns the number of lookups queued.
size_t mbdb_search_selection();

// Queues a lookup by MusicBrainz artist ID for each distinct valid MBID in `mbids`
// (cached artists finish without a network request). Counted as a batch, so the panel
// shows the same progress bar as "Search selected". Returns the number queued.
size_t mbdb_search_artist_mbids(const std::vector<std::string>& mbids);

// Drops every lookup that is still waiting. A lookup already running finishes.
void mbdb_cancel_queued();

// Refreshes the Now playing view after a successful "Search selected": the playing
// artist may have just been stored. Re-publishes the playing artist (so the view
// reloads its record), or, when the playing track had no match, looks it up again.
void mbdb_reload_now_playing();

// Stops the worker thread (call from on_quit, after musicbrainz_client_abort()).
void mbdb_lookup_queue_shutdown();
