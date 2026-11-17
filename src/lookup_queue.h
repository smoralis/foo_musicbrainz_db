#pragma once
#include <cstddef>

// Lookups run one at a time on a single background thread (MusicBrainz is
// rate limited), fed by this FIFO queue.

// Queues a lookup for every distinct artist among the currently selected tracks
// (foobar2000's selection, falling back to the active playlist's selection).
// Returns the number of lookups queued.
size_t mbdb_search_selection();

// Drops every lookup that is still waiting. A lookup already running finishes.
void mbdb_cancel_queued();

// Stops the worker thread (call from on_quit, after musicbrainz_client_abort()).
void mbdb_lookup_queue_shutdown();
