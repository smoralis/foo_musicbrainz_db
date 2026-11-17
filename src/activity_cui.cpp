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

class cui_activity_panel : public uie::window {
public:
    const GUID& get_extension_guid() const override { return guid_cui_activity; }
    void get_name(pfc::string_base& out) const override { out = "MusicBrainz DB activity"; }
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

uie::window_factory<cui_activity_panel> g_cui_activity_factory;
} // namespace
#endif // MBDB_HAVE_CUI
