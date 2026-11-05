#include "stdafx.h"
#include "provider.h"
#include "debug_log.h"
#include <cctype>
#include <chrono>

std::shared_ptr<musicbrainz_db_provider> musicbrainz_db_get_provider();

namespace {
std::mutex g_lookup_mutex;
std::mutex g_last_mutex;
std::string g_last_key; // last "artist\ntitle" queued, used to drop repeated stream updates

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool is_mbid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

struct track_query {
    std::string artist, title;
    std::string artist_mbid; // set when the file already carries a MusicBrainz artist id
};

// Reads artist/title from tags. An artist alone is enough (artist-name search).
// Radio streams often deliver only a combined "Artist - Title" StreamTitle, so
// when there is no artist tag, split the title field.
bool extract_artist_title(const file_info& info, std::string& artist, std::string& title) {
    const char* a = info.meta_get("artist", 0);
    const char* t = info.meta_get("title", 0);
    artist = trim(a ? a : "");
    title = trim(t ? t : "");
    if (!artist.empty()) return true;
    const auto pos = title.find(" - ");
    if (pos == std::string::npos || pos == 0) return false;
    artist = trim(title.substr(0, pos));
    title = trim(title.substr(pos + 3));
    if (artist.empty()) return false;
    MBDB_LOG("split combined title into artist='%s', title='%s'", artist.c_str(), title.c_str());
    return true;
}

// A MusicBrainz artist id in the tags wins: no title search is needed then.
bool extract_track_query(const file_info& info, track_query& q) {
    q = track_query();
    // Field names are matched case-insensitively. Taggers differ in how they
    // spell the MusicBrainz artist id field.
    static const char* const kMbidFields[] = { "MUSICBRAINZ_ARTISTID", "MUSICBRAINZ ARTIST ID" };
    for (const char* field : kMbidFields) {
        const char* id = info.meta_get(field, 0); // first value if multi-valued
        if (id == nullptr || *id == 0) continue;
        std::string v = trim(id);
        for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (is_mbid(v)) {
            q.artist_mbid = v;
            const char* a = info.meta_get("artist", 0);
            const char* t = info.meta_get("title", 0);
            q.artist = a ? a : "";
            q.title = t ? t : "";
            MBDB_LOG("found artist MBID %s in tag '%s'", v.c_str(), field);
            return true;
        }
        MBDB_LOG("ignoring invalid %s tag value '%s'", field, id);
    }
    return extract_artist_title(info, q.artist, q.title);
}

// Only copies metadata and schedules work. Network I/O and SQLite access happen
// on a worker thread, never inside foobar2000's playback callback.
void queue_lookup(const char* source, const track_query& q, bool dedupe) {
    const bool haveMbid = !q.artist_mbid.empty();
    {
        const std::string key = haveMbid ? "mbid:" + q.artist_mbid : q.artist + "\n" + q.title;
        std::lock_guard<std::mutex> lock(g_last_mutex);
        if (dedupe && key == g_last_key) {
            MBDB_LOG("%s: unchanged (%s), skipping", source,
                haveMbid ? q.artist_mbid.c_str() : (q.artist + " - " + q.title).c_str());
            return;
        }
        g_last_key = key;
    }
    if (haveMbid)
        MBDB_LOG("%s: artist MBID %s from tags, queueing artist lookup (no title search)", source, q.artist_mbid.c_str());
    else
        MBDB_LOG("%s: '%s' - '%s', queueing lookup (artist+title, then artist)", source, q.artist.c_str(), q.title.c_str());
    fb2k::splitTask([q] {
        try {
            MBDB_LOG("worker: waiting for lookup lock");
            std::lock_guard<std::mutex> serial(g_lookup_mutex);
            // Snapshot the provider so shutdown cannot destroy it mid-lookup.
            auto provider = musicbrainz_db_get_provider();
            if (!provider) { MBDB_LOG("worker: provider not available, aborting"); return; }
            const auto t0 = std::chrono::steady_clock::now();
            std::string artistMbid;
            bool matched;
            if (!q.artist_mbid.empty()) {
                artistMbid = q.artist_mbid;
                matched = provider->ensure_artist(q.artist_mbid.c_str());
            } else {
                matched = provider->search_artist_title(q.artist.c_str(), q.title.c_str(), artistMbid);
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (matched) {
                pfc::string_formatter message;
                if (!q.artist_mbid.empty())
                    message << "foo_musicbrainz_db: artist " << artistMbid.c_str() << " ready (from tags)";
                else
                    message << "foo_musicbrainz_db: matched " << q.artist.c_str()
                        << (q.title.empty() ? "" : " - ") << q.title.c_str() << " => " << artistMbid.c_str();
                console::print(message.get_ptr());
                MBDB_LOG("worker: lookup finished in %lld ms (ok)", static_cast<long long>(ms));
            } else {
                MBDB_LOG("worker: lookup finished in %lld ms (failed)", static_cast<long long>(ms));
            }
        } catch (const std::exception& e) {
            pfc::string_formatter message;
            message << "foo_musicbrainz_db: lookup error: " << e.what();
            console::print(message.get_ptr());
        }
    });
}

class musicbrainz_playback_monitor : public play_callback_static {
public:
    t_uint32 get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_dynamic_info_track;
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        if (track.is_empty()) { MBDB_LOG("new track: empty handle, skipping"); return; }
        file_info_impl info;
        if (!track->get_info(info)) { MBDB_LOG("new track: get_info failed (not loaded yet?), skipping"); return; }
        track_query q;
        if (!extract_track_query(info, q)) {
            // Normal for a stream that hasn't sent metadata yet; dynamic info will follow.
            MBDB_LOG("new track: no artist MBID or usable artist/title yet, waiting for dynamic info");
            return;
        }
        queue_lookup("new track", q, false);
    }

    // Fired when a stream (e.g. Shoutcast/Icecast) delivers new in-stream metadata.
    void on_playback_dynamic_info_track(const file_info& info) override {
        track_query q;
        if (!extract_track_query(info, q)) {
            MBDB_LOG("dynamic info (track): no artist MBID or usable artist/title, skipping");
            return;
        }
        queue_lookup("dynamic info (track)", q, true);
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_stop(play_control::t_stop_reason) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};

FB2K_SERVICE_FACTORY(musicbrainz_playback_monitor);
}
