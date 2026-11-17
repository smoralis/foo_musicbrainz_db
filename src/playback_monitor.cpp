#include "stdafx.h"
#include "provider.h"
#include "debug_log.h"
#include "activity.h"
#include "preferences.h"
#include "lookup_queue.h"
#include "lookup_queue_tracks.h"
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <vector>

std::shared_ptr<musicbrainz_db_provider> musicbrainz_db_get_provider();

namespace {
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
    bool batch = false;      // queued by "Search selected" (drives the progress bar)
};

// Evaluates a user-defined titleformatting pattern (Preferences > Tools >
// MusicBrainz DB) against the track. Returns false when the pattern is empty,
// invalid or gives an empty result, so the caller keeps the plain tag.
bool eval_custom_field(const metadb_handle_ptr& handle, const file_info& info,
                       const std::string& pattern, const char* what, std::string& out) {
    if (pattern.empty() || handle.is_empty()) return false;
    titleformat_object::ptr script;
    if (!titleformat_compiler::get()->compile(script, pattern.c_str())) {
        MBDB_LOG("custom %s pattern '%s' is not valid, using the tag", what, pattern.c_str());
        return false;
    }
    pfc::string_formatter result;
    handle->format_title_from_external_info(info, nullptr, result, script, nullptr);
    const std::string value = trim(result.get_ptr());
    if (value.empty()) return false;
    out = value;
    return true;
}

// Replaces the tag values with the user's custom artist/title fields where set.
void apply_custom_fields(const metadb_handle_ptr& handle, const file_info& info,
                         std::string& artist, std::string& title) {
    std::string v;
    if (eval_custom_field(handle, info, mbdb_prefs::artist_pattern(), "artist", v)) {
        MBDB_LOG("custom artist field: '%s' (tag: '%s')", v.c_str(), artist.c_str());
        artist = v;
    }
    if (eval_custom_field(handle, info, mbdb_prefs::title_pattern(), "title", v)) {
        MBDB_LOG("custom title field: '%s' (tag: '%s')", v.c_str(), title.c_str());
        title = v;
    }
}

// Reads artist/title from tags. An artist alone is enough (artist-name search).
// Radio streams often deliver only a combined "Artist - Title" StreamTitle, so
// when there is no artist tag, split the title field.
bool extract_artist_title(const metadb_handle_ptr& handle, const file_info& info,
                          std::string& artist, std::string& title) {
    const char* a = info.meta_get("artist", 0);
    const char* t = info.meta_get("title", 0);
    artist = trim(a ? a : "");
    title = trim(t ? t : "");
    apply_custom_fields(handle, info, artist, title);
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
bool extract_track_query(const metadb_handle_ptr& handle, const file_info& info, track_query& q) {
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
            apply_custom_fields(handle, info, q.artist, q.title);
            MBDB_LOG("found artist MBID %s in tag '%s'", v.c_str(), field);
            return true;
        }
        MBDB_LOG("ignoring invalid %s tag value '%s'", field, id);
    }
    return extract_artist_title(handle, info, q.artist, q.title);
}

// Only copies metadata and schedules work. Network I/O and SQLite access happen
// on a worker thread, never inside foobar2000's playback callback.
// Marks a lookup as running in the activity panel for as long as it lives.
struct lookup_scope {
    explicit lookup_scope(const std::string& what) : t0(std::chrono::steady_clock::now()) {
        mbdb_activity::lookup_begin(what.c_str());
    }
    ~lookup_scope() { mbdb_activity::lookup_end(ok, ms()); }
    long long ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    }
    bool ok = false;
    std::chrono::steady_clock::time_point t0;
};

// ---- one worker thread, FIFO queue (so batches never block thread-pool threads) ----
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<track_query> g_jobs;
std::thread g_worker;
bool g_stop = false;

// Reports a batch lookup as finished however run_lookup() exits.
struct batch_guard {
    explicit batch_guard(bool batch) : batch(batch) {}
    ~batch_guard() { if (batch) mbdb_activity::batch_item_done(ok); }
    bool batch, ok = false;
};

void run_lookup(const track_query& q) {
    batch_guard guard(q.batch);
    try {
        // Snapshot the provider so shutdown cannot destroy it mid-lookup.
        auto provider = musicbrainz_db_get_provider();
        if (!provider) { MBDB_LOG("worker: provider not available, skipping"); return; }
        const std::string what = !q.artist_mbid.empty()
            ? "artist MBID " + q.artist_mbid
            : (q.title.empty() ? q.artist : q.artist + " - " + q.title);
        lookup_scope scope(what);
        std::string artistMbid;
        bool matched;
        if (!q.artist_mbid.empty()) {
            artistMbid = q.artist_mbid;
            matched = provider->ensure_artist(q.artist_mbid.c_str());
        } else {
            matched = provider->search_artist_title(q.artist.c_str(), q.title.c_str(), artistMbid);
        }
        const long long ms = scope.ms();
        scope.ok = matched;
        guard.ok = matched;
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
}

void worker_main() {
    for (;;) {
        track_query q;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
            if (g_stop) return;
            q = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        mbdb_activity::lookup_dequeued();
        run_lookup(q);
    }
}

bool enqueue(const track_query& q) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    if (g_stop) return false;
    if (!g_worker.joinable()) g_worker = std::thread(worker_main);
    g_jobs.push_back(q);
    mbdb_activity::lookup_queued();
    g_queue_cv.notify_one();
    return true;
}

// Only copies metadata and queues work. Network I/O and SQLite access happen on
// the worker thread, never inside foobar2000's playback callback or the UI thread.
bool queue_lookup(const char* source, const track_query& q, bool dedupe) {
    const bool haveMbid = !q.artist_mbid.empty();
    {
        const std::string key = haveMbid ? "mbid:" + q.artist_mbid : q.artist + "\n" + q.title;
        std::lock_guard<std::mutex> lock(g_last_mutex);
        if (dedupe && key == g_last_key) {
            MBDB_LOG("%s: unchanged (%s), skipping", source,
                haveMbid ? q.artist_mbid.c_str() : (q.artist + " - " + q.title).c_str());
            return false;
        }
        g_last_key = key;
    }
    if (haveMbid)
        MBDB_LOG("%s: artist MBID %s from tags, queueing artist lookup (no title search)", source, q.artist_mbid.c_str());
    else
        MBDB_LOG("%s: '%s' - '%s', queueing lookup (artist+title, then artist)", source, q.artist.c_str(), q.title.c_str());
    if (!enqueue(q)) { MBDB_LOG("%s: shutting down, lookup dropped", source); return false; }
    return true;
}

// ---- delayed dynamic-info lookups ----
// Streams can change their metadata in quick succession (jingles, ads, partial
// updates). With a delay configured, the lookup starts only once the metadata
// has been stable for that long; a newer change restarts the wait.
std::mutex g_delay_mutex;
std::condition_variable g_delay_cv;
std::thread g_delay_thread;
bool g_delay_stop = false;
bool g_delay_pending = false;
track_query g_delay_query;
std::chrono::steady_clock::time_point g_delay_deadline;

void delay_main() {
    std::unique_lock<std::mutex> lock(g_delay_mutex);
    for (;;) {
        g_delay_cv.wait(lock, [] { return g_delay_stop || g_delay_pending; });
        if (g_delay_stop) return;
        // Sleep until the deadline; a notify means it moved or the job was cancelled.
        while (g_delay_pending && !g_delay_stop) {
            if (g_delay_cv.wait_until(lock, g_delay_deadline) == std::cv_status::timeout) break;
        }
        if (g_delay_stop) return;
        if (!g_delay_pending || std::chrono::steady_clock::now() < g_delay_deadline) continue;
        track_query q = std::move(g_delay_query);
        g_delay_pending = false;
        lock.unlock();
        queue_lookup("dynamic info (track, delayed)", q, true);
        lock.lock();
    }
}

void schedule_delayed(const track_query& q, unsigned seconds) {
    std::lock_guard<std::mutex> lock(g_delay_mutex);
    if (g_delay_stop) return;
    if (!g_delay_thread.joinable()) g_delay_thread = std::thread(delay_main);
    g_delay_query = q;
    g_delay_pending = true;
    g_delay_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    g_delay_cv.notify_all();
}

void cancel_delayed() {
    std::lock_guard<std::mutex> lock(g_delay_mutex);
    if (!g_delay_pending) return;
    g_delay_pending = false;
    g_delay_cv.notify_all();
}

class musicbrainz_playback_monitor : public play_callback_static {
public:
    t_uint32 get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_dynamic_info_track;
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        cancel_delayed(); // a pending stream lookup belongs to the previous track
        if (track.is_empty()) { MBDB_LOG("new track: empty handle, skipping"); return; }
        file_info_impl info;
        if (!track->get_info(info)) { MBDB_LOG("new track: get_info failed (not loaded yet?), skipping"); return; }
        track_query q;
        if (!extract_track_query(track, info, q)) {
            // Normal for a stream that hasn't sent metadata yet; dynamic info will follow.
            MBDB_LOG("new track: no artist MBID or usable artist/title yet, waiting for dynamic info");
            return;
        }
        queue_lookup("new track", q, false);
    }

    // Fired when a stream (e.g. Shoutcast/Icecast) delivers new in-stream metadata.
    void on_playback_dynamic_info_track(const file_info& info) override {
        track_query q;
        // The playing track's handle is only needed to evaluate custom field patterns.
        metadb_handle_ptr playing;
        play_control::get()->get_now_playing(playing);
        if (!extract_track_query(playing, info, q)) {
            MBDB_LOG("dynamic info (track): no artist MBID or usable artist/title, skipping");
            return;
        }
        const unsigned delay = mbdb_prefs::dynamic_delay_seconds();
        if (delay > 0) {
            MBDB_LOG("dynamic info (track): '%s' - '%s', lookup in %u s unless the metadata changes again",
                q.artist.c_str(), q.title.c_str(), delay);
            schedule_delayed(q, delay);
            return;
        }
        cancel_delayed();
        queue_lookup("dynamic info (track)", q, true);
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_stop(play_control::t_stop_reason) override { cancel_delayed(); }
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};

FB2K_SERVICE_FACTORY(musicbrainz_playback_monitor);
}

// ---- public API (lookup_queue.h) ----

size_t mbdb_search_selection() {
    metadb_handle_list items;
    try {
        ui_selection_manager::get()->get_selection(items);
        if (items.get_count() == 0) playlist_manager::get()->activeplaylist_get_selected_items(items);
    } catch (const std::exception& e) {
        MBDB_LOG("search selected: could not read the selection: %s", e.what());
    }
    return mbdb_search_tracks(items);
}

size_t mbdb_search_tracks(metadb_handle_list_cref items) {
    if (items.get_count() == 0) {
        mbdb_activity::add_event(mbdb_activity::lvl_warn, "Search selected: nothing is selected");
        return 0;
    }

    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    // One lookup per distinct artist: later tracks by the same artist would only
    // repeat the work (and cost another rate-limited request).
    std::set<std::string> seen;
    size_t queued = 0, duplicates = 0, skipped = 0;
    std::vector<track_query> jobs;
    for (t_size i = 0; i < items.get_count(); ++i) {
        file_info_impl info;
        track_query q;
        if (!items[i]->get_info(info) || !extract_track_query(items[i], info, q)) { ++skipped; continue; }
        const std::string key = q.artist_mbid.empty() ? "name:" + lower(q.artist) : "mbid:" + q.artist_mbid;
        if (!seen.insert(key).second) { ++duplicates; continue; }
        q.batch = true;
        jobs.push_back(std::move(q));
    }
    // Register the whole batch before queueing so progress never runs ahead of the total.
    queued = jobs.size();
    mbdb_activity::batch_add(queued);
    for (const auto& q : jobs) {
        if (!queue_lookup("selection", q, false)) mbdb_activity::batch_drop(1);
    }

    char msg[200];
    snprintf(msg, sizeof(msg),
        "Search selected: %zu track(s) -> %zu lookup(s) queued, %zu duplicate artist(s), %zu without usable tags",
        static_cast<size_t>(items.get_count()), queued, duplicates, skipped);
    mbdb_activity::add_event(queued ? mbdb_activity::lvl_info : mbdb_activity::lvl_warn, msg);
    return queued;
}

void mbdb_cancel_queued() {
    size_t dropped = 0, batch_dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        dropped = g_jobs.size();
        for (const auto& job : g_jobs) {
            mbdb_activity::lookup_dequeued();
            if (job.batch) ++batch_dropped;
        }
        g_jobs.clear();
    }
    mbdb_activity::batch_drop(batch_dropped);
    char msg[100];
    snprintf(msg, sizeof(msg), "Cancelled %zu queued lookup(s)", dropped);
    mbdb_activity::add_event(mbdb_activity::lvl_warn, msg);
}

void mbdb_lookup_queue_shutdown() {
    {
        std::thread delay;
        {
            std::lock_guard<std::mutex> lock(g_delay_mutex);
            g_delay_stop = true;
            g_delay_pending = false;
            delay = std::move(g_delay_thread);
        }
        g_delay_cv.notify_all();
        if (delay.joinable()) delay.join();
    }
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_stop = true;
        for (size_t i = 0; i < g_jobs.size(); ++i) mbdb_activity::lookup_dequeued();
        g_jobs.clear();
        worker = std::move(g_worker);
    }
    g_queue_cv.notify_all();
    if (worker.joinable()) worker.join(); // a lookup in flight ends once its HTTP call returns or times out
}
