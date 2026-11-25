
#include "musicbrainz_client.h"
#include "debug_log.h"
#include "activity.h"
#include "preferences.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

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

// The first bytes of a response, printable, for log messages about a body that is not JSON.
static std::string response_prefix(const std::string& body) {
    std::string out;
    const size_t n = (std::min)(body.size(), static_cast<size_t>(80));
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(body[i]);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\x%02X", c);
            out += esc;
        }
    }
    return out;
}

// Some HTTP stacks (seen under Wine) hand over the body with its framing still in it:
// a UTF-8 byte order mark, or chunked transfer coding ("<hex size>\r\n<data>\r\n ... 0\r\n\r\n").
// Both are removed here; a body that is already plain JSON is left alone.
static void normalize_response_body(std::string& body) {
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
        MBDB_LOG("http: removed a UTF-8 byte order mark from the response");
    }

    if (body.empty() || !std::isxdigit(static_cast<unsigned char>(body[0])))
        return; // JSON starts with '{' or '['

    std::string decoded;
    size_t pos = 0;
    for (;;) {
        const size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos || eol == pos || eol - pos > 8 + 64) return; // not chunked
        std::string size_text = body.substr(pos, eol - pos);
        const size_t ext = size_text.find(';');
        if (ext != std::string::npos) size_text.erase(ext);
        if (size_text.empty() || size_text.size() > 8) return;
        for (char c : size_text)
            if (!std::isxdigit(static_cast<unsigned char>(c))) return;
        const size_t size = static_cast<size_t>(std::stoul(size_text, nullptr, 16));
        pos = eol + 2;
        if (size == 0) break; // last chunk
        if (pos + size > body.size()) return;
        decoded.append(body, pos, size);
        pos += size;
        if (body.compare(pos, 2, "\r\n") != 0) return;
        pos += 2;
    }

    MBDB_LOG("http: removed chunked transfer coding from the response (%zu -> %zu bytes)",
        body.size(), decoded.size());
    body = std::move(decoded);
}

// ---------------------------------------------------------------------------------------
// WinINet transport. Under Wine the WinHTTP stack was seen to deliver larger bodies without
// their beginning (the size still matched Content-Length), so such a response is fetched again
// through WinINet, which is a separate implementation; if that delivers a sane body, WinINet
// is used for the rest of the session. Same request, same headers, same result handling.
// ---------------------------------------------------------------------------------------
static std::atomic<bool> g_use_wininet{ false };

static bool wininet_request(
    const std::wstring& path,
    std::string& response,
    DWORD& status,
    int& retry_after)
{
    response.clear();
    status = 0;
    retry_after = 0;

    HINTERNET net = InternetOpenW(
        L"foo_musicbrainz_db/0.1.3 (contact: https://github.com/smoralis/foo_musicbrainz_db)",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0);
    if (!net) {
        MBDB_LOG("wininet: InternetOpen failed (err %lu)", GetLastError());
        return false;
    }

    const DWORD timeout_ms = 20000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, const_cast<DWORD*>(&timeout_ms), sizeof(timeout_ms));
    InternetSetOptionW(net, INTERNET_OPTION_SEND_TIMEOUT, const_cast<DWORD*>(&timeout_ms), sizeof(timeout_ms));
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, const_cast<DWORD*>(&timeout_ms), sizeof(timeout_ms));

    bool ok = false;
    DWORD error = ERROR_SUCCESS;

    HINTERNET conn = InternetConnectW(
        net, L"musicbrainz.org", INTERNET_DEFAULT_HTTPS_PORT,
        nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);

    HINTERNET req = nullptr;
    if (conn) {
        const wchar_t* accept[] = { L"application/json", nullptr };
        req = HttpOpenRequestW(
            conn, L"GET", path.c_str(), nullptr, nullptr, accept,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
            INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI,
            0);
    }

    if (!conn || !req) {
        error = GetLastError();
    }
    else if (!HttpSendRequestW(req, nullptr, 0, nullptr, 0)) {
        error = GetLastError();
    }
    else {
        DWORD code = 0;
        DWORD code_size = sizeof(code);
        if (!HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &code_size, nullptr)) {
            error = GetLastError();
        }
        else {
            status = code;

            if (is_retryable_status(code)) {
                wchar_t value[64] = L"Retry-After";
                DWORD value_size = sizeof(value);
                if (HttpQueryInfoW(req, HTTP_QUERY_CUSTOM, value, &value_size, nullptr))
                    retry_after = parse_retry_after(value);
            }

            if (code == 200) {
                ok = true;
                for (;;) {
                    char chunk[16384];
                    DWORD received = 0;
                    if (!InternetReadFile(req, chunk, static_cast<DWORD>(sizeof(chunk)), &received)) {
                        error = GetLastError();
                        ok = false;
                        break;
                    }
                    if (received == 0)
                        break;
                    if (response.size() + received > kMaxResponse) {
                        error = ERROR_FILE_TOO_LARGE;
                        ok = false;
                        break;
                    }
                    response.append(chunk, received);
                }
                if (!ok)
                    response.clear();
            }
        }
    }

    if (req) InternetCloseHandle(req);
    if (conn) InternetCloseHandle(conn);
    InternetCloseHandle(net);

    MBDB_LOG(
        "wininet: %s (status %lu, %lu bytes, retry-after %d, error %lu)",
        ok ? "ok" : "FAILED",
        status,
        static_cast<unsigned long>(response.size()),
        retry_after,
        error);

    return ok;
}

// The last bytes of a body, printable (see response_prefix).
static std::string response_suffix(const std::string& body) {
    const size_t n = (std::min)(body.size(), static_cast<size_t>(80));
    return response_prefix(body.substr(body.size() - n));
}

// Under Wine the body has been seen to arrive with the right size but starting in the middle
// of the JSON, as if the data had been rotated. If moving the front part to the back turns it
// into valid JSON, do that. Returns the offset the body was rotated by, or 0 if no rotation
// gives valid JSON (the body is then left untouched).
static size_t repair_rotated_json(std::string& body) {
    for (size_t i = 1; i + 1 < body.size(); ++i) {
        if (body[i] != '{' || body[i + 1] != '"') continue;
        std::string candidate = body.substr(i);
        candidate.append(body, 0, i);
        if (json::accept(candidate)) {
            body = std::move(candidate);
            return i;
        }
    }
    return 0;
}

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

    if (g_use_wininet.load())
        return wininet_request(path, response, status, retry_after);

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

                    // Read response body. WinHttpReadData is called directly with a fixed
                    // buffer, without WinHttpQueryDataAvailable first: under Wine the
                    // query-then-read pattern delivered bodies with their beginning missing.
                    for (;;) {

                        char chunk[16384];
                        DWORD received = 0;

                        if (!WinHttpReadData(
                            req,
                            chunk,
                            static_cast<DWORD>(sizeof(chunk)),
                            &received)) {

                            error = GetLastError();
                            ok = false;
                            break;
                        }

                        if (received == 0)
                            break;

                        // Protect against excessively large responses.
                        if (received > kMaxResponse -
                            (std::min)(response.size(), kMaxResponse)) {

                            MBDB_LOG(
                                "http: response exceeded %zu bytes",
                                kMaxResponse);

                            error = ERROR_FILE_TOO_LARGE;
                            ok = false;
                            break;
                        }

                        response.append(chunk, received);
                    }

                    if (!ok)
                        response.clear();
                    else {
                        normalize_response_body(response);

                        // Cross-check the body against the headers, and make sure it looks
                        // like JSON, so a damaged body is reported here with its headers
                        // instead of as an unexplained parse error later.
                        DWORD declared = 0;
                        DWORD declared_size = sizeof(declared);
                        const bool has_length = WinHttpQueryHeaders(
                            req,
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &declared,
                            &declared_size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE;

                        size_t first = 0;
                        while (first < response.size() &&
                            std::isspace(static_cast<unsigned char>(response[first])))
                            ++first;
                        bool json_start = first < response.size() &&
                            (response[first] == '{' || response[first] == '[');

                        const bool length_mismatch =
                            has_length && declared != response.size();

                        if (!length_mismatch && !response.empty() && !json_start) {
                            const std::string before = response_prefix(response);
                            const size_t offset = repair_rotated_json(response);
                            if (offset > 0) {
                                MBDB_LOG(
                                    "http: response body (%lu bytes, Content-Length matched) "
                                    "started mid-JSON (\"%s\"); repaired by rotating it at offset %zu",
                                    static_cast<unsigned long>(response.size()),
                                    before.c_str(),
                                    offset);
                                json_start = true;
                            }
                        }

                        if (length_mismatch || (!response.empty() && !json_start)) {

                            DWORD hdr_bytes = 0;
                            WinHttpQueryHeaders(
                                req,
                                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                WINHTTP_NO_OUTPUT_BUFFER,
                                &hdr_bytes,
                                WINHTTP_NO_HEADER_INDEX);

                            std::wstring headers(hdr_bytes / sizeof(wchar_t) + 1, L'\0');
                            if (hdr_bytes > 0 && WinHttpQueryHeaders(
                                req,
                                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                headers.data(),
                                &hdr_bytes,
                                WINHTTP_NO_HEADER_INDEX)) {
                                headers.resize(hdr_bytes / sizeof(wchar_t));
                            }
                            else {
                                headers.clear();
                            }
                            for (auto& c : headers)
                                if (c == L'\r' || c == L'\n') c = L'|';
                            if (headers.size() > 700) headers.resize(700);

                            MBDB_LOG(
                                "http: damaged response body: %lu bytes received, "
                                "Content-Length %s%lu, starts with \"%s\", ends with \"%s\"; headers: %ls",
                                static_cast<unsigned long>(response.size()),
                                has_length ? "" : "(none) ",
                                static_cast<unsigned long>(has_length ? declared : 0),
                                response_prefix(response).c_str(),
                                response_suffix(response).c_str(),
                                headers.c_str());

                            // Fetch it again through WinINet (after a short pause, to stay
                            // within the MusicBrainz rate limit).
                            error = ERROR_INVALID_DATA;
                            ok = false;
                            response.clear();

                            Sleep(1000);
                            std::string alt;
                            DWORD alt_status = 0;
                            int alt_retry = 0;
                            if (wininet_request(path, alt, alt_status, alt_retry) && alt_status == 200) {
                                size_t alt_first = 0;
                                while (alt_first < alt.size() &&
                                    std::isspace(static_cast<unsigned char>(alt[alt_first])))
                                    ++alt_first;
                                if (alt_first < alt.size() && (alt[alt_first] == '{' || alt[alt_first] == '[')) {
                                    response = std::move(alt);
                                    status = 200;
                                    error = ERROR_SUCCESS;
                                    ok = true;
                                    g_use_wininet.store(true);
                                    MBDB_LOG(
                                        "http: WinINet delivered a sane body (%lu bytes); "
                                        "WinINet is used for the remaining requests",
                                        static_cast<unsigned long>(response.size()));
                                }
                                else {
                                    MBDB_LOG("http: WinINet also delivered a damaged body (starts: \"%s\")",
                                        response_prefix(alt).c_str());
                                }
                            }
                        }
                    }
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
            "client: recording search JSON error: %s (response starts: \"%s\")",
            e.what(),
            response_prefix(response).c_str());

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
            "client: artist search JSON error: %s (response starts: \"%s\")",
            e.what(),
            response_prefix(response).c_str());

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
            "client: artist JSON error: %s (response starts: \"%s\")",
            e.what(),
            response_prefix(response).c_str());

        return false;
    }
}
