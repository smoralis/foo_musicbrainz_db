#pragma once
// Minimal WinINet GET transport. Kept in its own translation unit because wininet.h and
// winhttp.h declare overlapping types and must not be included together.
#include <string>

// GET an https:// (or http://) URL. Returns true only for HTTP 200 with the body read in full.
// On failure `error` describes the problem and `body` is empty.
bool wininet_http_get(
    const std::wstring& url,
    std::string& body,
    std::string& error,
    unsigned long timeout_ms = 0);
