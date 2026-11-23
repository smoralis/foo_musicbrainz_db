
#include "stdafx.h"
#include "../include/listenbrainz_similar_artists.h"

#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace {

    struct cache_entry {
        std::chrono::steady_clock::time_point stored;
        std::vector<listenbrainz_similar_artist> artists;
    };

    std::mutex g_cache_mutex;
    std::unordered_map<std::string, cache_entry> g_cache;

    constexpr auto k_cache_ttl = std::chrono::hours(24 * 7);

    bool valid_mbid(const std::string& s) {
        if (s.size() != 36)
            return false;

        for (size_t i = 0; i < s.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (s[i] != '-')
                    return false;
            }
            else if (!(
                (s[i] >= '0' && s[i] <= '9') ||
                (s[i] >= 'a' && s[i] <= 'f') ||
                (s[i] >= 'A' && s[i] <= 'F'))) {
                return false;
            }
        }

        return true;
    }

    std::string url_encode(const std::string& value) {
        static const char hex[] = "0123456789ABCDEF";

        std::string out;

        for (unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            }
            else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            }
        }

        return out;
    }

    bool http_get(
        const std::wstring& url,
        std::string& body,
        std::string& error,
        DWORD timeout_ms = 0)
    {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);

        parts.dwSchemeLength =
            parts.dwHostNameLength =
            parts.dwUrlPathLength =
            parts.dwExtraInfoLength =
            static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
            error = "Invalid ListenBrainz URL";
            return false;
        }

        const std::wstring host(
            parts.lpszHostName,
            parts.dwHostNameLength);

        std::wstring path(
            parts.lpszUrlPath,
            parts.dwUrlPathLength);

        if (parts.dwExtraInfoLength) {
            path.append(
                parts.lpszExtraInfo,
                parts.dwExtraInfoLength);
        }

        HINTERNET session = WinHttpOpen(
            L"foo_musicbrainz_db/ListenBrainz",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!session) {
            error = "WinHttpOpen failed";
            return false;
        }

        if (timeout_ms) {
            WinHttpSetTimeouts(
                session,
                timeout_ms,
                timeout_ms,
                timeout_ms,
                timeout_ms);
        }
        else {
            WinHttpSetTimeouts(
                session,
                10000,
                10000,
                15000,
                30000);
        }

        HINTERNET connection =
            WinHttpConnect(session, host.c_str(), parts.nPort, 0);

        HINTERNET request = connection
            ? WinHttpOpenRequest(
                connection,
                L"GET",
                path.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                parts.nScheme == INTERNET_SCHEME_HTTPS
                ? WINHTTP_FLAG_SECURE
                : 0)
            : nullptr;

        bool ok = false;

        if (request &&
            WinHttpAddRequestHeaders(
                request,
                L"Accept: application/json\r\n",
                -1L,
                WINHTTP_ADDREQ_FLAG_ADD) &&
            WinHttpSendRequest(
                request,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0) &&
            WinHttpReceiveResponse(request, nullptr)) {

            DWORD status = 0;
            DWORD size = sizeof(status);

            if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE |
                WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &size,
                WINHTTP_NO_HEADER_INDEX) &&
                status == 200) {

                for (;;) {
                    DWORD available = 0;

                    if (!WinHttpQueryDataAvailable(
                        request,
                        &available)) {
                        break;
                    }

                    if (!available) {
                        ok = true;
                        break;
                    }

                    const size_t old = body.size();
                    body.resize(old + available);

                    DWORD read = 0;

                    if (!WinHttpReadData(
                        request,
                        body.data() + old,
                        available,
                        &read)) {
                        body.resize(old);
                        break;
                    }

                    body.resize(old + read);
                }
            }
            else {
                error =
                    "ListenBrainz returned HTTP " +
                    std::to_string(status);
            }
        }
        else {
            error = "ListenBrainz request failed";
        }

        if (request)
            WinHttpCloseHandle(request);

        if (connection)
            WinHttpCloseHandle(connection);

        WinHttpCloseHandle(session);

        return ok;
    }

    std::filesystem::path persistent_cache_path(
        const std::string& mbid,
        const std::string& algorithm)
    {
        pfc::string8 native_profile;

        foobar2000_io::extract_native_path(
            core_api::get_profile_path(),
            native_profile);

        const auto profile =
            std::filesystem::u8path(native_profile.c_str());

        // Hash the algorithm so custom identifiers cannot
        // create unsafe path names.
        const auto hash =
            static_cast<unsigned long long>(
                std::hash<std::string>{}(algorithm));

        char filename[96] = {};

        sprintf_s(
            filename,
            sizeof(filename),
            "%s_%016llx.json",
            mbid.c_str(),
            hash);

        return profile
            / "foo_musicbrainz_db"
            / "repository"
            / "listenbrainz"
            / filename;
    }

    bool load_disk_cache(
        const std::filesystem::path& path,
        std::vector<listenbrainz_similar_artist>& out)
    {
        try {
            std::ifstream file(path, std::ios::binary);

            if (!file)
                return false;

            const auto root = json::parse(file);

            if (!root.is_object() ||
                !root.contains("saved_at") ||
                !root.contains("artists") ||
                !root["saved_at"].is_number_integer() ||
                !root["artists"].is_array()) {
                return false;
            }

            const auto saved =
                root["saved_at"].get<int64_t>();

            const auto now =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now()
                    .time_since_epoch()).count();

            if (saved > now ||
                now - saved > 7 * 24 * 60 * 60) {
                return false;
            }

            std::vector<listenbrainz_similar_artist> loaded;

            for (const auto& item : root["artists"]) {
                if (!item.is_object())
                    continue;

                listenbrainz_similar_artist a;

                a.mbid = item.value("mbid", std::string());

                if (!valid_mbid(a.mbid))
                    continue;

                a.name = item.value("name", std::string());
                a.comment = item.value("comment", std::string());
                a.type = item.value("type", std::string());
                a.gender = item.value("gender", std::string());
                a.reference_mbid =
                    item.value("reference_mbid", std::string());

                a.score = item.value("score", 0.0);

                loaded.push_back(std::move(a));
            }

            out.swap(loaded);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void save_disk_cache(
        const std::filesystem::path& path,
        const std::vector<listenbrainz_similar_artist>& artists)
    {
        try {
            std::filesystem::create_directories(
                path.parent_path());

            json root;

            root["saved_at"] =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now()
                    .time_since_epoch()).count();

            root["artists"] = json::array();

            for (const auto& a : artists) {
                root["artists"].push_back({
                    {"mbid", a.mbid},
                    {"name", a.name},
                    {"comment", a.comment},
                    {"type", a.type},
                    {"gender", a.gender},
                    {"reference_mbid", a.reference_mbid},
                    {"score", a.score}
                    });
            }

            auto temp = path;
            temp += L".tmp";

            {
                std::ofstream file(
                    temp,
                    std::ios::binary | std::ios::trunc);

                if (!file)
                    return;

                file << root.dump(2);
            }

            std::error_code ec;

            std::filesystem::rename(temp, path, ec);

            if (ec) {
                std::filesystem::remove(path, ec);
                ec.clear();

                std::filesystem::rename(temp, path, ec);
            }
        }
        catch (...) {
            // Cache failures must never fail the artist lookup.
        }
    }

} // namespace


bool listenbrainz_fetch_available_algorithms(
    std::vector<std::string>& out,
    std::string* error_out)
{
    out.clear();

    std::string body;
    std::string error;

    if (!http_get(
        L"https://labs.api.listenbrainz.org/similar-artists",
        body,
        error,
        5000)) {

        if (error_out)
            *error_out = error;

        return false;
    }

    try {
        const std::regex option_re(
            R"rx(<option\b([^>]*)>([\s\S]*?)</option\s*>)rx",
            std::regex_constants::icase);

        const std::regex value_re(
            R"rx(\bvalue\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))rx",
            std::regex_constants::icase);

        const std::regex tag_re(R"rx(<[^>]*>)rx");

        auto decode_entities = [](std::string value) {
            const std::pair<const char*, const char*> entities[] = {
                {"&amp;", "&"},
                {"&quot;", "\""},
                {"&#39;", "'"},
                {"&#x27;", "'"},
                {"&lt;", "<"},
                {"&gt;", ">"},
                {"&nbsp;", " "}
            };

            for (const auto& entity : entities) {
                size_t pos = 0;

                while ((pos = value.find(
                    entity.first,
                    pos)) != std::string::npos) {

                    value.replace(
                        pos,
                        strlen(entity.first),
                        entity.second);

                    pos += strlen(entity.second);
                }
            }

            return value;
            };

        for (std::sregex_iterator it(
            body.begin(),
            body.end(),
            option_re),
            end;
            it != end;
            ++it) {

            const std::string attributes = (*it)[1].str();

            std::string value;
            std::smatch value_match;

            if (std::regex_search(
                attributes,
                value_match,
                value_re)) {

                if (value_match[1].matched)
                    value = value_match[1].str();
                else if (value_match[2].matched)
                    value = value_match[2].str();
                else
                    value = value_match[3].str();
            }
            else {
                value = (*it)[2].str();

                value = std::regex_replace(
                    value,
                    tag_re,
                    " ");
            }

            value = decode_entities(value);

            const auto first =
                value.find_first_not_of(" \t\r\n");

            if (first == std::string::npos)
                continue;

            const auto last =
                value.find_last_not_of(" \t\r\n");

            value = value.substr(
                first,
                last - first + 1);

            if (value.size() > 512 ||
                value.find_first_of(" \t\r\n") !=
                std::string::npos) {
                continue;
            }

            std::string lower = value;

            std::transform(
                lower.begin(),
                lower.end(),
                lower.begin(),
                [](unsigned char c) {
                    return static_cast<char>(
                        std::tolower(c));
                });

            if (lower == "select" ||
                lower == "choose" ||
                lower == "none" ||
                lower == "-" ||
                lower == "--") {
                continue;
            }

            if (std::find(
                out.begin(),
                out.end(),
                value) == out.end()) {
                out.push_back(std::move(value));
            }
        }

        if (out.empty()) {
            if (error_out) {
                *error_out =
                    "No algorithm choices were found in the "
                    "ListenBrainz Labs page. You can still "
                    "enter an identifier manually.";
            }

            return false;
        }

        std::sort(out.begin(), out.end());

        if (error_out)
            error_out->clear();

        return true;
    }
    catch (const std::exception& e) {
        if (error_out) {
            *error_out =
                std::string(
                    "Could not read ListenBrainz algorithm choices: ")
                + e.what();
        }

        return false;
    }
}


const char* listenbrainz_similar_artists::default_algorithm() {
    return "session_based_days_1800_session_300_contribution_3_threshold_10_limit_100_filter_True_skip_30";
}


bool listenbrainz_similar_artists::fetch(
    const std::string& mbid,
    const std::string& algorithm,
    std::vector<listenbrainz_similar_artist>& out,
    std::string* error_out)
{
    out.clear();

    std::string error;

    if (!valid_mbid(mbid)) {
        if (error_out)
            *error_out = "A valid artist MBID is required";

        return false;
    }

    const std::string selected =
        algorithm.empty()
        ? default_algorithm()
        : algorithm;

    if (selected.size() > 512) {
        if (error_out)
            *error_out = "Algorithm identifier is too long";

        return false;
    }

    const std::string key = mbid + "|" + selected;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);

        auto it = g_cache.find(key);

        if (it != g_cache.end() &&
            std::chrono::steady_clock::now() -
            it->second.stored < k_cache_ttl) {

            out = it->second.artists;
            return true;
        }
    }

    // Persistent seven-day cache: read it before making a network request.
    const auto disk_path =
        persistent_cache_path(mbid, selected);

    if (load_disk_cache(disk_path, out)) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);

        g_cache[key] = {
            std::chrono::steady_clock::now(),
            out
        };

        if (error_out)
            error_out->clear();

        return true;
    }

    const std::string encoded_algorithm =
        url_encode(selected);

    const std::wstring url =
        L"https://labs.api.listenbrainz.org/similar-artists/json?artist_mbids="
        + std::wstring(mbid.begin(), mbid.end())
        + L"&algorithm="
        + std::wstring(
            encoded_algorithm.begin(),
            encoded_algorithm.end());

    std::string response;

    if (!http_get(url, response, error)) {
        if (error_out)
            *error_out = error;

        return false;
    }

    try {
        const auto root = json::parse(response);

        if (!root.is_array()) {
            throw std::runtime_error(
                "Unexpected ListenBrainz response format");
        }

        for (const auto& item : root) {
            if (!item.is_object())
                continue;

            listenbrainz_similar_artist a;

            if (item.contains("artist_mbid") &&
                item["artist_mbid"].is_string()) {
                a.mbid =
                    item["artist_mbid"].get<std::string>();
            }

            if (!valid_mbid(a.mbid) || a.mbid == mbid)
                continue;

            auto get = [&](const char* field) {
                return item.contains(field) &&
                    item[field].is_string()
                    ? item[field].get<std::string>()
                    : std::string();
                };

            a.name = get("name");
            a.comment = get("comment");
            a.type = get("type");
            a.gender = get("gender");
            a.reference_mbid = get("reference_mbid");

            if (item.contains("score") &&
                item["score"].is_number()) {
                a.score =
                    item["score"].get<double>();
            }

            out.push_back(std::move(a));
        }
    }
    catch (const std::exception& e) {
        if (error_out) {
            *error_out =
                std::string(
                    "Could not parse ListenBrainz response: ")
                + e.what();
        }

        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);

        g_cache[key] = {
            std::chrono::steady_clock::now(),
            out
        };
    }

    save_disk_cache(disk_path, out);

    if (error_out)
        error_out->clear();

    return true;
}
