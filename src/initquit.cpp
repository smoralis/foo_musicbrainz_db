#include "stdafx.h"
#include "provider.h"
#include "debug_log.h"

namespace {
std::mutex g_provider_mutex;
std::shared_ptr<musicbrainz_db_provider> g_provider;
}

std::shared_ptr<musicbrainz_db_provider> musicbrainz_db_get_provider() {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    return g_provider;
}

namespace {
class musicbrainz_db_initquit : public initquit {
public:
    void on_init() override {
        try {
            MBDB_LOG("on_init: starting");
            // get_profile_path() returns a file:// URL in UTF-8; convert to a native path.
            pfc::string8 nativeProfile;
            foobar2000_io::extract_native_path(core_api::get_profile_path(), nativeProfile);
            const std::filesystem::path profile =
                std::filesystem::u8path(nativeProfile.c_str());
            const auto root = profile / "foo_musicbrainz_db";
            const auto dbDir = root / "database";
            const auto repoDir = root / "repository";
            MBDB_LOG("on_init: profile=%s", profile.u8string().c_str());
            std::filesystem::create_directories(dbDir);
            std::filesystem::create_directories(repoDir);

            auto provider = std::make_shared<musicbrainz_db_provider>();
            const auto dbPath = (dbDir / "musicbrainz.sqlite3").u8string();
            if (!provider->initialize(dbPath, repoDir.u8string())) {
                console::print("foo_musicbrainz_db: database initialization failed");
                return;
            }
            std::lock_guard<std::mutex> lock(g_provider_mutex);
            g_provider = std::move(provider);
            MBDB_LOG("on_init: provider installed, database at %s", dbPath.c_str());
            console::print("foo_musicbrainz_db: initialized");
        } catch (const std::exception& e) {
            pfc::string_formatter message;
            message << "foo_musicbrainz_db: init error: " << e.what();
            console::print(message.get_ptr());
        }
    }

    void on_quit() override {
        musicbrainz_client_abort();
        MBDB_LOG("on_quit: releasing provider");
        std::lock_guard<std::mutex> lock(g_provider_mutex);
        g_provider.reset();
    }
};

FB2K_SERVICE_FACTORY(musicbrainz_db_initquit);
}
