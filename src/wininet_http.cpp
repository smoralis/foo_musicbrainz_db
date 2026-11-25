#include "wininet_http.h"
#include "debug_log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

namespace {
    constexpr size_t k_max_response = 10u * 1024u * 1024u;
}

bool wininet_http_get(
    const std::wstring& url,
    std::string& body,
    std::string& error,
    unsigned long timeout_ms)
{
    body.clear();

    DWORD timeout = timeout_ms ? static_cast<DWORD>(timeout_ms) : 20000;

    HINTERNET net = InternetOpenW(
        L"foo_musicbrainz_db/ListenBrainz",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0);

    if (!net) {
        error = "InternetOpen failed";
        return false;
    }

    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(net, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    const DWORD flags =
        INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_NO_COOKIES |
        INTERNET_FLAG_NO_UI |
        INTERNET_FLAG_SECURE;

    HINTERNET req = InternetOpenUrlW(
        net,
        url.c_str(),
        L"Accept: application/json\r\n",
        static_cast<DWORD>(-1),
        flags,
        0);

    bool ok = false;
    DWORD status = 0;

    if (!req) {
        error = "ListenBrainz request failed";
    }
    else {
        DWORD size = sizeof(status);

        if (!HttpQueryInfoW(
            req,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &status,
            &size,
            nullptr)) {
            error = "ListenBrainz request failed";
        }
        else if (status != 200) {
            error = "ListenBrainz returned HTTP " + std::to_string(status);
        }
        else {
            ok = true;

            for (;;) {
                char chunk[16384];
                DWORD received = 0;

                if (!InternetReadFile(req, chunk, sizeof(chunk), &received)) {
                    error = "ListenBrainz read failed";
                    ok = false;
                    break;
                }

                if (!received)
                    break;

                if (body.size() + received > k_max_response) {
                    error = "ListenBrainz response too large";
                    ok = false;
                    break;
                }

                body.append(chunk, received);
            }

            if (!ok)
                body.clear();
        }
    }

    if (req)
        InternetCloseHandle(req);

    InternetCloseHandle(net);

    MBDB_LOG(
        "ListenBrainz wininet: %s (status %lu, %lu bytes)",
        ok ? "ok" : "FAILED",
        status,
        static_cast<unsigned long>(body.size()));

    return ok;
}
