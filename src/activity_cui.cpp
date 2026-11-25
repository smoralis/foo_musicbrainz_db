// Columns UI panel: "MusicBrainz DB activity".
// Compiled only when the columns_ui-sdk headers are on the include path.
#include "stdafx.h"

#if __has_include(<columns_ui-sdk/ui_extension.h>)
#include <columns_ui-sdk/ui_extension.h>
#include <columns_ui-sdk/colours.h>
#define MBDB_HAVE_CUI 1
#elif __has_include(<ui_extension.h>)
#include <ui_extension.h>
#include <colours.h>
#define MBDB_HAVE_CUI 1
#endif

#ifdef MBDB_HAVE_CUI
#include "activity_window.h"

namespace {
const GUID guid_cui_nowplaying = { 0xbad49171, 0x1b85, 0x4f5b, { 0x8e, 0x0e, 0x4c, 0x1d, 0x66, 0xb0, 0xa4, 0xf1 } };
const GUID guid_cui_selected = { 0x7e1a52c8, 0x3d94, 0x4f06, { 0xa8, 0x2b, 0x91, 0x6c, 0x0d, 0xf4, 0x35, 0xe7 } };
const GUID guid_cui_activity = { 0xc8543529, 0x2e8d, 0x4a4e, { 0x92, 0xfb, 0x0f, 0x44, 0x0a, 0x79, 0x7c, 0xd7 } };

// Colours come from Columns UI's global colour scheme (including its dark mode).
class cui_colours_host : public activity_host {
public:
    bool query_colours(COLORREF& background, COLORREF& text) override {
        background = m_helper.get_colour(cui::colours::colour_background);
        text = m_helper.get_colour(cui::colours::colour_text);
        return true;
    }
    HFONT query_font() override { return nullptr; } // default GUI font
private:
    cui::colours::helper m_helper{ pfc::guid_null };
};

// Mode 0 is the full activity panel, 1 the standalone "Now playing" panel and 2 the standalone
// "Selected Artist" panel.
template <int Mode>
class cui_panel_t : public uie::window {
public:
    const GUID& get_extension_guid() const override { return Mode == 1 ? guid_cui_nowplaying : Mode == 2 ? guid_cui_selected : guid_cui_activity; }
    void get_name(pfc::string_base& out) const override {
        out = Mode == 1 ? "MusicBrainz DB now playing" : Mode == 2 ? "MusicBrainz DB selected artist" : "MusicBrainz DB activity";
    }
    void get_category(pfc::string_base& out) const override { out = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }
    bool is_available(const uie::window_host_ptr&) const override { return true; }
    const uie::window_host_ptr& get_host() const { return m_window_host; }
    HWND get_wnd() const override { return m_window; }

    HWND create_or_transfer_window(HWND parent, const uie::window_host_ptr& host,
                                   const ui_helpers::window_position_t& position) override {
        if (m_window.IsWindow()) {
            // Moving between hosts (e.g. layout change): reparent the existing window.
            ShowWindow(m_window, SW_HIDE);
            m_window.SetParent(parent);
            m_window_host->relinquish_ownership(m_window);
            m_window_host = host;
            m_window.SetWindowPos(nullptr, static_cast<int>(position.x), static_cast<int>(position.y),
                static_cast<int>(position.cx), static_cast<int>(position.cy), SWP_NOZORDER);
        } else {
            m_window_host = host;
            m_window.set_host(&m_colours);
            if (Mode == 1) m_window.show_now_playing_only();
            if (Mode == 2) m_window.show_selected_only();
            // x/y/cx/cy are not all the same signedness, so convert explicitly.
            const LONG left = static_cast<LONG>(position.x), top = static_cast<LONG>(position.y);
            RECT rc = { left, top, left + static_cast<LONG>(position.cx), top + static_cast<LONG>(position.cy) };
            m_window.Create(parent, rc, nullptr,
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, WS_EX_CONTROLPARENT);
        }
        return m_window;
    }

    void destroy_window() override {
        if (m_window.IsWindow()) m_window.DestroyWindow();
        m_window_host.release();
    }

private:
    activity_window m_window;
    cui_colours_host m_colours;
    uie::window_host_ptr m_window_host;
};

uie::window_factory<cui_panel_t<0>> g_cui_activity_factory;
uie::window_factory<cui_panel_t<1>> g_cui_nowplaying_factory;
uie::window_factory<cui_panel_t<2>> g_cui_selected_factory;
} // namespace
#endif // MBDB_HAVE_CUI
