#include "musicbrainz_client.h"
#include "debug_log.h"
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
#include <mutex>
#include <thread>

#pragma comment(lib,"winhttp.lib")
using json=nlohmann::json;

namespace {
// nlohmann's value() throws when the key exists but is null (e.g. "end": null),
// so read fields defensively.
std::string jstr(const json& j, const char* key) {
    if(!j.is_object()) return {};
    auto it=j.find(key);
    return (it!=j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
int jint(const json& j, const char* key) {
    if(!j.is_object()) return 0;
    auto it=j.find(key);
    return (it!=j.end() && it->is_number_integer()) ? it->get<int>() : 0;
}
// Quotes a value as a Lucene phrase for MusicBrainz search queries.
std::string lucene_phrase(const std::string& s) {
    std::string o="\"";
    for(char c:s) { if(c=='"'||c=='\\') o+='\\'; o+=c; }
    o+='"';
    return o;
}
constexpr size_t kMaxResponse=10u*1024u*1024u;
// MusicBrainz allows an average of 1 request/second per client and answers
// excess traffic with 503 (+ Retry-After). Stay safely under that.
using mb_clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds kMinInterval(1100);
constexpr int kMaxRetries = 2;          // retries after a 503/429
constexpr int kDefaultBackoffSec = 5;   // used when no Retry-After header is sent
constexpr int kMaxBackoffSec = 60;

std::atomic<bool> g_abort{false};
std::mutex g_turn_mutex;                // held while waiting, so callers queue up in order
std::mutex g_state_mutex;               // guards g_next_allowed (short critical sections)
mb_clock::time_point g_next_allowed{};  // earliest time the next request may start

// Blocks until a request slot is free. Returns false if the component is shutting down.
bool rate_limit() {
    std::lock_guard<std::mutex> turn(g_turn_mutex);
    bool logged = false;
    for(;;) {
        if(g_abort.load()) return false;
        mb_clock::time_point next;
        { std::lock_guard<std::mutex> st(g_state_mutex); next = g_next_allowed; }
        const auto now = mb_clock::now();
        if(now >= next) break;
        if(!logged) {
            MBDB_LOG("rate limit: waiting %lld ms", static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(next-now).count()));
            logged = true;
        }
        // Sleep in short slices: the deadline can be pushed back by a 503, and
        // we want to notice shutdown quickly.
        std::this_thread::sleep_for((std::min<mb_clock::duration>)(next-now, std::chrono::milliseconds(100)));
    }
    // Reserve the next slot immediately so the interval is measured from request start.
    std::lock_guard<std::mutex> st(g_state_mutex);
    g_next_allowed = mb_clock::now() + kMinInterval;
    return true;
}

// Call after a response: spaces the next request from the *end* of this one
// (slow responses must not overlap the next slot) and applies any cool-down.
void note_response(int cooldown_seconds) {
    std::lock_guard<std::mutex> st(g_state_mutex);
    const auto t = mb_clock::now() + (cooldown_seconds > 0
        ? std::chrono::duration_cast<mb_clock::duration>(std::chrono::seconds(cooldown_seconds))
        : std::chrono::duration_cast<mb_clock::duration>(kMinInterval));
    if(t > g_next_allowed) g_next_allowed = t;
}
std::wstring widen(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),
        static_cast<int>(s.size()),nullptr,0);
    if(n<=0) return {};
    std::wstring out(n,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),
        static_cast<int>(s.size()),out.data(),n);
    return out;
}
std::string encode(const std::string& s) {
    static const char h[]="0123456789ABCDEF";
    std::string out;
    for(unsigned char c:s) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
           c=='-'||c=='_'||c=='.'||c=='~') out+=static_cast<char>(c);
        else { out+='%'; out+=h[c>>4]; out+=h[c&15]; }
    }
    return out;
}
}
// One HTTP attempt. On failure `status` holds the HTTP status (0 if none) and
// `retry_after` the Retry-After seconds when the server sent one.
static bool http_request(const std::wstring& path,std::string& response,
                         DWORD& status,int& retry_after) {
    status=0; retry_after=0;
    MBDB_LOG("http GET https://musicbrainz.org%ls", path.c_str());
    HINTERNET session=WinHttpOpen(
        L"foo_musicbrainz_db/0.1 (contact: REPLACE_WITH_CONTACT)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) { MBDB_LOG("http: WinHttpOpen failed (err %lu)", GetLastError()); return false; }
    WinHttpSetTimeouts(session,5000,5000,10000,20000);
    HINTERNET conn=WinHttpConnect(session,L"musicbrainz.org",
        INTERNET_DEFAULT_HTTPS_PORT,0);
    if(!conn) { MBDB_LOG("http: WinHttpConnect failed (err %lu)", GetLastError()); WinHttpCloseHandle(session); return false; }
    HINTERNET req=WinHttpOpenRequest(conn,L"GET",path.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if(!req) { MBDB_LOG("http: WinHttpOpenRequest failed (err %lu)", GetLastError()); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }
    WinHttpAddRequestHeaders(req,L"Accept: application/json\r\n",
        static_cast<DWORD>(-1),WINHTTP_ADDREQ_FLAG_ADD);
    bool ok=false;
    if(WinHttpSendRequest(req,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(req,nullptr)) {
        DWORD size=sizeof(status);
        if(WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)) {
            if(status==200) {
                response.clear(); ok=true;
                for(;;) {
                    DWORD available=0;
                    if(!WinHttpQueryDataAvailable(req,&available)) { ok=false; break; }
                    if(!available) break;
                    if(response.size()+available>kMaxResponse) { ok=false; break; }
                    std::string buf(available,'\0'); DWORD got=0;
                    if(!WinHttpReadData(req,buf.data(),available,&got)) { ok=false; break; }
                    response.append(buf.data(),got);
                }
            } else if(status==503||status==429) {
                wchar_t value[32]={};
                DWORD len=sizeof(value);
                if(WinHttpQueryHeaders(req,WINHTTP_QUERY_CUSTOM,L"Retry-After",
                    value,&len,WINHTTP_NO_HEADER_INDEX))
                    retry_after=_wtoi(value); // seconds form; an HTTP-date parses as 0 -> default backoff
            }
        }
    }
    MBDB_LOG("http: %s (status %lu, %lu bytes, retry-after %d, last error %lu)", ok?"ok":"FAILED",
        status, static_cast<unsigned long>(response.size()), retry_after, ok?0ul:GetLastError());
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return ok;
}
bool musicbrainz_client::http_get(const std::wstring& path,std::string& response) {
    for(int attempt=0;;++attempt) {
        if(!rate_limit()) { MBDB_LOG("http: aborted, shutting down"); return false; }
        DWORD status=0; int retry_after=0;
        if(http_request(path,response,status,retry_after)) { note_response(0); return true; }
        if(status!=503&&status!=429) { note_response(0); return false; }
        // Throttled: honour Retry-After (or back off exponentially) for *all* callers.
        int wait=retry_after>0?retry_after:(kDefaultBackoffSec<<attempt);
        wait=(std::min)(wait,kMaxBackoffSec);
        note_response(wait);
        MBDB_LOG("http: throttled (status %lu), cooling down %d s (attempt %d of %d)",
            status, wait, attempt+1, kMaxRetries+1);
        if(attempt>=kMaxRetries) return false; // cool-down stays in force for later lookups
    }
}
void musicbrainz_client_abort() { g_abort.store(true); }
bool musicbrainz_client::search_artist_title(const std::string& artist,
        const std::string& title,std::string& artist_mbid) {
    artist_mbid.clear();
    if(artist.empty()||title.empty()) return false;
    auto lower=[](std::string s) {
        for(auto& c:s) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string wanted=lower(artist);
    std::string q="recording:"+lucene_phrase(title)+" AND artist:"+lucene_phrase(artist);
    std::string response;
    if(!http_get(widen("/ws/2/recording/?query="+encode(q)+"&fmt=json&limit=1"),
                 response)) return false;
    try {
        auto data=json::parse(response);
        if(!data.contains("recordings")||!data["recordings"].is_array()) return false;
        for(const auto& item:data["recordings"]) {
            if(!item.contains("artist-credit")||!item["artist-credit"].is_array()) continue;
            std::string first, exact;
            for(const auto& credit:item["artist-credit"]) {
                if(!credit.contains("artist")) continue;
                const auto& a=credit["artist"];
                const std::string id=jstr(a,"id");
                if(id.empty()) continue;
                if(first.empty()) first=id;
                if(exact.empty()&&lower(jstr(a,"name"))==wanted) exact=id;
            }
            // Prefer the credited artist whose name matches the tag (collaborations).
            const std::string& pick=exact.empty()?first:exact;
            if(pick.empty()) continue;
            artist_mbid=pick;
            MBDB_LOG("client: title search matched artist %s (%s) via recording '%s'",
                artist_mbid.c_str(), exact.empty()?"first credit":"name match", jstr(item,"title").c_str());
            return true;
        }
    } catch(const std::exception& e) { MBDB_LOG("client: recording search JSON error: %s", e.what()); return false; }
    MBDB_LOG("client: no recording with artist credits in results");
    return false;
}
bool musicbrainz_client::search_artist_name(const std::string& artist,std::string& artist_mbid) {
    artist_mbid.clear();
    if(artist.empty()) return false;
    auto lower=[](std::string s) {
        for(auto& c:s) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string response;
    if(!http_get(widen("/ws/2/artist/?query="+encode("artist:"+lucene_phrase(artist))+
        "&fmt=json&limit=1"),response)) return false;
    try {
        auto data=json::parse(response);
        if(!data.contains("artists")||!data["artists"].is_array()||data["artists"].empty()) {
            MBDB_LOG("client: artist name search returned nothing for '%s'", artist.c_str());
            return false;
        }
        const auto& top=data["artists"][0];
        const std::string id=jstr(top,"id");
        const std::string name=jstr(top,"name");
        const int score=jint(top,"score");
        const bool sameName=lower(name)==lower(artist);
        // With only one result, guard against loosely related hits.
        if(id.empty()||(!sameName&&score<95)) {
            MBDB_LOG("client: artist name search rejected '%s' (%s, score %d)", name.c_str(), id.c_str(), score);
            return false;
        }
        artist_mbid=id;
        MBDB_LOG("client: artist name search matched '%s' => %s (score %d)", name.c_str(), id.c_str(), score);
        return true;
    } catch(const std::exception& e) { MBDB_LOG("client: artist search JSON error: %s", e.what()); return false; }
}
bool musicbrainz_client::get_artist(const std::string& mbid,artist_record& r) {
    if(mbid.empty()) return false;
    std::string response;
    if(!http_get(widen("/ws/2/artist/"+mbid+"?inc=genres+tags+ratings+annotation+aliases+url-rels&fmt=json"),response)) return false;
    try {
        auto d=json::parse(response);
        r.mbid=jstr(d,"id"); r.name=jstr(d,"name");
        r.type=jstr(d,"type"); r.country=jstr(d,"country");
        r.disambiguation=jstr(d,"disambiguation");
        if(d.contains("area")&&d["area"].is_object())
            r.area=jstr(d["area"],"name");
        if(d.contains("begin-area")&&d["begin-area"].is_object())
            r.begin_area=jstr(d["begin-area"],"name");
        if(d.contains("end-area")&&d["end-area"].is_object())
            r.end_area=jstr(d["end-area"],"name");
        if(d.contains("life-span")&&d["life-span"].is_object()) {
            r.begin_date=jstr(d["life-span"],"begin");
            r.end_date=jstr(d["life-span"],"end");
        }
        r.raw_json=d.dump(2,' ',false,json::error_handler_t::replace);
        MBDB_LOG("client: artist %s => name '%s', country '%s' (%zu bytes of json)", r.mbid.c_str(), r.name.c_str(), r.country.c_str(), r.raw_json.size());
        return !r.mbid.empty();
    } catch(const std::exception& e) { MBDB_LOG("client: artist JSON error: %s", e.what()); return false; }
}
