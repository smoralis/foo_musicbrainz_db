#pragma once
#include <string>

// One row of the artist table shown in the panel.
struct artist_list_item {
    std::string mbid, name, country, type;
    std::string area, begin, end; // main area name, begin and end dates (as stored: YYYY[-MM[-DD]])
    // Extra text the table search looks through (sort name and aliases, disambiguation, areas,
    // genres, tags: five sections separated by 0x1F), folded to lower case without accents;
    // built by the database. Empty if not available.
    std::string search;
};

struct artist_record {
    std::string mbid, name, type, country, area;
    std::string begin_area, end_area, begin_date, end_date, disambiguation;
    // Complete MusicBrainz artist response (genres, tags, ratings, annotation,
    // aliases, url-rels, ...). The fields above are only a quick-access subset.
    std::string raw_json;
};
