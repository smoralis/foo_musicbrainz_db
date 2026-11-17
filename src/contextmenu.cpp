// Context menu: <right-click tracks> > MusicBrainz DB > Search MusicBrainz for selected artists
#include "stdafx.h"
#include "lookup_queue_tracks.h"
#include "debug_log.h"

namespace {
const GUID guid_menu_group = { 0x56c0b5b9, 0xf338, 0x40b9, { 0x8a, 0x30, 0x1b, 0x0f, 0x96, 0x6f, 0x90, 0x7c } };
const GUID guid_cmd_search_selection = { 0x418b61af, 0x8384, 0x48bd, { 0xa8, 0xcf, 0xdd, 0xca, 0x5c, 0xe1, 0x33, 0x40 } };

class mbdb_contextmenu : public contextmenu_item_simple {
public:
    enum { cmd_search = 0, cmd_count };

    // The item lives in the "MusicBrainz DB" popup group declared below.
    GUID get_parent() override { return guid_menu_group; }
    unsigned get_num_items() override { return cmd_count; }

    void get_item_name(unsigned, pfc::string_base& out) override {
        out = "Search MusicBrainz for selected artists";
    }
    void get_item_default_path(unsigned, pfc::string_base& out) override {
        out = ""; // legacy menu path; the popup group above defines the submenu
    }
    GUID get_item_guid(unsigned) override { return guid_cmd_search_selection; }
    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Looks up each distinct artist among the selected tracks on MusicBrainz and caches the results.";
        return true;
    }

    // `data` is exactly what was right-clicked (playlist selection, library selection, ...).
    void context_command(unsigned, metadb_handle_list_cref data, const GUID&) override {
        MBDB_LOG("context menu: command run on %zu track(s)", static_cast<size_t>(data.get_count()));
        mbdb_search_tracks(data);
    }
};

contextmenu_group_popup_factory g_menu_group(guid_menu_group, contextmenu_groups::root, "MusicBrainz DB", 0);
FB2K_SERVICE_FACTORY(mbdb_contextmenu);
} // namespace
