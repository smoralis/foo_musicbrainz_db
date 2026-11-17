#pragma once
// Thread-safe activity feed + statistics shared by the worker threads (which
// write) and the DUI/CUI panels (which read). Panels poll version() on a timer,
// so no cross-thread messaging is needed.
#include <cstdint>
#include <string>
#include <vector>

namespace mbdb_activity {

enum level_t { lvl_info, lvl_good, lvl_warn, lvl_error };

struct event_t {
    int hour = 0, minute = 0, second = 0;
    level_t level = lvl_info;
    std::string text; // UTF-8
};

struct stats_t {
    uint64_t lookups_started = 0, lookups_ok = 0, lookups_failed = 0;
    uint64_t queued = 0; // lookups waiting for the worker
    uint64_t cache_hits = 0, cache_misses = 0;
    uint64_t http_requests = 0, http_failures = 0, http_throttled = 0;
    unsigned last_http_status = 0;
    double avg_http_ms = 0;
    long long db_artists = -1; // -1 = unknown
    // "Search selected" batch progress
    uint64_t batch_total = 0, batch_done = 0, batch_failed = 0;
    bool batch_cancelled = false;
    bool batch_visible = false;     // active, or finished within the last few seconds
    long long batch_elapsed_ms = 0;
    bool searching = false;
    std::string current;     // what is being looked up right now
    std::string step;        // which stage the lookup is in
    std::string last_result; // outcome of the previous lookup
    std::string task;        // background panel task in progress (delete / export), empty if none
    std::string db_path;
};

// --- events ---
void add_event(level_t level, const char* utf8_text);
void add_log_line(const char* utf8_text); // classifies the level from the wording
void clear_events();
size_t event_count();
std::vector<event_t> events(size_t skip, size_t count); // newest first

// --- statistics ---
stats_t stats();
bool searching();      // cheap check for the panel's animation
uint64_t version();    // changes whenever anything above changes
bool working();          // a background panel task is running
uint64_t db_revision(); // changes whenever the artists table changes (panel reloads its table)

// Batch progress (the panel's "Search selected" progress bar)
void batch_add(size_t count);       // `count` lookups were queued as part of a batch
void batch_item_done(bool ok);      // one batch lookup finished
void batch_drop(size_t count);      // `count` queued batch lookups were cancelled
bool batch_visible();

void lookup_queued();
void lookup_dequeued();
void lookup_begin(const char* what);
void set_step(const char* step);
void lookup_end(bool ok, long long milliseconds);
void cache_hit();
void cache_miss();
void http_done(unsigned status, bool ok, long long milliseconds);
void set_task(const char* text);
void clear_task();
void set_db_artists(long long count);
void set_db_path(const char* utf8_path);

// Shows `text` in the panel while it is alive.
struct task_scope {
    explicit task_scope(const char* text) { set_task(text); }
    ~task_scope() { clear_task(); }
    task_scope(const task_scope&) = delete;
    task_scope& operator=(const task_scope&) = delete;
};

} // namespace mbdb_activity
