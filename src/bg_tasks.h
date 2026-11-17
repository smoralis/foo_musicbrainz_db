#pragma once
#include <functional>

// One background thread for panel-initiated work (listing, deleting, exporting),
// so the UI thread never waits on SQLite or on file I/O. Separate from the lookup
// queue, so these never wait behind rate-limited MusicBrainz requests.
void mbdb_run_async(std::function<void()> task);

// Stops the thread and drops queued tasks (call from on_quit).
void mbdb_bg_shutdown();
