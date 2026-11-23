#pragma once
#include "stdafx.h"
#include <memory>
#include "artist_info_view.h"
#include <mutex>
#include <string>
#include <vector>
#include "../include/musicbrainz_db_types.h"
#include "activity_webview.h"
#include "listenbrainz_similar_artists.h"

// Result of a background "list artists" request, polled by the window's timer.
struct activity_reload_state {
    std::mutex m;
    bool ready = false;
    uint64_t revision = 0;
    std::vector<artist_list_item> rows;
};

// Result of a background lookup of the playing artist's stored record.
struct activity_np_fetch {
    std::mutex m;
    bool ready = false;
    bool ok = false;
    artist_record record;
    std::shared_ptr<const mbdb_info_content> content; // built on the background thread
};

// Result of a ListenBrainz request; the worker only writes under the mutex and
// the window timer installs the result on the UI thread.
struct activity_similar_artists_state {
    std::mutex m;
    bool ready = false;
    bool ok = false;
    std::string artist_mbid;
    std::string error;
    std::vector<listenbrainz_similar_artist> artists;
};

// What the panel needs from its host (DUI element or CUI panel).
class activity_host {
public:
    virtual ~activity_host() {}
    // Fill in the host's current background/text colours.
    virtual bool query_colours(COLORREF& background, COLORREF& text) = 0;
    // The host's preferred font, or nullptr for the default GUI font. Not owned.
    virtual HFONT query_font() = 0;
    // True while the host is in layout edit mode (Default UI). In that mode the
    // panel must not take mouse input so the host can select and move it.
    virtual bool is_edit_mode() { return false; }
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
    void on_edit_mode_changed(); // call when the host's layout edit mode is toggled
    void show_now_playing_only(); // standalone "Now playing" panel: only that view, no title/tabs

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
        MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDblClk)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnThemeMessage)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnThemeMessage)
        MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
        MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
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
    LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonDblClk(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnNotify(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnThemeMessage(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&);

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
    // Scrollbar beside the events list / artists table (activity_scrollbar.cpp)
    void draw_scrollbar(HDC dc, int left, int top, int bottom, int dpi, size_t total, size_t visible, size_t offset);
    bool scrollbar_mouse_down(POINT pt);
    void scrollbar_mouse_move(POINT pt);
    void scrollbar_mouse_up();
    size_t scroll_max() const;
    void set_scroll(long long offset);

    // Now-playing tab (activity_nowplaying.cpp)
    void paint_nowplaying(HDC dc, const RECT& rc, int x, int right, int& y, int line_h, int gap, int pad, int dpi);
    bool poll_nowplaying();
    void render_nowplaying();
    void create_info();
    void position_info(const RECT& r);

    bool artists_dbl_click(POINT pt);     // opens the artist info window
    void artists_wheel(int steps);
    void artists_context_menu(POINT screen_pt);
    void reload_artists();   // asks the background thread for a fresh list
    bool poll_reload();      // installs a finished list; true if anything changed
    void request_similar_artists();
    std::vector<std::string> similar_missing_mbids() const; // similar artists not yet in the database
    void download_similar_artists();
    void download_similar_of_selected();
    bool poll_similar_artists();
    void paint_similar_artists(HDC dc, const RECT& pane, int line_h, int gap, int dpi);
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
    // 6 = Download similar artists (similar pane of the artists tab).
    // 7 = Download similar artists of the selected rows (artists toolbar).
    RECT m_btn[8] = {};
    int m_hot = -1, m_down = -1;
    RECT m_sb_track = {}, m_sb_thumb = {};   // empty when the list fits
    size_t m_sb_total = 0, m_sb_visible = 0;
    int m_sb_grab = 0;
    bool m_sb_dragging = false, m_sb_hot = false;

    // Now-playing tab
    CWindow m_info;                         // Rich Edit showing the playing artist
    RECT m_info_applied = {};
    uint64_t m_np_revision = 0, m_np_db_revision = 0;
    int m_np_state = 0;                     // mbdb_activity::np_state_t
    std::string m_np_label, m_np_mbid;
    bool m_np_has_record = false, m_np_raw = false, m_np_dirty = true;
    artist_record m_np_record;
    std::shared_ptr<const mbdb_info_content> m_np_content; // ready-to-render sections for the record
    std::shared_ptr<activity_np_fetch> m_np_fetch;
    bool m_tracking = false;
	activity_webview m_webview;
		
    // Artists tab
    bool m_np_only = false;                 // standalone Now playing panel
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
    RECT m_tab[3] = {}, m_header_rc = {}, m_list_rc = {}, m_similar_rc = {};
    std::shared_ptr<activity_similar_artists_state> m_similar_fetch;
    std::string m_similar_requested_mbid, m_similar_error;
    std::vector<listenbrainz_similar_artist> m_similar_rows;
    bool m_similar_loading = false;
    int m_similar_row_top = 0, m_similar_row_height = 0;
    size_t m_similar_scroll = 0, m_similar_visible_rows = 0, m_similar_max_scroll = 0;
    long long m_similar_selected = -1;
    RECT m_similar_sb_track = {}, m_similar_sb_thumb = {};
    int m_similar_sb_grab = 0;
    bool m_similar_sb_dragging = false;
    int m_col_x[4] = {};
    int m_row_h = 0;
    bool m_batch_was_visible = false; // so the timer repaints once more when the bar hides
	
};
