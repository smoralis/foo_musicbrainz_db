#pragma once
#include "../include/musicbrainz_db_types.h"

// Opens (or brings to the front) a modeless window with everything stored about
// the artist. One window per artist. Call from the UI thread.
void mbdb_show_artist_info(const artist_list_item& item);
