#pragma once
// Thin façade between the panel and the provider for the artist table.
#include <string>
#include <vector>
#include "../include/musicbrainz_db_types.h"

bool mbdb_artist_list(std::vector<artist_list_item>& out);
// Removes the artists from the database and from every JSON file. Returns how many were deleted.
size_t mbdb_artist_delete(const std::vector<std::string>& mbids);
// Writes a JSON file at `path`. Empty `mbids` = every artist.
//   list_only == false: { "<mbid>": <full MusicBrainz record>, ... }
//   list_only == true:  [ { "mbid", "name", "country", "type" }, ... ]
bool mbdb_artist_export(const std::vector<std::string>& mbids, const std::wstring& path,
                        bool list_only, size_t& count, std::string& error);
// The stored record (including the full MusicBrainz JSON) of one artist.
bool mbdb_artist_get(const std::string& mbid, artist_record& out);
