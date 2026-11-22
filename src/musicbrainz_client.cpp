
#include "musicbrainz_client.h"
#include "debug_log.h"
#include "activity.h"
#include "preferences.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace {

    // -----------------------------------------------------------------------------
    // JSON helpers
    // -----------------------------------------------------------------------------

    std::string jstr(const json& j, const char* key)
    {
        if (!j.is_object())
            return {};

        auto it = j.find(key);

        return (it != j.end() && it->is_string())
            ? it->get<std::string>()
            : std::string();
    }

    int jint(const json& j, const char* key)
    {
        if (!j.is_object())
            return 0;

        auto it = j.find(key);

        return (it != j.end() && it->is_number_integer())
            ? it->get<int>()
            : 0;
    }

    // Quotes a value as a Lucene phrase.
    std::string lucene_phrase(const std::string& s)
    {
        std::string out = "\"";

        for (char c : s) {
            if (c == '"' || c == '\\')
                out += '\\';

            out += c;
        }

        out += '"';

        return out;
    }

    // -----------------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------------

    constexpr size_t kMaxResponse = 10u * 1024u * 1024u;

    constexpr int kMaxRetries = 3;
    constexpr int kDefaultBackoffSec = 5;
    constexpr int kMaxBackoffSec = 60;

    // MusicBrainz recommends no more than one request per second.
    // Use a small safety margin.
    constexpr long long kMinimumIntervalMs = 1100;

    using mb_clock = std::chrono::steady_clock;

    std::chrono::milliseconds min_interval()
    {
        const auto configured = mbdb_prefs::rate_limit_seconds();

        const long long configured_ms =
            static_cast<long long>(configured) * 1000LL;

        return std::chrono::milliseconds(
            (std::max)(kMinimumIntervalMs, configured_ms));
    }

    std::atomic<bool> g_abort{ false };

    // One global lock serializes the entire request lifecycle.
    // This includes waiting, HTTP transfers and retries.
    //
    // This is intentional: concurrent requests to MusicBrainz must
    // not bypass the shared rate limiter or server cooldown.
    std::mutex g_request_mutex;

    mb_clock::time_point g_next_allowed{};

    // -----------------------------------------------------------------------------
    // Random jitter
    // -----------------------------------------------------------------------------

    int retry_jitter(int base_seconds)
    {
        if (base_seconds <= 0)
            return 0;

        // Add 0-20% jitter to locally generated backoff.
        const int max_jitter = (std::max)(1, base_seconds / 5);

        thread_local std::mt19937 generator{ [] {
            std::random_device rd;

            return std::mt19937(
                (static_cast<std::mt19937::result_type>(rd()) ^
                 static_cast<std::mt19937::result_type>(
                     mb_clock::now().time_since_epoch().count())));
        }() };

        std::uniform_int_distribution<int> distribution(
            0, max_jitter);

        return base_seconds + distribution(generator);
    }

    // -----------------------------------------------------------------------------
    // Request scheduler
    // -----------------------------------------------------------------------------

    bool wait_for_request_slot()
    {
        while (!g_abort.load()) {

            const auto now = mb_clock::now();

            if (now >= g_next_allowed)
                return true;

            const auto remaining = g_next_allowed - now;

            std::this_thread::sleep_for(
                (std::min<mb_clock::duration>)(
                    remaining,
                    std::chrono::milliseconds(100)));
        }

        return false;
    }

    // Schedule the next request relative to the END of the previous request.
    // The caller must hold g_request_mutex.
    void schedule_next_request(int cooldown_seconds = 0)
    {
        auto delay = min_interval();

        if (cooldown_seconds > 0) {
            const auto cooldown =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::seconds(cooldown_seconds));

            delay = (std::max)(delay, cooldown);
        }

        g_next_allowed = mb_clock::now() + delay;
    }

    // -----------------------------------------------------------------------------
    // UTF-8 / URL helpers
    // -----------------------------------------------------------------------------

    std::wstring widen(const std::string& s)
    {
        if (s.empty())
            return {};

        const int n = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            s.data(),
            static_cast<int>(s.size()),
            nullptr,
            0);

        if (n <= 0)
            return {};

        std::wstring out(n, L'\0');

        if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            s.data(),
            static_cast<int>(s.size()),
            out.data(),
            n) != n) {
            return {};
        }

        return out;
    }

    std::string encode(const std::string& s)
    {
        static const char hex[] = "0123456789ABCDEF";

        std::string out;

        for (unsigned char c : s) {

            if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {

                out += static_cast<char>(c);

            }
            else {

                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }

        return out;
    }

    // -----------------------------------------------------------------------------
    // Retry-After parsing
    // -----------------------------------------------------------------------------

    // Supports both:
    // Retry-After: 10
    // Retry-After: Sat, 21 Nov 2026 15:30:00 GMT
    //
    // Returns seconds. Zero means that the header was unavailable or invalid.
    int parse_retry_after(const wchar_t* value)
    {
        if (!value || !*value)
            return 0;

        // Numeric delay-seconds format.
        if (value[0] >= L'0' && value[0] <= L'9') {

            wchar_t* end = nullptr;

            const long long seconds = wcstoll(value, &end, 10);

            if (end == value || seconds <= 0)
                return 0;

            return seconds > INT_MAX
                ? INT_MAX
                : static_cast<int>(seconds);
        }

        // HTTP-date format.
        SYSTEMTIME st{};

        if (!WinHttpTimeToSystemTime(value, &st))
            return 0;

        std::tm tm{};

        tm.tm_year = static_cast<int>(st.wYear) - 1900;
        tm.tm_mon = static_cast<int>(st.wMonth) - 1;
        tm.tm_mday = static_cast<int>(st.wDay);
        tm.tm_hour = static_cast<int>(st.wHour);
        tm.tm_min = static_cast<int>(st.wMinute);
        tm.tm_sec = static_cast<int>(st.wSecond);

        const __time64_t target = _mkgmtime64(&tm);
        const __time64_t now = _time64(nullptr);

        if (target <= now)
            return 0;

        const __time64_t seconds = target - now;

        return seconds > INT_MAX
            ? INT_MAX
            : static_cast<int>(seconds);
    }

    // -----------------------------------------------------------------------------
    // Retryable HTTP status codes
    // -----------------------------------------------------------------------------

    bool is_retryable_status(DWORD status)
    {
        return status == 429 ||
            status == 502 ||
            status == 503 ||
            status == 504;
    }

    // -----------------------------------------------------------------------------
    // Persistent WinHTTP session
    // -----------------------------------------------------------------------------

    class HttpSession
    {
    public:

        HttpSession()
        {
            handle_ = WinHttpOpen(
                L"foo_musicbrainz_db/0.1.3 "
                L"(contact: https://github.com/smoralis/foo_musicbrainz_db)",
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);

            if (!handle_) {
                MBDB_LOG(
                    "http: WinHttpOpen failed (err %lu)",
                    GetLastError());

                return;
            }

            if (!WinHttpSetTimeouts(
                handle_,
                5000,   // DNS resolution
                5000,   // connection
                10000,  // send
                20000)) // receive
            {
                MBDB_LOG(
                    "http: WinHttpSetTimeouts failed (err %lu)",
                    GetLastError());
            }
        }

        ~HttpSession()
        {
            if (handle_)
                WinHttpCloseHandle(handle_);
        }

        HttpSession(const HttpSession&) = delete;
        HttpSession& operator=(const HttpSession&) = delete;

        HINTERNET get() const
        {
            return handle_;
        }

    private:

        HINTERNET handle_ = nullptr;
    };

    HINTERNET get_http_session()
    {
        static HttpSession session;

        return session.get();
    }

    // -----------------------------------------------------------------------------
    // Read Retry-After header
    // -----------------------------------------------------------------------------

    int get_retry_after(HINTERNET request)
    {
        wchar_t value[128] = {};

        DWORD length = sizeof(value);

        if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CUSTOM,
            L"Retry-After",
            value,
            &length,
            WINHTTP_NO_HEADER_INDEX)) {

            return 0;
        }

        return parse_retry_after(value);
    }

} // namespace

// =============================================================================
// HTTP request implementation
// =============================================================================

// One HTTP attempt.
//
// status:
//   HTTP response status, or zero if no HTTP response was received.
//
// retry_after:
//   Retry-After delay in seconds, if supplied by the server.

static bool http_request_impl(
    const std::wstring& path,
    std::string& response,
    DWORD& status,
    int& retry_after)
{
    status = 0;
    retry_after = 0;

    response.clear();

    MBDB_LOG(
        "http GET https://musicbrainz.org%ls",
        path.c_str());

    HINTERNET session = get_http_session();

    if (!session) {
        MBDB_LOG("http: no valid WinHTTP session");
        return false;
    }

    HINTERNET conn = WinHttpConnect(
        session,
        L"musicbrainz.org",
        INTERNET_DEFAULT_HTTPS_PORT,
        0);

    if (!conn) {
        const DWORD error = GetLastError();

        MBDB_LOG(
            "http: WinHttpConnect failed (err %lu)",
            error);

        return false;
    }

    HINTERNET req = WinHttpOpenRequest(
        conn,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!req) {
        const DWORD error = GetLastError();

        MBDB_LOG(
            "http: WinHttpOpenRequest failed (err %lu)",
            error);

        WinHttpCloseHandle(conn);

        return false;
    }

    bool ok = false;
    DWORD error = ERROR_SUCCESS;

    // Request headers.
    if (!WinHttpAddRequestHeaders(
        req,
        L"Accept: application/json\r\n",
        static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD)) {

        error = GetLastError();

    }
    else {

        // Send request.
        if (!WinHttpSendRequest(
            req,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {

            error = GetLastError();

        }
        else if (!WinHttpReceiveResponse(req, nullptr)) {

            error = GetLastError();

        }
        else {

            DWORD size = sizeof(status);

            if (!WinHttpQueryHeaders(
                req,
                WINHTTP_QUERY_STATUS_CODE |
                WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &size,
                WINHTTP_NO_HEADER_INDEX)) {

                error = GetLastError();

            }
            else {

                // An HTTP response was received.
                error = ERROR_SUCCESS;

                if (is_retryable_status(status))
                    retry_after = get_retry_after(req);

                if (status == 200) {

                    ok = true;

                    // Read response body.
                    for (;;) {

                        DWORD available = 0;

                        if (!WinHttpQueryDataAvailable(
                            req,
                            &available)) {

                            error = GetLastError();
                            ok = false;
                            break;
                        }

                        if (available == 0)
                            break;

                        // Protect against excessively large responses.
                        if (available > kMaxResponse -
                            (std::min)(response.size(), kMaxResponse)) {

                            MBDB_LOG(
                                "http: response exceeded %zu bytes",
                                kMaxResponse);

                            error = ERROR_FILE_TOO_LARGE;
                            ok = false;
                            break;
                        }

                        std::string buffer(
                            static_cast<size_t>(available),
                            '\0');

                        DWORD received = 0;

                        if (!WinHttpReadData(
                            req,
                            buffer.data(),
                            available,
                            &received)) {

                            error = GetLastError();
                            ok = false;
                            break;
                        }

                        if (received == 0)
                            break;

                        response.append(
                            buffer.data(),
                            received);
                    }

                    if (!ok)
                        response.clear();
                }
            }
        }
    }

    MBDB_LOG(
        "http: %s (status %lu, %lu bytes, "
        "retry-after %d, error %lu)",
        ok ? "ok" : "FAILED",
        status,
        static_cast<unsigned long>(response.size()),
        retry_after,
        error);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);

    return ok;
}

// =============================================================================
// HTTP request wrapper with activity tracking
// =============================================================================

static bool http_request(
    const std::wstring& path,
    std::string& response,
    DWORD& status,
    int& retry_after)
{
    const auto start = mb_clock::now();

    const bool ok = http_request_impl(
        path,
        response,
        status,
        retry_after);

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            mb_clock::now() - start).count();

    mbdb_activity::http_done(
        status,
        ok,
        elapsed);

    return ok;
}

// =============================================================================
// Public HTTP GET
// =============================================================================

bool musicbrainz_client::http_get(
    const std::wstring& path,
    std::string& response)
{
    if (path.empty()) {
        MBDB_LOG("http: rejected empty request path");
        return false;
    }

    // Serialize all callers, including retries and cooldowns.
    std::unique_lock<std::mutex> lock(g_request_mutex);

    for (int attempt = 0;
        attempt <= kMaxRetries;
        ++attempt) {

        if (!wait_for_request_slot()) {

            MBDB_LOG(
                "http: aborted while waiting for request slot");

            return false;
        }

        DWORD status = 0;
        int retry_after = 0;

        const bool ok = http_request(
            path,
            response,
            status,
            retry_after);

        if (ok) {

            // Wait at least the configured interval after the
            // previous request finishes.
            schedule_next_request();

            return true;
        }

        // Do not retry permanent HTTP errors or transport failures
        // without an HTTP status.
        if (!is_retryable_status(status)) {

            schedule_next_request();

            MBDB_LOG(
                "http: request failed permanently (status %lu)",
                status);

            return false;
        }

        int wait_seconds = 0;

        if (retry_after > 0) {

            // Respect the server-provided delay exactly.
            wait_seconds = retry_after;

        }
        else {

            // Local exponential backoff:
            // 5, 10, 20, 40 seconds, plus jitter.
            const int shift = (std::min)(attempt, 10);

            const int base_seconds =
                (std::min)(
                    kDefaultBackoffSec * (1 << shift),
                    kMaxBackoffSec);

            wait_seconds = retry_jitter(base_seconds);
        }

        // Apply shared cooldown for every caller.
        schedule_next_request(wait_seconds);

        MBDB_LOG(
            "http: HTTP %lu throttled/temporary failure; "
            "cooldown %d seconds "
            "(attempt %d of %d, retry-after %d)",
            status,
            wait_seconds,
            attempt + 1,
            kMaxRetries + 1,
            retry_after);

        if (attempt == kMaxRetries) {

            MBDB_LOG(
                "http: retry limit reached for %ls",
                path.c_str());

            // Cooldown remains active for subsequent callers.
            return false;
        }

        // The next iteration waits for the shared cooldown.
    }

    return false;
}

// =============================================================================
// Shutdown
// =============================================================================

void musicbrainz_client_abort()
{
    g_abort.store(true);

    MBDB_LOG("http: MusicBrainz client shutdown requested");
}

// =============================================================================
// Artist matching
// =============================================================================

namespace {

    // Compare an artist's name or aliases against the wanted name.
    // wanted is already lower-cased.

    bool artist_name_matches(
        const json& artist,
        const std::string& wanted)
    {
        auto lower = [](std::string s) {

            for (auto& c : s) {
                c = static_cast<char>(
                    std::tolower(
                        static_cast<unsigned char>(c)));
            }

            return s;
            };

        if (!artist.is_object())
            return false;

        if (lower(jstr(artist, "name")) == wanted)
            return true;

        auto it = artist.find("aliases");

        if (it != artist.end() && it->is_array()) {

            for (const auto& alias : *it) {

                if (alias.is_object() &&
                    lower(jstr(alias, "name")) == wanted) {

                    return true;
                }
            }
        }

        return false;
    }

} // namespace

// =============================================================================
// Search artist by recording title and artist name
// =============================================================================

bool musicbrainz_client::search_artist_title(
    const std::string& artist,
    const std::string& title,
    std::string& artist_mbid)
{
    artist_mbid.clear();

    if (artist.empty() || title.empty())
        return false;

    auto lower = [](std::string s) {

        for (auto& c : s) {
            c = static_cast<char>(
                std::tolower(
                    static_cast<unsigned char>(c)));
        }

        return s;
        };

    const std::string wanted = lower(artist);

    const std::string query =
        "recording:" + lucene_phrase(title) +
        " AND artist:" + lucene_phrase(artist);

    std::string response;

    const std::string url =
        "/ws/2/recording/?query=" +
        encode(query) +
        "&fmt=json&limit=5";

    if (!http_get(widen(url), response))
        return false;

    try {

        auto data = json::parse(response);

        if (!data.contains("recordings") ||
            !data["recordings"].is_array()) {

            return false;
        }

        for (const auto& item : data["recordings"]) {

            if (!item.is_object())
                continue;

            if (!item.contains("artist-credit") ||
                !item["artist-credit"].is_array()) {

                continue;
            }

            for (const auto& credit :
                item["artist-credit"]) {

                if (!credit.is_object() ||
                    !credit.contains("artist") ||
                    !credit["artist"].is_object()) {

                    continue;
                }

                const auto& a = credit["artist"];

                const std::string id = jstr(a, "id");

                if (id.empty())
                    continue;

                if (!artist_name_matches(a, wanted) &&
                    lower(jstr(credit, "name")) != wanted) {

                    continue;
                }

                artist_mbid = id;

                MBDB_LOG(
                    "client: title search matched artist %s "
                    "via recording '%s'",
                    artist_mbid.c_str(),
                    jstr(item, "title").c_str());

                return true;
            }
        }

    }
    catch (const std::exception& e) {

        MBDB_LOG(
            "client: recording search JSON error: %s",
            e.what());

        return false;
    }

    MBDB_LOG(
        "client: no recording credits an artist named '%s'",
        artist.c_str());

    return false;
}

// =============================================================================
// Search artist by name
// =============================================================================

bool musicbrainz_client::search_artist_name(
    const std::string& artist,
    std::string& artist_mbid)
{
    artist_mbid.clear();

    if (artist.empty())
        return false;

    auto lower = [](std::string s) {

        for (auto& c : s) {
            c = static_cast<char>(
                std::tolower(
                    static_cast<unsigned char>(c)));
        }

        return s;
        };

    std::string response;

    const std::string query =
        "artist:" + lucene_phrase(artist);

    const std::string url =
        "/ws/2/artist/?query=" +
        encode(query) +
        "&fmt=json&limit=10";

    if (!http_get(widen(url), response))
        return false;

    try {

        auto data = json::parse(response);

        if (!data.contains("artists") ||
            !data["artists"].is_array() ||
            data["artists"].empty()) {

            MBDB_LOG(
                "client: artist name search returned nothing "
                "for '%s'",
                artist.c_str());

            return false;
        }

        const std::string wanted = lower(artist);

        for (const auto& item : data["artists"]) {

            if (!item.is_object())
                continue;

            const std::string id = jstr(item, "id");
            const std::string name = jstr(item, "name");

            if (id.empty() ||
                !artist_name_matches(item, wanted)) {

                continue;
            }

            artist_mbid = id;

            MBDB_LOG(
                "client: artist name search matched '%s' "
                "=> %s (score %d%s)",
                name.c_str(),
                id.c_str(),
                jint(item, "score"),
                lower(name) == wanted
                ? ""
                : ", via alias");

            return true;
        }

        MBDB_LOG(
            "client: artist name search found no exact result "
            "for '%s' (top hit: '%s')",
            artist.c_str(),
            jstr(data["artists"][0], "name").c_str());

        return false;

    }
    catch (const std::exception& e) {

        MBDB_LOG(
            "client: artist search JSON error: %s",
            e.what());

        return false;
    }
}

// =============================================================================
// Get artist details
// =============================================================================

bool musicbrainz_client::get_artist(
    const std::string& mbid,
    artist_record& r)
{
    if (mbid.empty())
        return false;

    std::string response;

    const std::string url =
        "/ws/2/artist/" +
        mbid +
        "?inc=genres+tags+ratings+annotation+aliases+url-rels&fmt=json";

    if (!http_get(widen(url), response))
        return false;

    try {

        auto d = json::parse(response);

        if (!d.is_object()) {
            MBDB_LOG("client: artist response is not a JSON object");
            return false;
        }

        r.mbid = jstr(d, "id");
        r.name = jstr(d, "name");
        r.type = jstr(d, "type");
        r.country = jstr(d, "country");
        r.disambiguation = jstr(d, "disambiguation");

        if (d.contains("area") &&
            d["area"].is_object()) {

            r.area = jstr(d["area"], "name");
        }

        if (d.contains("begin-area") &&
            d["begin-area"].is_object()) {

            r.begin_area =
                jstr(d["begin-area"], "name");
        }

        if (d.contains("end-area") &&
            d["end-area"].is_object()) {

            r.end_area =
                jstr(d["end-area"], "name");
        }

        if (d.contains("life-span") &&
            d["life-span"].is_object()) {

            r.begin_date =
                jstr(d["life-span"], "begin");

            r.end_date =
                jstr(d["life-span"], "end");
        }

        r.raw_json = d.dump(
            2,
            ' ',
            false,
            json::error_handler_t::replace);

        MBDB_LOG(
            "client: artist %s => name '%s', country '%s' "
            "(%zu bytes of json)",
            r.mbid.c_str(),
            r.name.c_str(),
            r.country.c_str(),
            r.raw_json.size());

        return !r.mbid.empty();

    }
    catch (const std::exception& e) {

        MBDB_LOG(
            "client: artist JSON error: %s",
            e.what());

        return false;
    }
}
