#pragma once
#include <string>

// One row of the artist table shown in the panel.
struct artist_list_item {
    std::string mbid, name, country, type;
};

struct artist_record {
    std::string mbid, name, type, country, area;
    std::string begin_area, end_area, begin_date, end_date, disambiguation;
    // Complete MusicBrainz artist response (genres, tags, ratings, annotation,
    // aliases, url-rels, ...). The fields above are only a quick-access subset.
    std::string raw_json;
};
