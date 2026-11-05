# foo_musicbrainz_db

A foobar2000 component that quietly builds a local MusicBrainz artist database while you listen.

When a track starts (or a radio stream sends new metadata), the component looks up the artist on MusicBrainz, then stores the complete artist record in a local SQLite cache and in a set of plain JSON files. Each artist is fetched only once; after that, lookups are served from disk.

**Version:** 0.1.0 (early release)

## Features

- **Automatic lookups.** Triggered on new track and on dynamic stream info, so internet radio (Shoutcast/Icecast) works too.
- **Uses your tags first.** If the file carries a MusicBrainz artist ID (`MUSICBRAINZ_ARTISTID` or `MUSICBRAINZ ARTIST ID`), that ID is used directly and no search is needed.
- **Falls back to searching.** Without an ID, it searches by artist + title, then by artist name alone. Streams that only send a combined `Artist - Title` string are split automatically.
- **Full records.** Stores the complete MusicBrainz artist response: genres, tags, ratings, annotation, aliases and URL relationships.
- **Local cache.** SQLite database, so repeat plays never hit the network.
- **JSON repository.** Split into small, easy-to-read files (see below) that other tools can consume.
- **Polite to MusicBrainz.** Requests are spaced at least 1.1 seconds apart, and 503/429 responses are retried with the server's `Retry-After` back-off.
- **Non-blocking.** All network and database work runs on a worker thread, never in a playback callback.

## Where your data lives

Everything is stored in your foobar2000 profile folder:

```
<profile>\foo_musicbrainz_db\
├── database\
│   └── musicbrainz.sqlite3
└── repository\
    └── artists\
        ├── artists_all.json         full MusicBrainz artist response
        ├── artists_basic.json       name, sort-name, type, gender, disambiguation, life-span, ISNIs, IPIs
        ├── artists_country.json    name, country, area, begin-area, end-area
        ├── artists_genres.json      name, genres
        ├── artists_relations.json   name, URL relationships
        └── artists_tags.json        name, tags
```

Every JSON file is a single object keyed by artist MBID, and each record also carries its `mbid`. Files are written atomically, and a file that can't be parsed is never overwritten.

## Installation

1. Download `foo_musicbrainz_db.dll` (or a `.fb2k-component` package, if provided).
2. In foobar2000, open **File > Preferences > Components**, click **Install...** and choose the file, or copy the DLL into your profile's `user-components-x64` folder.
3. Restart foobar2000.

After restart the console (**View > Console**) shows `foo_musicbrainz_db: initialized`. Play something and you'll see a `matched ...` line as artists are resolved.

## Settings

Debug logging is **off by default**. To turn it on, go to **Preferences > Advanced > Tools > MusicBrainz DB** and tick **Enable debug logging to the console**. Each debug line is prefixed with `foo_musicbrainz_db [debug]`.

Status messages (initialized, matched, errors) are always printed to the console.

## Building

Requirements:

- Visual Studio 2022 (toolset v143), C++17
- foobar2000 SDK, with this project placed in the SDK's `foobar2000` folder as `foo_musicbrainz_db`
- SQLite3 and nlohmann/json (for example via vcpkg)
- WTL (restored through NuGet, see `packages.config`)
- The SDK's `shared-x64.lib` / `shared-Win32.lib`

Open `foo_musicbrainz_db.sln` and build the `Release|x64` configuration.

To remove all debug logging from the binary entirely, set `MBDB_DEBUG_LOG` to `0` in `src/debug_log.h` or in the project's preprocessor definitions.

Before distributing a build, set the contact details in the `User-Agent` string in `src/musicbrainz_client.cpp`, as MusicBrainz asks API clients to identify themselves.

## Programmatic access

Other components and external tools can read the data directly:

- **SQLite:** open `<profile>\foo_musicbrainz_db\database\musicbrainz.sqlite3` read-only (for example with the URI `file:...?mode=ro`). The database uses WAL mode, so reading while foobar2000 is running is fine. Use a busy timeout in case a write is in progress. Your component needs its own SQLite copy (statically linked is simplest).
- **JSON:** read the files in `repository\artists\` (see above).

The `artists` table has these columns: `mbid` (primary key), `name`, `type`, `country`, `area`, `begin_area`, `end_area`, `begin_date`, `end_date`, `disambiguation` and `json` (the full MusicBrainz response). For example:

```sql
SELECT country FROM artists WHERE mbid = '<artist mbid>';
```

Please treat the database as read-only and don't write to it. The schema version is stored in `PRAGMA user_version` (currently `1`) and is incremented on any breaking change, so check it before relying on the layout.

`include/musicbrainz_db_interface.h` describes a lookup contract (`search_artist_title`, `get_artist_country`, `get_artist_json`). It is a conceptual interface for now and is not yet registered as a foobar2000 service, so it can't be called across components.

## Privacy and network use

The component sends artist names, track titles and MusicBrainz IDs from the tracks you play to `musicbrainz.org` over HTTPS. Nothing else is sent, and nothing is collected by the author.

## Credits

Metadata is provided by [MusicBrainz](https://musicbrainz.org/) and is available under its own [data licensing terms](https://musicbrainz.org/doc/About/Data_License).

## License

MIT License

