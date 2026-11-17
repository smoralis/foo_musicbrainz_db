#pragma once
#include "stdafx.h"

// Queues a lookup for every distinct artist among `tracks` (same rules and the
// same progress bar as the panel's "Search selected" button). Returns how many
// lookups were queued.
size_t mbdb_search_tracks(metadb_handle_list_cref tracks);
