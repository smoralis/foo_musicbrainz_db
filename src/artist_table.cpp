#include "artist_table.h"
#include "provider.h"
#include "debug_log.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>

std::shared_ptr<musicbrainz_db_provider> musicbrainz_db_get_provider();

using json=nlohmann::json;

bool mbdb_artist_list(std::vector<artist_list_item>& out) {
    out.clear();
    auto provider=musicbrainz_db_get_provider();
    return provider&&provider->list_artists(out);
}

bool mbdb_artist_get(const std::string& mbid, artist_record& out) {
    out=artist_record();
    auto provider=musicbrainz_db_get_provider();
    return provider&&provider->get_stored_artist(mbid,out);
}

size_t mbdb_artist_delete(const std::vector<std::string>& mbids) {
    auto provider=musicbrainz_db_get_provider();
    return provider?provider->delete_artists(mbids):0;
}

bool mbdb_artist_export(const std::vector<std::string>& mbids,const std::wstring& path,
                        bool list_only,size_t& count,std::string& error) {
    count=0; error.clear();
    auto provider=musicbrainz_db_get_provider();
    if(!provider) { error="the database is not ready"; return false; }
    try {
        json out;
        if(list_only) {
            out=json::array();
            std::vector<artist_list_item> all;
            if(!provider->list_artists(all)) { error="could not read the database"; return false; }
            const std::set<std::string> wanted(mbids.begin(),mbids.end());
            for(const auto& a:all) {
                if(!wanted.empty()&&!wanted.count(a.mbid)) continue;
                out.push_back({{"mbid",a.mbid},{"name",a.name},{"country",a.country},{"type",a.type}});
            }
        } else {
            out=json::object();
            std::vector<std::pair<std::string,std::string>> records;
            if(!provider->get_artist_jsons(mbids,records)) { error="could not read the database"; return false; }
            for(const auto& r:records) {
                try { out[r.first]=json::parse(r.second); }
                catch(const std::exception&) { MBDB_LOG("export: skipping %s (stored json is invalid)", r.first.c_str()); }
            }
        }
        count=out.size();
        if(count==0) { error="there is nothing to export"; return false; }
        std::ofstream f(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
        if(!f) { error="could not open the file for writing"; count=0; return false; }
        const std::string data=out.dump(2,' ',false,json::error_handler_t::replace);
        f.write(data.data(),static_cast<std::streamsize>(data.size()));
        f.flush();
        if(!f) { error="writing the file failed"; count=0; return false; }
        return true;
    } catch(const std::exception& e) {
        error=e.what(); count=0;
        return false;
    }
}
