#include "stdafx.h"
#include <helpers/DarkMode.h>
#include <mutex>
#include "preferences.h"
#include "resource.h"

namespace {

// Preferences > Tools > MusicBrainz DB
const GUID guid_prefs_page = { 0x88e02942, 0x8d89, 0x4878, { 0x9f, 0x79, 0x5c, 0x60, 0x42, 0xd9, 0xbb, 0xa8 } };

const GUID guid_cfg_debug_log    = { 0x4602f224, 0xbb4d, 0x4fa4, { 0xb0, 0x3d, 0x40, 0x86, 0xc9, 0xcd, 0x8e, 0x7e } };
const GUID guid_cfg_artist_field = { 0x9c34da68, 0x9e46, 0x44b8, { 0x88, 0xd6, 0xa3, 0xa3, 0x81, 0x50, 0xf9, 0x35 } };
const GUID guid_cfg_title_field  = { 0x6a400bd4, 0x3dee, 0x4c4b, { 0xb3, 0xff, 0xa4, 0x88, 0xa4, 0xc9, 0x1d, 0xec } };

const GUID guid_cfg_dynamic_delay = { 0x2b0529ae, 0xc9a6, 0x447e, { 0x99, 0xe7, 0x97, 0xa2, 0x6e, 0x3d, 0xd8, 0x2b } };

constexpr unsigned k_max_dynamic_delay = 300; // seconds

constexpr bool k_default_debug_log = false; // debug logging is off unless the user enables it

cfg_bool   g_cfg_debug_log(guid_cfg_debug_log, k_default_debug_log);
cfg_string g_cfg_artist(guid_cfg_artist_field, "");
cfg_string g_cfg_title(guid_cfg_title_field, "");
cfg_int    g_cfg_dynamic_delay(guid_cfg_dynamic_delay, 0);

// cfg_string is a plain pfc::string8; guard it because lookups read it from
// worker threads while the page may be writing it on the UI thread.
std::mutex g_cfg_mutex;

bool pattern_ok(const char* pattern) {
    if (pattern == nullptr || *pattern == 0) return true; // empty = use the tag
    titleformat_object::ptr script;
    return titleformat_compiler::get()->compile(script, pattern);
}

class mbdb_prefs_instance : public CDialogImpl<mbdb_prefs_instance>, public preferences_page_instance {
public:
    explicit mbdb_prefs_instance(preferences_page_callback::ptr callback) : m_callback(callback) {}

    enum { IDD = IDD_MBDB_PREFS };

    // ---- preferences_page_instance ----
    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (has_changed()) state |= preferences_state::changed;
        return state;
    }
    HWND get_wnd() override { return m_hWnd; }

    void apply() override {
        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            g_cfg_debug_log = is_debug_checked();
            g_cfg_artist.set_string(read_edit(IDC_ARTIST_FIELD).get_ptr());
            g_cfg_title.set_string(read_edit(IDC_TITLE_FIELD).get_ptr());
            g_cfg_dynamic_delay = static_cast<t_int32>(read_delay());
        }
        m_callback->on_state_changed();
    }

    void reset() override {
        CheckDlgButton(IDC_DEBUG_LOG, k_default_debug_log ? BST_CHECKED : BST_UNCHECKED);
        uSetDlgItemText(*this, IDC_ARTIST_FIELD, "");
        uSetDlgItemText(*this, IDC_TITLE_FIELD, "");
        SetDlgItemInt(IDC_DYNAMIC_DELAY, 0, FALSE);
        update_status();
        m_callback->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(mbdb_prefs_instance)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_DEBUG_LOG, BN_CLICKED, OnEdited)
        COMMAND_HANDLER_EX(IDC_ARTIST_FIELD, EN_CHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_TITLE_FIELD, EN_CHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_DYNAMIC_DELAY, EN_CHANGE, OnEdited)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM) {
        // Registers the dialog and every child control with the host's dark mode
        // support; colours follow the host's light/dark setting from here on.
        m_dark.AddDialogWithControls(*this);

        {
            std::lock_guard<std::mutex> lock(g_cfg_mutex);
            CheckDlgButton(IDC_DEBUG_LOG, g_cfg_debug_log.get_value() ? BST_CHECKED : BST_UNCHECKED);
            uSetDlgItemText(*this, IDC_ARTIST_FIELD, g_cfg_artist.get_ptr());
            uSetDlgItemText(*this, IDC_TITLE_FIELD, g_cfg_title.get_ptr());
            SetDlgItemInt(IDC_DYNAMIC_DELAY, static_cast<UINT>(g_cfg_dynamic_delay.get_value()), FALSE);
        }
        ::SendMessage(GetDlgItem(IDC_ARTIST_FIELD), EM_SETCUEBANNER, FALSE, (LPARAM)L"%artist% (default)");
        ::SendMessage(GetDlgItem(IDC_TITLE_FIELD), EM_SETCUEBANNER, FALSE, (LPARAM)L"%title% (default)");
        ::SendMessage(GetDlgItem(IDC_DYNAMIC_DELAY), EM_SETLIMITTEXT, 3, 0);
        update_status();
        return FALSE;
    }

    void OnEdited(UINT, int, CWindow) {
        update_status();
        m_callback->on_state_changed();
    }

    bool is_debug_checked() { return IsDlgButtonChecked(IDC_DEBUG_LOG) == BST_CHECKED; }

    pfc::string8 read_edit(int id) {
        pfc::string8 s;
        uGetDlgItemText(*this, id, s);
        return s;
    }

    // Seconds typed into the delay box, clamped to 0..k_max_dynamic_delay.
    unsigned read_delay() {
        const UINT v = GetDlgItemInt(IDC_DYNAMIC_DELAY, nullptr, FALSE);
        return v > k_max_dynamic_delay ? k_max_dynamic_delay : v;
    }

    bool has_changed() {
        std::lock_guard<std::mutex> lock(g_cfg_mutex);
        return is_debug_checked() != g_cfg_debug_log.get_value()
            || strcmp(read_edit(IDC_ARTIST_FIELD).get_ptr(), g_cfg_artist.get_ptr()) != 0
            || strcmp(read_edit(IDC_TITLE_FIELD).get_ptr(), g_cfg_title.get_ptr()) != 0
            || read_delay() != static_cast<unsigned>(g_cfg_dynamic_delay.get_value());
    }

    void update_status() {
        const bool artistOk = pattern_ok(read_edit(IDC_ARTIST_FIELD).get_ptr());
        const bool titleOk = pattern_ok(read_edit(IDC_TITLE_FIELD).get_ptr());
        const char* text = "";
        if (!artistOk && !titleOk) text = "Neither pattern is valid; the plain tags will be used.";
        else if (!artistOk) text = "The artist pattern is not valid; the artist tag will be used.";
        else if (!titleOk) text = "The title pattern is not valid; the title tag will be used.";
        uSetDlgItemText(*this, IDC_FIELDS_STATUS, text);
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CDarkModeHooks m_dark;
};

class mbdb_preferences_page : public preferences_page_v3 {
public:
    const char* get_name() { return "MusicBrainz DB"; }
    GUID get_guid() { return guid_prefs_page; }
    GUID get_parent_guid() { return preferences_page::guid_tools; }

    preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr callback) override {
        auto page = fb2k::service_new<mbdb_prefs_instance>(callback);
        page->Create(parent);
        return page;
    }
};

FB2K_SERVICE_FACTORY(mbdb_preferences_page);

} // namespace

namespace mbdb_prefs {

bool debug_logging() { return g_cfg_debug_log.get_value(); }

unsigned dynamic_delay_seconds() {
    const t_int32 v = g_cfg_dynamic_delay.get_value();
    return v < 0 ? 0u : (static_cast<unsigned>(v) > k_max_dynamic_delay ? k_max_dynamic_delay : static_cast<unsigned>(v));
}

std::string artist_pattern() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    return std::string(g_cfg_artist.get_ptr());
}

std::string title_pattern() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    return std::string(g_cfg_title.get_ptr());
}

}
