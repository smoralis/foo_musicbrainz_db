#pragma once
#include "stdafx.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../include/musicbrainz_db_types.h"

// Result of a background "list artists" request, polled by the window's timer.
struct activity_reload_state {
    std::mutex m;
    bool ready = false;
    uint64_t revision = 0;
    std::vector<artist_list_item> rows;
};

// What the panel needs from its host (DUI element or CUI panel).
class activity_host {
public:
    virtual ~activity_host() {}
    // Fill in the host's current background/text colours.
    virtual bool query_colours(COLORREF& background, COLORREF& text) = 0;
    // The host's preferred font, or nullptr for the default GUI font. Not owned.
    virtual HFONT query_font() = 0;
};

// Owner-drawn panel showing database statistics and a live event feed. Used by
// both the Default UI element and the Columns UI panel.
//
// Theming: colours come from the host and the window re-reads them on every
// refresh_theme() call (host colour/font notifications) and on its 250 ms timer,
// so switching between light and dark schemes is picked up either way. Dark mode
// is detected from the background luminance and selects readable status colours.
class activity_window : public CWindowImpl<activity_window> {
public:
    DECLARE_WND_CLASS_EX(NULL, CS_DBLCLKS, 0)

    void set_host(activity_host* host) { m_host = host; }
    void refresh_theme(); // call when the host reports colour/font changes

    BEGIN_MSG_MAP(activity_window)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
        MESSAGE_HANDLER(WM_SETFOCUS, OnFocusChange)
        MESSAGE_HANDLER(WM_KILLFOCUS, OnFocusChange)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnThemeMessage)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnThemeMessage)
        MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
    END_MSG_MAP()

private:
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseWheel(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnKeyDown(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnCommand(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnCtlColorEdit(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnGetDlgCode(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnFocusChange(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnThemeMessage(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL&);

    bool update_theme(); // returns true if anything changed
    void paint(HDC dc, const RECT& rc);
    void copy_events();
    int hit_test(POINT pt) const;     // index into m_btn, or -1
    bool button_enabled(int index) const;
    void activate_button(int index);

    // Artists tab (activity_artists.cpp)
    void paint_tabs(HDC dc, int x, int right, int& y, int line_h, int gap, int dpi, long long db_artists);
    void paint_artists(HDC dc, const RECT& rc, int x, int right, int& y, int line_h, int gap, int pad, int dpi);
    void draw_button(HDC dc, int index, const std::wstring& label);
    bool tab_mouse_down(POINT pt);
    bool artists_mouse_down(POINT pt);
    void artists_wheel(int steps);
    void artists_context_menu(POINT screen_pt);
    void reload_artists();   // asks the background thread for a fresh list
    bool poll_reload();      // installs a finished list; true if anything changed
    void create_search();
    void position_search(const RECT& inner);
    void rebuild_haystack();
    void sort_artists();
    void select_range(long long a, long long b, bool clear_first);
    void ensure_visible(long long pos);
    size_t selected_count() const;
    std::vector<std::string> selected_mbids() const; // in display order
    void delete_selected();
    void export_json(bool list_only);
    void copy_mbids();

    activity_host* m_host = nullptr;
    COLORREF m_bg = CLR_INVALID, m_fg = CLR_INVALID;
    COLORREF m_dim = 0, m_accent = 0, m_good = 0, m_warn = 0, m_error = 0;
    bool m_dark = false;
    HFONT m_font = nullptr;  // not owned
    HFONT m_bold = nullptr;  // owned
    uint64_t m_version = 0;
    unsigned m_ticks = 0;
    unsigned long long m_db_size = 0;
    size_t m_scroll = 0, m_max_scroll = 0;
    int m_wheel = 0;
    // Toolbar buttons drawn on the title row: 0 = Search selected, 1 = Cancel queued.
    // Toolbar buttons: 0 = Search selected, 1 = Cancel queued (title row);
    // 2 = Delete selected, 3 = Export JSON, 4 = Refresh (artists tab only).
    RECT m_btn[5] = {};
    int m_hot = -1, m_down = -1;
    bool m_tracking = false;
    // Artists tab
    int m_view = 0;                         // 0 = activity, 1 = artists
    std::vector<artist_list_item> m_rows;
    std::vector<char> m_sel;                // indexed by row id
    std::vector<size_t> m_order;            // display position -> row id
    uint64_t m_rows_revision = 0;
    bool m_rows_loaded = false;
    std::shared_ptr<activity_reload_state> m_reload; // request in flight, if any
    std::vector<std::string> m_haystack;    // lowercase "name\ncountry\nmbid" per row, for the filter
    std::string m_filter;                   // lowercase search text
    CWindow m_search;                       // EDIT control (artists tab)
    RECT m_search_applied = {};
    HBRUSH m_edit_brush = nullptr;
    COLORREF m_edit_bg = 0;
    HFONT m_search_font_applied = nullptr;
    size_t m_ascroll = 0, m_amax_scroll = 0, m_visible_rows = 0;
    int m_sort_col = 0;                     // 0 name, 1 country, 2 mbid
    bool m_sort_asc = true;
    long long m_focus = -1, m_anchor = -1;  // display positions
    RECT m_tab[2] = {}, m_header_rc = {}, m_list_rc = {};
    int m_col_x[4] = {};
    int m_row_h = 0;
    bool m_batch_was_visible = false; // so the timer repaints once more when the bar hides
};
