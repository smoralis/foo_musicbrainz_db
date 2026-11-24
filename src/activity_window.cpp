#include "activity_window.h"
#include "activity.h"
#include "ui_font.h"
#include "lookup_queue.h"
#include "preferences.h"
#include <windowsx.h>
#include <algorithm>
#include <filesystem>
#include <vector>

namespace {
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 250;

int mix_channel(int a, int b, int pct_a) { return (a * pct_a + b * (100 - pct_a)) / 100; }
COLORREF mix(COLORREF a, COLORREF b, int pct_a) {
    return RGB(mix_channel(GetRValue(a), GetRValue(b), pct_a),
               mix_channel(GetGValue(a), GetGValue(b), pct_a),
               mix_channel(GetBValue(a), GetBValue(b), pct_a));
}
bool is_dark(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000 < 128;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::wstring format_count(unsigned long long n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}
std::wstring format_bytes(unsigned long long b) {
    wchar_t buf[64];
    if (b < 1024ull) swprintf_s(buf, L"%llu B", b);
    else if (b < 1024ull * 1024ull) swprintf_s(buf, L"%.1f KB", static_cast<double>(b) / 1024.0);
    else swprintf_s(buf, L"%.2f MB", static_cast<double>(b) / 1048576.0);
    return buf;
}
std::wstring format_duration(long long ms) {
    const long long s = ms / 1000;
    wchar_t buf[48];
    if (s < 60) swprintf_s(buf, L"%llds", s);
    else if (s < 3600) swprintf_s(buf, L"%lld min", (s + 30) / 60);
    else swprintf_s(buf, L"%lldh %lldm", s / 3600, (s % 3600) / 60);
    return buf;
}
std::wstring format_time(const mbdb_activity::event_t& e) {
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d:%02d", e.hour, e.minute, e.second);
    return buf;
}
// Database + WAL size on disk. Only touches the filesystem, never SQLite.
unsigned long long db_file_size(const std::string& utf8_path) {
    if (utf8_path.empty()) return 0;
    unsigned long long total = 0;
    std::error_code ec;
    for (const char* suffix : { "", "-wal" }) {
        std::filesystem::path p = std::filesystem::u8path(utf8_path);
        p += suffix;
        const auto size = std::filesystem::file_size(p, ec);
        if (!ec) total += size;
        ec.clear();
    }
    return total;
}
int text_width(HDC dc, const std::wstring& s) {
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    return sz.cx;
}
void draw_text(HDC dc, int x, int y, int right, int height, const std::wstring& s, COLORREF colour) {
    if (x >= right) return;
    SetTextColor(dc, colour);
    RECT r = { x, y, right, y + height };
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r,
        DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
}
} // namespace

// ---------------------------------------------------------------- theme ----

bool activity_window::update_theme() {
    COLORREF bg = GetSysColor(COLOR_WINDOW), fg = GetSysColor(COLOR_WINDOWTEXT);
    HFONT font = nullptr;
    if (m_host) {
        m_host->query_colours(bg, fg);
        font = m_host->query_font();
    }
    if (!font) font = mbdb_ui_default_font(); // e.g. Columns UI: the font its settings define
    if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    const bool changed = bg != m_bg || fg != m_fg || font != m_font;
    // Marked here (not only in the timer): refresh_theme(), called when the host notifies a
    // colour or font change, consumes the change, so the timer would never see it. The Now
    // playing text is rendered into a Rich Edit and must be rebuilt with the new colours.
    if (changed) m_np_dirty = true;
    if (font != m_font) {
        m_font = font;
        if (m_bold) { DeleteObject(m_bold); m_bold = nullptr; }
        LOGFONT lf = {};
        if (GetObject(m_font, sizeof(lf), &lf)) {
            lf.lfWeight = FW_BOLD;
            m_bold = CreateFontIndirect(&lf);
        }
    }
    if (bg != m_bg || fg != m_fg) {
        m_bg = bg; m_fg = fg;
        m_dark = is_dark(bg);
        m_dim = mix(fg, bg, 60);
        // Status colours chosen per scheme so they stay readable on dark and light backgrounds.
        m_accent = m_dark ? RGB(120, 170, 255) : RGB(30, 90, 200);
        m_good = m_dark ? RGB(110, 200, 130) : RGB(30, 130, 60);
        m_warn = m_dark ? RGB(230, 190, 90) : RGB(170, 110, 0);
        m_error = m_dark ? RGB(240, 120, 120) : RGB(190, 40, 40);
        m_edit_bg = mix(fg, bg, 6);
        if (m_edit_brush) DeleteObject(m_edit_brush);
        m_edit_brush = CreateSolidBrush(m_edit_bg);
        if (m_search.IsWindow()) m_search.Invalidate();
    }
    if (font != m_search_font_applied && m_search.IsWindow()) { m_search.SetFont(font); m_search_font_applied = font; }
    return changed;
}

void activity_window::refresh_theme() {
    update_theme();
    if (IsWindow()) Invalidate(FALSE);
}

LRESULT activity_window::OnThemeMessage(UINT, WPARAM, LPARAM, BOOL& handled) {
    refresh_theme();
    handled = FALSE; // let other handlers / the default procedure see it too
    return 0;
}

// ------------------------------------------------------------- lifecycle ---

LRESULT activity_window::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
    update_theme();
    create_search();
    create_info();
    if (!mbdb_prefs::embedded_browser_disabled())
        m_webview.create(m_hWnd); // otherwise created later if the option is switched off
    m_search.SetFont(m_font);
    m_db_size = db_file_size(mbdb_activity::stats().db_path);
    SetTimer(kTimerId, kTimerMs);
    return 0;
}

LRESULT activity_window::OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
    KillTimer(kTimerId);
	m_webview.close();
    if (m_bold) { DeleteObject(m_bold); m_bold = nullptr; }
    if (m_edit_brush) { DeleteObject(m_edit_brush); m_edit_brush = nullptr; }
    handled = FALSE;
    return 0;
}

LRESULT activity_window::OnTimer(UINT, WPARAM wp, LPARAM, BOOL& handled) {
    if (wp != kTimerId) { handled = FALSE; return 0; }
    bool dirty = update_theme(); // also catches dark/light switches the host did not notify us about
    if (dirty) m_np_dirty = true; // the Now playing view re-renders with the new colours
    if ((++m_ticks % 8) == 0) {  // about every 2 s
        const auto size = db_file_size(mbdb_activity::stats().db_path);
        if (size != m_db_size) { m_db_size = size; dirty = true; }
    }
    const uint64_t v = mbdb_activity::version();
    if (v != m_version) { m_version = v; dirty = true; }
    if (poll_reload()) dirty = true;
    if (poll_similar_artists()) dirty = true;
    if (poll_nowplaying()) dirty = true;
    if (m_view == 2 && m_np_dirty) { render_nowplaying(); m_np_dirty = false; dirty = true; }
    if (m_view == 1 && !m_reload && m_rows_revision != mbdb_activity::db_revision()) reload_artists();
    if (mbdb_activity::working()) dirty = true; // animates the "Working..." dots
    if (mbdb_activity::searching()) dirty = true; // animates the "Searching..." dots
    if (mbdb_activity::batch_visible() || m_batch_was_visible) dirty = true; // progress bar + its hide timer
    if (dirty) Invalidate(FALSE);
    return 0;
}

LRESULT activity_window::OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) { return 1; }

LRESULT activity_window::OnSize(UINT, WPARAM, LPARAM, BOOL& handled) {
    Invalidate(FALSE);
    handled = FALSE;
    return 0;
}

LRESULT activity_window::OnMouseWheel(UINT, WPARAM wp, LPARAM, BOOL&) {
    m_wheel += GET_WHEEL_DELTA_WPARAM(wp);
    const int steps = m_wheel / WHEEL_DELTA;
    if (steps != 0) {
        m_wheel -= steps * WHEEL_DELTA;
        if (m_view == 1) { artists_wheel(steps); return 0; }
        // Newest events are at the top: wheel up = towards newer.
        long long target = static_cast<long long>(m_scroll) - static_cast<long long>(steps) * 3;
        target = (std::max)(0LL, (std::min)(target, static_cast<long long>(m_max_scroll)));
        m_scroll = static_cast<size_t>(target);
        Invalidate(FALSE);
    }
    return 0;
}

// ---------------------------------------------------------------- buttons --

int activity_window::hit_test(POINT pt) const {
    for (int i = 0; i < 9; ++i) if (PtInRect(&m_btn[i], pt)) return i;
    return -1;
}

bool activity_window::button_enabled(int index) const {
    switch (index) {
    case 1: return mbdb_activity::stats().queued > 0; // "Cancel" needs something queued
    case 2: return m_view == 1 && selected_count() > 0;
    case 3: return m_view == 1 && !m_rows.empty();
    case 8: return m_view == 2 && !mbdb_prefs::embedded_browser_disabled();
    case 7: return m_view == 1 && selected_count() > 0;
    case 6: return m_view == 1 && !m_similar_loading && !similar_missing_mbids().empty();
    case 5: return m_view == 2 && m_np_has_record && m_np_content && mbdb_info_has_raw(*m_np_content);
    default: return true;
    }
}

void activity_window::activate_button(int index) {
    if (index == 0) mbdb_search_selection();
    else if (index == 1) mbdb_cancel_queued();
    else if (index == 2) delete_selected();
    else if (index == 3) export_json(false);
    else if (index == 4) reload_artists();
    else if (index == 6) download_similar_artists();
    else if (index == 7) download_similar_of_selected();
    else if (index == 8) { mbdb_prefs::set_auto_load_artist_page(!mbdb_prefs::auto_load_artist_page()); m_np_dirty = true; }
    else if (index == 5) { m_np_raw = !m_np_raw; m_np_dirty = true; Invalidate(FALSE); }
}

LRESULT activity_window::OnMouseMove(UINT, WPARAM, LPARAM lp, BOOL&) {
    const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (!m_tracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
        TrackMouseEvent(&tme);
        m_tracking = true;
    }
    if (m_similar_sb_dragging) {
        const int track_h = m_similar_sb_track.bottom - m_similar_sb_track.top;
        const int thumb_h = m_similar_sb_thumb.bottom - m_similar_sb_thumb.top;
        const int travel = track_h - thumb_h;
        if (travel > 0) {
            const int thumb_top = (std::max)(m_similar_sb_track.top, (std::min)(pt.y - m_similar_sb_grab, m_similar_sb_track.bottom - thumb_h));
            const double fraction = static_cast<double>(thumb_top - m_similar_sb_track.top) / travel;
            m_similar_scroll = static_cast<size_t>(fraction * m_similar_max_scroll + 0.5);
            Invalidate(FALSE);
        }
        return 0;
    }
    if (m_sb_dragging) { scrollbar_mouse_move(pt); return 0; }
    const bool sb_hot = PtInRect(&m_sb_track, pt) != 0;
    if (sb_hot != m_sb_hot) { m_sb_hot = sb_hot; Invalidate(FALSE); }
    const int hot = hit_test(pt);
    if (hot != m_hot) { m_hot = hot; Invalidate(FALSE); }
    return 0;
}

LRESULT activity_window::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&) {
    m_tracking = false;
    if (m_hot != -1) { m_hot = -1; Invalidate(FALSE); }
    if (m_sb_hot) { m_sb_hot = false; Invalidate(FALSE); }
    return 0;
}

LRESULT activity_window::OnLButtonDown(UINT, WPARAM, LPARAM lp, BOOL& handled) {
    const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (tab_mouse_down(pt)) return 0;
    if (m_view == 1 && PtInRect(&m_similar_rc, pt) && !PtInRect(&m_btn[6], pt) && artists_mouse_down(pt)) return 0;
    if (scrollbar_mouse_down(pt)) return 0;
    const int idx = hit_test(pt);
    if (idx >= 0 && button_enabled(idx)) {
        m_down = idx;
        SetCapture();
        Invalidate(FALSE);
        return 0;
    }
    if (artists_mouse_down(pt)) return 0;
    handled = FALSE;
    return 0;
}

// A double-click on an artist row opens its info window. Anywhere else the second
// click of a double-click is handled like an ordinary press, as before the panel
// had a double-click action, so quick repeated clicks on buttons and tabs still work.
LRESULT activity_window::OnLButtonDblClk(UINT, WPARAM, LPARAM lp, BOOL& handled) {
    const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (!PtInRect(&m_sb_track, pt) && artists_dbl_click(pt)) return 0; // the scrollbar is not a row
    return OnLButtonDown(WM_LBUTTONDOWN, 0, lp, handled);
}

LRESULT activity_window::OnLButtonUp(UINT, WPARAM, LPARAM lp, BOOL& handled) {
    if (m_similar_sb_dragging) { m_similar_sb_dragging = false; ReleaseCapture(); Invalidate(FALSE); return 0; }
    if (m_sb_dragging) { scrollbar_mouse_up(); return 0; }
    if (m_down < 0) { handled = FALSE; return 0; }
    const int down = m_down;
    m_down = -1;
    ReleaseCapture();
    const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (hit_test(pt) == down) activate_button(down); // released over the same button
    Invalidate(FALSE);
    return 0;
}

// ----------------------------------------------------------- context menu --

void activity_window::copy_events() {
    const size_t total = mbdb_activity::event_count();
    const auto all = mbdb_activity::events(0, total); // newest first
    std::wstring text;
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        text += format_time(*it) + L"  " + widen(it->text) + L"\r\n";
    }
    if (!OpenClipboard()) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// In DUI layout edit mode the host selects elements by clicking them. Report the
// panel as transparent to hit testing so those clicks go to the host instead.
LRESULT activity_window::OnNcHitTest(UINT, WPARAM, LPARAM, BOOL& handled) {
    if (m_host && m_host->is_edit_mode()) return HTTRANSPARENT;
    handled = FALSE;
    return 0;
}

// The artists-tab search box is a child window and would still swallow clicks in
// edit mode, so hide it while editing and restore it afterwards.
void activity_window::on_edit_mode_changed() {
    if (m_search.IsWindow()) {
        const bool editing = m_host && m_host->is_edit_mode();
        m_search.ShowWindow(editing || m_view != 1 ? SW_HIDE : SW_SHOW);
    }
    if (m_info.IsWindow()) {
        const bool editing = m_host && m_host->is_edit_mode();
        m_info.ShowWindow(editing || m_view != 2 ? SW_HIDE : SW_SHOW);
    }
	
    const bool editing = m_host && m_host->is_edit_mode();
	m_webview.show(!editing && m_view == 2 && !mbdb_prefs::embedded_browser_disabled());
	
    Invalidate();
}

LRESULT activity_window::OnContextMenu(UINT, WPARAM, LPARAM lp, BOOL&) {
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (m_view == 1) {
        // The artists menu (delete, export, ...) acts on the table's selection, so the
        // similar artists pane next to it gets a menu of its own.
        if (pt.x != -1 || pt.y != -1) {
            POINT client = pt;
            ScreenToClient(&client);
            if (PtInRect(&m_similar_rc, client)) { similar_context_menu(pt); return 0; }
        }
        artists_context_menu(pt);
        return 0;
    }
    if (m_view == 2) return 0; // the Rich Edit has its own menu
    if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt); // keyboard-invoked
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Copy events");
    AppendMenuW(menu, MF_STRING, 2, L"Clear events");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) copy_events();
    else if (cmd == 2) { mbdb_activity::clear_events(); m_scroll = 0; }
    return 0;
}

// ---------------------------------------------------------------- paint ----

LRESULT activity_window::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(&ps);
    RECT rc;
    GetClientRect(&rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w > 0 && h > 0) {
        // Double-buffered to avoid flicker while the feed updates.
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ old_bmp = SelectObject(mem, bmp);
        paint(mem, rc);
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_bmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(&ps);
    return 0;
}

void activity_window::paint(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(m_bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    const int pad = MulDiv(8, dpi, 96);
    const int gap = MulDiv(10, dpi, 96);
    HGDIOBJ old_font = SelectObject(dc, m_font);
    TEXTMETRIC tm = {};
    GetTextMetrics(dc, &tm);
    const int line_h = tm.tmHeight + MulDiv(2, dpi, 96);

    const auto st = mbdb_activity::stats();
    const int x = pad, right = rc.right - pad;
    int y = pad;
    for (int i = 2; i < 9; ++i) m_btn[i] = RECT(); // only the artists / now playing tabs have these buttons
    m_sb_track = RECT(); // set again by draw_scrollbar() when the list is long enough to scroll
    m_sb_thumb = RECT();

    if (m_np_only) { // standalone "Now playing" panel: no title row, status line or tabs
        m_btn[0] = RECT();
        m_btn[1] = RECT();
        paint_nowplaying(dc, rc, x, right, y, line_h, gap, pad, dpi);
        SelectObject(dc, old_font);
        return;
    }

    // Buttons, right-aligned on the title row
    {
        const std::wstring labels[2] = { L"Search selected", L"Cancel queued" };
        const int padx = MulDiv(10, dpi, 96);
        const int w1 = text_width(dc, labels[1]) + 2 * padx;
        const int w0 = text_width(dc, labels[0]) + 2 * padx;
        m_btn[1] = { right - w1, y, right, y + line_h };
        m_btn[0] = { m_btn[1].left - gap / 2 - w0, y, m_btn[1].left - gap / 2, y + line_h };
        for (int i = 0; i < 2; ++i) {
            const bool enabled = button_enabled(i);
            const RECT& r = m_btn[i];
            const int pct = !enabled ? 8 : (m_down == i && m_hot == i ? 32 : (m_hot == i ? 22 : 12));
            HBRUSH fill = CreateSolidBrush(mix(m_fg, m_bg, pct));
            FillRect(dc, &r, fill);
            DeleteObject(fill);
            HBRUSH border = CreateSolidBrush(mix(m_fg, m_bg, enabled ? 35 : 18));
            FrameRect(dc, &r, border);
            DeleteObject(border);
            SetTextColor(dc, enabled ? m_fg : m_dim);
            RECT t = r;
            DrawTextW(dc, labels[i].c_str(), static_cast<int>(labels[i].size()), &t,
                DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_CENTER);
        }
    }

    // Title
    SelectObject(dc, m_bold ? m_bold : m_font);
    draw_text(dc, x, y, m_btn[0].left - gap, line_h, L"MusicBrainz DB", m_accent);
    SelectObject(dc, m_font);
    y += line_h;

    // Status line
    if (st.searching) {
        const int dots = static_cast<int>((GetTickCount64() / 400) % 4);
        const std::wstring head = L"Searching" + std::wstring(static_cast<size_t>(dots), L'.');
        const int head_w = text_width(dc, L"Searching...") + gap;
        draw_text(dc, x, y, right, line_h, head, m_accent);
        std::wstring what = widen(st.current);
        if (!st.step.empty()) what += L"  -  " + widen(st.step);
        draw_text(dc, x + head_w, y, right, line_h, what, m_fg);
    } else if (!st.task.empty()) {
        const int dots = static_cast<int>((GetTickCount64() / 400) % 4);
        const int head_w = text_width(dc, L"Working...") + gap;
        draw_text(dc, x, y, right, line_h, L"Working" + std::wstring(static_cast<size_t>(dots), L'.'), m_accent);
        draw_text(dc, x + head_w, y, right, line_h, widen(st.task), m_fg);
    } else {
        const std::wstring last = st.last_result.empty() ? std::wstring(L"Idle") : L"Idle  -  last: " + widen(st.last_result);
        draw_text(dc, x, y, right, line_h, last, m_dim);
    }
    y += line_h + MulDiv(4, dpi, 96);

    // "Search selected" progress bar
    m_batch_was_visible = st.batch_visible;
    if (st.batch_visible) {
        const bool finished = st.batch_done >= st.batch_total;
        const double frac = st.batch_total ? static_cast<double>(st.batch_done) / static_cast<double>(st.batch_total) : 0.0;
        wchar_t pct[16];
        swprintf_s(pct, L"%.0f%%", frac * 100.0);
        std::wstring label = format_count(st.batch_done) + L" / " + format_count(st.batch_total) + L"  (" + pct + L")";
        if (st.batch_failed) label += L", " + format_count(st.batch_failed) + L" failed";
        if (finished) {
            label += st.batch_cancelled ? L"  - cancelled" : L"  - done in " + format_duration(st.batch_elapsed_ms);
        } else if (st.batch_done > 0) {
            const long long eta = st.batch_elapsed_ms / static_cast<long long>(st.batch_done) *
                static_cast<long long>(st.batch_total - st.batch_done);
            label += L"  ~" + format_duration(eta) + L" left";
        }
        const int label_w = text_width(dc, label);
        const int inset = line_h / 4;
        const int bar_right = (std::max)(x + MulDiv(40, dpi, 96), right - label_w - gap);
        RECT bar = { x, y + inset, bar_right, y + line_h - inset };
        HBRUSH track = CreateSolidBrush(mix(m_fg, m_bg, 14));
        FillRect(dc, &bar, track);
        DeleteObject(track);
        RECT fill = bar;
        fill.right = bar.left + static_cast<LONG>(static_cast<double>(bar.right - bar.left) * frac);
        if (fill.right > fill.left) {
            HBRUSH fb = CreateSolidBrush(finished && !st.batch_cancelled ? m_good : m_accent);
            FillRect(dc, &fill, fb);
            DeleteObject(fb);
        }
        HBRUSH border = CreateSolidBrush(mix(m_fg, m_bg, 35));
        FrameRect(dc, &bar, border);
        DeleteObject(border);
        draw_text(dc, bar_right + gap, y, right, line_h, label, m_fg);
        y += line_h + MulDiv(4, dpi, 96);
    }

    // The search box belongs to the artists tab, the Rich Edit to the now-playing tab.
    if (m_view != 1 && m_search.IsWindow() && m_search.IsWindowVisible()) {
        m_search.ShowWindow(SW_HIDE);
        m_search_applied = RECT();
    }
    if (m_view != 2 && m_info.IsWindow() && m_info.IsWindowVisible()) {
        m_webview.show(false);
		m_info.ShowWindow(SW_HIDE);
        m_info_applied = RECT();
    }

    // Tabs: Activity | Artists | Now playing
    paint_tabs(dc, x, right, y, line_h, gap, dpi, st.db_artists);
    if (m_view == 2) {
        paint_nowplaying(dc, rc, x, right, y, line_h, gap, pad, dpi);
        SelectObject(dc, old_font);
        return;
    }
    if (m_view == 1) {
        paint_artists(dc, rc, x, right, y, line_h, gap, pad, dpi);
        SelectObject(dc, old_font);
        return;
    }

    // Statistics
    struct row_t { std::wstring label, value; COLORREF colour; };
    std::vector<row_t> rows;
    rows.push_back({ L"Artists in database", st.db_artists >= 0 ? format_count(static_cast<unsigned long long>(st.db_artists)) : L"-", m_fg });
    rows.push_back({ L"Database size", m_db_size ? format_bytes(m_db_size) : std::wstring(L"-"), m_fg });
    rows.push_back({ L"Lookups",
        format_count(st.lookups_ok) + L" ok, " + format_count(st.lookups_failed) + L" failed, " +
        format_count(st.lookups_started) + L" started",
        st.lookups_failed ? m_warn : m_fg });
    rows.push_back({ L"Queued lookups", format_count(st.queued), st.queued ? m_accent : m_fg });
    {
        std::wstring value = L"-";
        const unsigned long long total = st.cache_hits + st.cache_misses;
        if (total) {
            wchar_t pct[32];
            swprintf_s(pct, L"%.0f%%", 100.0 * static_cast<double>(st.cache_hits) / static_cast<double>(total));
            value = format_count(st.cache_hits) + L" hits, " + format_count(st.cache_misses) + L" misses (" + pct + L" hit rate)";
        }
        rows.push_back({ L"Artist cache", value, m_fg });
    }
    rows.push_back({ L"HTTP requests",
        format_count(st.http_requests) + L", " + format_count(st.http_failures) + L" failed, " +
        format_count(st.http_throttled) + L" throttled",
        st.http_throttled ? m_warn : m_fg });
    {
        std::wstring value = L"-";
        if (st.http_requests) {
            wchar_t buf[64];
            swprintf_s(buf, L"status %u, avg %.0f ms", st.last_http_status, st.avg_http_ms);
            value = buf;
        }
        rows.push_back({ L"Last request", value, m_fg });
    }

    int label_w = 0;
    for (const auto& r : rows) label_w = (std::max)(label_w, text_width(dc, r.label));
    const int value_x = x + label_w + gap;
    for (const auto& r : rows) {
        if (y + line_h > rc.bottom) break;
        draw_text(dc, x, y, value_x, line_h, r.label, m_dim);
        draw_text(dc, value_x, y, right, line_h, r.value, r.colour);
        y += line_h;
    }

    // Separator
    y += gap / 2;
    {
        HPEN pen = CreatePen(PS_SOLID, 1, mix(m_fg, m_bg, 25));
        HGDIOBJ old_pen = SelectObject(dc, pen);
        MoveToEx(dc, x, y, nullptr);
        LineTo(dc, right, y);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    y += gap / 2;

    // Events header
    const size_t total_events = mbdb_activity::event_count();
    SelectObject(dc, m_bold ? m_bold : m_font);
    draw_text(dc, x, y, right, line_h, L"Events (" + format_count(total_events) + L")", m_accent);
    SelectObject(dc, m_font);
    y += line_h;

    // Events, newest first, scrollable with the mouse wheel
    const int avail = rc.bottom - pad - y;
    const size_t visible = avail > 0 ? static_cast<size_t>(avail / line_h) : 0;
    m_max_scroll = total_events > visible ? total_events - visible : 0;
    if (m_scroll > m_max_scroll) m_scroll = m_max_scroll;

    // Leave room for the scrollbar when the feed is longer than the panel.
    const int sb_width = MulDiv(12, dpi, 96), sb_gap = MulDiv(2, dpi, 96);
    const bool need_sb = total_events > visible;
    const int row_right = need_sb ? right - sb_width - sb_gap : right;
    const int list_top = y;

    if (visible > 0) {
        const auto evs = mbdb_activity::events(m_scroll, visible);
        const int time_w = text_width(dc, L"00:00:00") + gap / 2;
        for (const auto& e : evs) {
            COLORREF colour = m_fg;
            switch (e.level) {
            case mbdb_activity::lvl_good: colour = m_good; break;
            case mbdb_activity::lvl_warn: colour = m_warn; break;
            case mbdb_activity::lvl_error: colour = m_error; break;
            default: break;
            }
            draw_text(dc, x, y, x + time_w, line_h, format_time(e), m_dim);
            draw_text(dc, x + time_w, y, row_right, line_h, widen(e.text), colour);
            y += line_h;
        }
        if (m_scroll > 0) {
            // Hint that newer events are above the current view.
            const std::wstring hint = L"scrolled - " + format_count(m_scroll) + L" newer";
            const int hw = text_width(dc, hint);
            draw_text(dc, row_right - hw, rc.bottom - pad - line_h, row_right, line_h, hint, m_dim);
        }
    }
    if (need_sb && visible > 0)
        draw_scrollbar(dc, right - sb_width, list_top, rc.bottom - pad, dpi, total_events, visible, m_scroll);
    SelectObject(dc, old_font);
}
