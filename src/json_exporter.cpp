#include "json_exporter.h"
#include "debug_log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <system_error>

using json=nlohmann::json;
namespace fs=std::filesystem;

json_exporter::json_exporter(const fs::path& root):m_root(root) {
    static const char* const kFiles[kStoreCount] = {
        "artists_all.json", "artists_basic.json", "artists_country.json",
        "artists_genres.json", "artists_relations.json", "artists_tags.json" };
    for(int i=0;i<kStoreCount;++i) m_stores[i].file=m_root/"artists"/kFiles[i];
}
bool json_exporter::valid_mbid(const std::string& s) {
    if (s.size()!=36) return false;
    for (size_t i=0;i<s.size();++i) {
        if (i==8||i==13||i==18||i==23) { if(s[i]!='-') return false; }
        else if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||
                   (s[i]>='A'&&s[i]<='F'))) return false;
    }
    return true;
}
bool json_exporter::write_atomic(const fs::path& path,const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(),ec);
    if(ec) return false;
    fs::path tmp=path; tmp += ".tmp";
    {
        std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
        if(!f) return false;
        f.write(data.data(),static_cast<std::streamsize>(data.size()));
        f.flush();
        if(!f) { f.close(); fs::remove(tmp,ec); return false; }
    }
    fs::rename(tmp,path,ec);
    if(ec) {
        ec.clear(); fs::remove(path,ec);
        if(ec) return false;
        fs::rename(tmp,path,ec);
    }
    if(ec) { fs::remove(tmp,ec); return false; }
    return true;
}
bool json_exporter::update_store(store& st, const std::string& mbid, json value) {
    try {
        if(!st.loaded) {
            // Load the existing file once. If it is unreadable or corrupt we refuse to
            // write, so a good file is never overwritten with a partial one.
            json existing=json::object();
            std::error_code ec;
            if(fs::exists(st.file,ec)) {
                std::ifstream f(st.file,std::ios::binary);
                if(!f) { MBDB_LOG("export: cannot open %s", st.file.u8string().c_str()); return false; }
                existing=json::parse(f);
                if(!existing.is_object()) { MBDB_LOG("export: %s is not a json object, refusing to overwrite", st.file.u8string().c_str()); return false; }
            }
            st.data=std::move(existing);
            st.loaded=true;
            MBDB_LOG("export: loaded %zu record(s) from %s", st.data.size(), st.file.u8string().c_str());
        }
        st.data[mbid]=std::move(value);
        const bool ok=write_atomic(st.file,st.data.dump(2,' ',false,json::error_handler_t::replace));
        MBDB_LOG("export: %s %s (%zu record(s))", ok?"wrote":"FAILED writing", st.file.u8string().c_str(), st.data.size());
        return ok;
    } catch(const std::exception& e) {
        MBDB_LOG("export: error for %s in %s: %s", mbid.c_str(), st.file.u8string().c_str(), e.what());
        return false;
    }
}

namespace {
// Record with the mbid plus whichever of `keys` the artist response has (non-null).
json pick(const json& d, const std::string& mbid, std::initializer_list<const char*> keys) {
    json out=json::object();
    out["mbid"]=mbid;
    for(const char* k:keys) {
        auto it=d.find(k);
        if(it!=d.end()&&!it->is_null()) out[k]=*it;
    }
    return out;
}
// Record with the mbid and artist name plus one list field, always present
// (empty if the artist has none).
json pick_list(const json& d, const std::string& mbid, const char* key) {
    json out=json::object();
    out["mbid"]=mbid;
    auto nm=d.find("name");
    if(nm!=d.end()&&nm->is_string()) out["name"]=*nm;
    auto it=d.find(key);
    out[key]=(it!=d.end()&&it->is_array())?*it:json::array();
    return out;
}
}

bool json_exporter::export_artist(const artist_record& a) {
    if(!valid_mbid(a.mbid)||a.raw_json.empty()) return false;
    json d;
    try { d=json::parse(a.raw_json); }
    catch(const std::exception& e) { MBDB_LOG("export: artist %s has invalid json: %s", a.mbid.c_str(), e.what()); return false; }

    std::lock_guard<std::mutex> lock(m_mutex);
    bool ok=true;
    // `update_store(...) && ok` (not `ok && update_store(...)`) so every file is written.
    ok=update_store(m_stores[kAll],a.mbid,d)&&ok;
    ok=update_store(m_stores[kBasic],a.mbid,pick(d,a.mbid,
        {"name","sort-name","type","type-id","gender","gender-id","disambiguation","life-span","isnis","ipis"}))&&ok;
    ok=update_store(m_stores[kCountry],a.mbid,pick(d,a.mbid,
        {"name","country","area","begin-area","end-area"}))&&ok;
    ok=update_store(m_stores[kGenres],a.mbid,pick_list(d,a.mbid,"genres"))&&ok;
    ok=update_store(m_stores[kRelations],a.mbid,pick_list(d,a.mbid,"relations"))&&ok;
    ok=update_store(m_stores[kTags],a.mbid,pick_list(d,a.mbid,"tags"))&&ok;
    return ok;
}
