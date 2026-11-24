#include "stdafx.h"
#include <helpers/DarkMode.h>
#include <mutex>
#include "preferences.h"
#include "resource.h"
#include "../include/listenbrainz_similar_artists.h"

namespace {

// Preferences > Tools > MusicBrainz DB
const GUID guid_prefs_page = { 0x88e02942, 0x8d89, 0x4878, { 0x9f, 0x79, 0x5c, 0x60, 0x42, 0xd9, 0xbb, 0xa8 } };

// Preferences > Tools > MusicBrainz DB > Browser
const GUID guid_prefs_browser_page = { 0x1d6e3f84, 0x2a7b, 0x4c95, { 0xb0, 0x48, 0x5e, 0x9a, 0xd3, 0x71, 0x0f, 0xc6 } };

const GUID guid_cfg_debug_log    = { 0x4602f224, 0xbb4d, 0x4fa4, { 0xb0, 0x3d, 0x40, 0x86, 0xc9, 0xcd, 0x8e, 0x7e } };
const GUID guid_cfg_artist_field = { 0x9c34da68, 0x9e46, 0x44b8, { 0x88, 0xd6, 0xa3, 0xa3, 0x81, 0x50, 0xf9, 0x35 } };
const GUID guid_cfg_title_field  = { 0x6a400bd4, 0x3dee, 0x4c4b, { 0xb3, 0xff, 0xa4, 0x88, 0xa4, 0xc9, 0x1d, 0xec } };

const GUID guid_cfg_dynamic_delay = { 0x2b0529ae, 0xc9a6, 0x447e, { 0x99, 0xe7, 0x97, 0xa2, 0x6e, 0x3d, 0xd8, 0x2b } };
const GUID guid_cfg_rate_limit    = { 0x5d1c7a3e, 0x2f84, 0x4b19, { 0xa6, 0x0e, 0x91, 0x3c, 0xd4, 0x7b, 0x28, 0xf5 } };
const GUID guid_cfg_lb_algorithm  = { 0x9c90a8b1, 0x5e16, 0x48f4, { 0x9b, 0x62, 0x32, 0x17, 0x84, 0x29, 0xa1, 0x0c } };

constexpr unsigned k_max_dynamic_delay = 300; // seconds

constexpr unsigned k_min_rate_limit = 1;      // seconds; never allow less
constexpr unsigned k_default_rate_limit = 2;  // seconds
constexpr unsigned k_max_rate_limit = 60;     // seconds

const GUID guid_cfg_auto_page     = { 0x3e7b1c52, 0x8d0a, 0x4f63, { 0xa1, 0x95, 0x6b, 0x2d, 0x0c, 0xe4, 0x7a, 0x18 } };

const GUID guid_cfg_force_dark    = { 0x7a2f94d1, 0x13c8, 0x4e0b, { 0x8f, 0x5a, 0xc6, 0x31, 0xb9, 0x0d, 0x42, 0xe7 } };

const GUID guid_cfg_block_ads     = { 0xc4d8e0a6, 0x5b27, 0x4a91, { 0x9e, 0x13, 0x08, 0xf7, 0x6a, 0x3b, 0x5c, 0xd2 } };

const GUID guid_cfg_no_browser    = { 0x68b5d2e9, 0x40c1, 0x4f7a, { 0x83, 0x6e, 0x2a, 0xd9, 0x15, 0xc7, 0xb0, 0x4e } };

constexpr bool k_default_no_browser = false;
constexpr bool k_default_block_ads = false;
constexpr bool k_default_force_dark = false;
constexpr bool k_default_auto_page = false;
constexpr bool k_default_debug_log = false; // debug logging is off unless the user enables it

cfg_bool   g_cfg_debug_log(guid_cfg_debug_log, k_default_debug_log);
cfg_bool   g_cfg_auto_page(guid_cfg_auto_page, k_default_auto_page);
cfg_bool   g_cfg_force_dark(guid_cfg_force_dark, k_default_force_dark);
cfg_bool   g_cfg_block_ads(guid_cfg_block_ads, k_default_block_ads);
cfg_bool   g_cfg_no_browser(guid_cfg_no_browser, k_default_no_browser);
cfg_string g_cfg_artist(guid_cfg_artist_field, "");
cfg_string g_cfg_title(guid_cfg_title_field, "");
cfg_int    g_cfg_dynamic_delay(guid_cfg_dynamic_delay, 0);
cfg_int    g_cfg_rate_limit(guid_cfg_rate_limit, k_default_rate_limit);
cfg_string g_cfg_lb_algorithm(guid_cfg_lb_algorithm, "session_based_days_1800_session_300_contribution_3_threshold_10_limit_100_filter_True_skip_30");

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
            g_cfg_rate_limit = static_cast<t_int32>(read_rate_limit());
            g_cfg_lb_algorithm.set_string(read_edit(IDC_LB_ALGORITHM).get_ptr());
        }
        SetDlgItemInt(IDC_RATE_LIMIT, read_rate_limit(), FALSE); // shows the clamped value
        m_callback->on_state_changed();
    }

    void reset() override {
        CheckDlgButton(IDC_DEBUG_LOG, k_default_debug_log ? BST_CHECKED : BST_UNCHECKED);
        uSetDlgItemText(*this, IDC_ARTIST_FIELD, "");
        uSetDlgItemText(*this, IDC_TITLE_FIELD, "");
        SetDlgItemInt(IDC_DYNAMIC_DELAY, 0, FALSE);
        SetDlgItemInt(IDC_RATE_LIMIT, k_default_rate_limit, FALSE);
        uSetDlgItemText(*this, IDC_LB_ALGORITHM, "session_based_days_1800_session_300_contribution_3_threshold_10_limit_100_filter_True_skip_30");
        update_status();
        m_callback->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(mbdb_prefs_instance)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_DEBUG_LOG, BN_CLICKED, OnEdited)
        COMMAND_HANDLER_EX(IDC_ARTIST_FIELD, EN_CHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_TITLE_FIELD, EN_CHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_DYNAMIC_DELAY, EN_CHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_RATE_LIMIT, EN_CHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_LB_ALGORITHM, CBN_EDITCHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_LB_ALGORITHM, CBN_SELCHANGE, OnEdited)
        COMMAND_HANDLER_EX(IDC_LB_LOAD_ALGORITHMS, BN_CLICKED, OnLoadAlgorithms)
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
            SetDlgItemInt(IDC_RATE_LIMIT, mbdb_prefs::rate_limit_seconds(), FALSE);
            uSetDlgItemText(*this, IDC_LB_ALGORITHM, g_cfg_lb_algorithm.get_ptr());
        }
        ::SendMessage(GetDlgItem(IDC_ARTIST_FIELD), EM_SETCUEBANNER, FALSE, (LPARAM)L"%artist% (default)");
        ::SendMessage(GetDlgItem(IDC_TITLE_FIELD), EM_SETCUEBANNER, FALSE, (LPARAM)L"%title% (default)");
        ::SendMessage(GetDlgItem(IDC_DYNAMIC_DELAY), EM_SETLIMITTEXT, 3, 0);
        ::SendMessage(GetDlgItem(IDC_RATE_LIMIT), EM_SETLIMITTEXT, 2, 0);
        update_status();
        return FALSE;
    }

    void OnLoadAlgorithms(UINT, int, CWindow) {
        ::SetDlgItemTextW(m_hWnd, IDC_LB_ALGORITHM_STATUS, L"Loading choices from ListenBrainz Labs...");
        ::UpdateWindow(GetDlgItem(IDC_LB_ALGORITHM_STATUS));
        std::vector<std::string> algorithms;
        std::string error;
        if (!listenbrainz_fetch_available_algorithms(algorithms, &error)) {
            uSetDlgItemText(*this, IDC_LB_ALGORITHM_STATUS, error.c_str());
            return;
        }
        HWND combo = GetDlgItem(IDC_LB_ALGORITHM);
        const pfc::string8 current = read_edit(IDC_LB_ALGORITHM);
        SendMessage(combo, CB_RESETCONTENT, 0, 0);
        for (const auto& algorithm : algorithms) {
            const std::wstring wide(algorithm.begin(), algorithm.end());
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        }
        SetWindowTextA(combo, current.get_ptr());
        uSetDlgItemText(*this, IDC_LB_ALGORITHM_STATUS,
            (std::to_string(algorithms.size()) + " algorithm choice(s) loaded. Select one or keep typing a custom identifier.").c_str());
        m_callback->on_state_changed();
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

    // Seconds typed into the rate limit box, clamped to k_min_rate_limit..k_max_rate_limit
    // (an empty box or 0 counts as the minimum).
    unsigned read_rate_limit() {
        const UINT v = GetDlgItemInt(IDC_RATE_LIMIT, nullptr, FALSE);
        return v < k_min_rate_limit ? k_min_rate_limit : (v > k_max_rate_limit ? k_max_rate_limit : v);
    }

    bool has_changed() {
        std::lock_guard<std::mutex> lock(g_cfg_mutex);
        return is_debug_checked() != g_cfg_debug_log.get_value()
            || strcmp(read_edit(IDC_ARTIST_FIELD).get_ptr(), g_cfg_artist.get_ptr()) != 0
            || strcmp(read_edit(IDC_TITLE_FIELD).get_ptr(), g_cfg_title.get_ptr()) != 0
            || strcmp(read_edit(IDC_LB_ALGORITHM).get_ptr(), g_cfg_lb_algorithm.get_ptr()) != 0
            || read_delay() != static_cast<unsigned>(g_cfg_dynamic_delay.get_value())
            || read_rate_limit() != mbdb_prefs::rate_limit_seconds();
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

// Sub-page "Browser" under MusicBrainz DB: options of the Now playing browser.
class mbdb_browser_prefs_instance : public CDialogImpl<mbdb_browser_prefs_instance>, public preferences_page_instance {
public:
    explicit mbdb_browser_prefs_instance(preferences_page_callback::ptr callback) : m_callback(callback) {}

    enum { IDD = IDD_MBDB_PREFS_BROWSER };

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (has_changed()) state |= preferences_state::changed;
        return state;
    }
    HWND get_wnd() override { return m_hWnd; }

    void apply() override {
        g_cfg_no_browser = checked(IDC_NO_BROWSER);
        g_cfg_force_dark = checked(IDC_FORCE_DARK_BROWSER);
        g_cfg_block_ads = checked(IDC_BLOCK_ADS);
        m_callback->on_state_changed();
    }

    void reset() override {
        CheckDlgButton(IDC_NO_BROWSER, k_default_no_browser ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_FORCE_DARK_BROWSER, k_default_force_dark ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_BLOCK_ADS, k_default_block_ads ? BST_CHECKED : BST_UNCHECKED);
        m_callback->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(mbdb_browser_prefs_instance)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_NO_BROWSER, BN_CLICKED, OnEdited)
        COMMAND_HANDLER_EX(IDC_FORCE_DARK_BROWSER, BN_CLICKED, OnEdited)
        COMMAND_HANDLER_EX(IDC_BLOCK_ADS, BN_CLICKED, OnEdited)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM) {
        m_dark.AddDialogWithControls(*this);
        CheckDlgButton(IDC_NO_BROWSER, g_cfg_no_browser.get_value() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_FORCE_DARK_BROWSER, g_cfg_force_dark.get_value() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_BLOCK_ADS, g_cfg_block_ads.get_value() ? BST_CHECKED : BST_UNCHECKED);
        return FALSE;
    }

    void OnEdited(UINT, int, CWindow) { m_callback->on_state_changed(); }

    bool checked(int id) { return IsDlgButtonChecked(id) == BST_CHECKED; }

    bool has_changed() {
        return checked(IDC_NO_BROWSER) != g_cfg_no_browser.get_value()
            || checked(IDC_FORCE_DARK_BROWSER) != g_cfg_force_dark.get_value()
            || checked(IDC_BLOCK_ADS) != g_cfg_block_ads.get_value();
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CDarkModeHooks m_dark;
};

class mbdb_browser_preferences_page : public preferences_page_v3 {
public:
    const char* get_name() { return "Browser"; }
    GUID get_guid() { return guid_prefs_browser_page; }
    GUID get_parent_guid() { return guid_prefs_page; }

    preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr callback) override {
        auto page = fb2k::service_new<mbdb_browser_prefs_instance>(callback);
        page->Create(parent);
        return page;
    }
};

FB2K_SERVICE_FACTORY(mbdb_browser_preferences_page);

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

bool auto_load_artist_page() { return g_cfg_auto_page.get_value(); }

void set_auto_load_artist_page(bool enabled) { g_cfg_auto_page = enabled; }

bool force_dark_browser() { return g_cfg_force_dark.get_value(); }

bool block_ads_in_browser() { return g_cfg_block_ads.get_value(); }

bool embedded_browser_disabled() { return g_cfg_no_browser.get_value(); }

unsigned dynamic_delay_seconds() {
    const t_int32 v = g_cfg_dynamic_delay.get_value();
    return v < 0 ? 0u : (static_cast<unsigned>(v) > k_max_dynamic_delay ? k_max_dynamic_delay : static_cast<unsigned>(v));
}

unsigned rate_limit_seconds() {
    const t_int32 v = g_cfg_rate_limit.get_value();
    if (v < static_cast<t_int32>(k_min_rate_limit)) return k_min_rate_limit; // also guards a hand-edited config
    return static_cast<unsigned>(v) > k_max_rate_limit ? k_max_rate_limit : static_cast<unsigned>(v);
}

std::string artist_pattern() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    return std::string(g_cfg_artist.get_ptr());
}

std::string title_pattern() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    return std::string(g_cfg_title.get_ptr());
}

std::string listenbrainz_similar_artists_algorithm() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    return std::string(g_cfg_lb_algorithm.get_ptr());
}

}
