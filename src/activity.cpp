#include "activity.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

namespace mbdb_activity {
namespace {
std::mutex g_mutex;
std::deque<event_t> g_events;
stats_t g_stats;
long long g_http_total_ms = 0;
std::atomic<uint64_t> g_version{1};
std::atomic<uint64_t> g_db_revision{1};
constexpr size_t kMaxEvents = 1000;

ULONGLONG g_batch_start = 0, g_batch_end = 0;
constexpr ULONGLONG kBatchLingerMs = 6000; // keep a finished bar on screen briefly

bool batch_visible_locked() {
    return g_stats.batch_total > 0 &&
        (g_stats.batch_done < g_stats.batch_total || GetTickCount64() - g_batch_end < kBatchLingerMs);
}

void bump() { g_version.fetch_add(1, std::memory_order_relaxed); }

bool has_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const char* n : needles) if (text.find(n) != std::string::npos) return true;
    return false;
}

level_t classify(const std::string& raw) {
    std::string t = raw;
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t.rfind("http: ok", 0) == 0) return lvl_good; // "last error 0" must not look like an error
    if (has_any(t, { "ignoring", "throttled", "rate limit", "rejected", "cooling", "falling back" })) return lvl_warn;
    if (has_any(t, { "failed", "json error", "error:", "invalid", "refusing", "cannot" })) return lvl_error;
    if (has_any(t, { "matched", "cache hit", "saved", "wrote", "ready", "opened", "found artist mbid" })) return lvl_good;
    return lvl_info;
}
} // namespace

void add_event(level_t level, const char* text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    event_t e;
    e.hour = st.wHour; e.minute = st.wMinute; e.second = st.wSecond;
    e.level = level;
    e.text = text ? text : "";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_events.push_back(std::move(e));
        while (g_events.size() > kMaxEvents) g_events.pop_front();
    }
    bump();
}

void add_log_line(const char* text) {
    const std::string s = text ? text : "";
    add_event(classify(s), s.c_str());
}

void clear_events() {
    { std::lock_guard<std::mutex> lock(g_mutex); g_events.clear(); }
    bump();
}

size_t event_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_events.size();
}

std::vector<event_t> events(size_t skip, size_t count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<event_t> out;
    const size_t n = g_events.size();
    for (size_t i = skip; i < n && out.size() < count; ++i) out.push_back(g_events[n - 1 - i]);
    return out;
}

stats_t stats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    stats_t s = g_stats;
    s.batch_visible = batch_visible_locked();
    if (s.batch_total > 0) {
        const ULONGLONG end = s.batch_done < s.batch_total ? GetTickCount64() : g_batch_end;
        s.batch_elapsed_ms = static_cast<long long>(end - g_batch_start);
    }
    s.avg_http_ms = s.http_requests ? static_cast<double>(g_http_total_ms) / static_cast<double>(s.http_requests) : 0.0;
    return s;
}

bool working() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_stats.task.empty();
}

void set_task(const char* text) { { std::lock_guard<std::mutex> lock(g_mutex); g_stats.task = text ? text : ""; } bump(); }
void clear_task() { { std::lock_guard<std::mutex> lock(g_mutex); g_stats.task.clear(); } bump(); }

bool searching() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stats.searching;
}

uint64_t version() { return g_version.load(std::memory_order_relaxed); }
uint64_t db_revision() { return g_db_revision.load(std::memory_order_relaxed); }

void batch_add(size_t count) {
    if (count == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool finished = g_stats.batch_total == 0 || g_stats.batch_done >= g_stats.batch_total;
        if (finished) { // a new batch replaces the previous one; otherwise it extends the running one
            g_stats.batch_total = g_stats.batch_done = g_stats.batch_failed = 0;
            g_stats.batch_cancelled = false;
            g_batch_start = GetTickCount64();
            g_batch_end = 0;
        }
        g_stats.batch_total += count;
    }
    bump();
}

void batch_item_done(bool ok) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_stats.batch_done;
        if (!ok) ++g_stats.batch_failed;
        if (g_stats.batch_done >= g_stats.batch_total) g_batch_end = GetTickCount64();
    }
    bump();
}

void batch_drop(size_t count) {
    if (count == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stats.batch_total = g_stats.batch_total > count ? g_stats.batch_total - count : 0;
        if (g_stats.batch_total < g_stats.batch_done) g_stats.batch_total = g_stats.batch_done;
        g_stats.batch_cancelled = true;
        if (g_stats.batch_total > 0 && g_stats.batch_done >= g_stats.batch_total) g_batch_end = GetTickCount64();
    }
    bump();
}

bool batch_visible() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return batch_visible_locked();
}

void lookup_queued() { { std::lock_guard<std::mutex> lock(g_mutex); ++g_stats.queued; } bump(); }
void lookup_dequeued() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stats.queued > 0) --g_stats.queued;
    }
    bump();
}

void lookup_begin(const char* what) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_stats.lookups_started;
        g_stats.searching = true;
        g_stats.current = what ? what : "";
        g_stats.step = "starting";
    }
    bump();
}

void set_step(const char* step) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_stats.searching) return;
        g_stats.step = step ? step : "";
    }
    bump();
}

void lookup_end(bool ok, long long ms) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (ok) ++g_stats.lookups_ok; else ++g_stats.lookups_failed;
        g_stats.last_result = g_stats.current + (ok ? " - ok (" : " - failed (") + std::to_string(ms) + " ms)";
        g_stats.searching = false;
        g_stats.current.clear();
        g_stats.step.clear();
    }
    bump();
}

void cache_hit() { { std::lock_guard<std::mutex> lock(g_mutex); ++g_stats.cache_hits; } bump(); }
void cache_miss() { { std::lock_guard<std::mutex> lock(g_mutex); ++g_stats.cache_misses; } bump(); }

void http_done(unsigned status, bool ok, long long ms) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_stats.http_requests;
        if (!ok) ++g_stats.http_failures;
        if (status == 503 || status == 429) ++g_stats.http_throttled;
        g_stats.last_http_status = status;
        g_http_total_ms += ms;
    }
    bump();
}

void set_db_artists(long long count) { g_db_revision.fetch_add(1, std::memory_order_relaxed); { std::lock_guard<std::mutex> lock(g_mutex); g_stats.db_artists = count; } bump(); }
void set_db_path(const char* path) { { std::lock_guard<std::mutex> lock(g_mutex); g_stats.db_path = path ? path : ""; } bump(); }

} // namespace mbdb_activity
