// Owner-drawn vertical scrollbar for the panel's events list and artists table.
// Drawn from the panel's own colours, so it follows light and dark schemes.
#include "activity_window.h"
#include "activity_util.h"
#include <windowsx.h>
#include <algorithm>

using namespace actutil;

size_t activity_window::scroll_max() const {
    return m_view == 1 ? m_amax_scroll : m_view == 3 ? m_pmax_scroll : m_max_scroll;
}

void activity_window::set_scroll(long long offset) {
    offset = (std::max)(0LL, (std::min)(offset, static_cast<long long>(scroll_max())));
    size_t& target = m_view == 1 ? m_ascroll : m_view == 3 ? m_pscroll : m_scroll;
    if (static_cast<size_t>(offset) != target) {
        target = static_cast<size_t>(offset);
        Invalidate(FALSE);
    }
}

// Draws the track and thumb in [left, left + width) x [top, bottom) and remembers
// their rectangles for hit-testing. `offset` is the index of the first visible row.
void activity_window::draw_scrollbar(HDC dc, int left, int top, int bottom, int dpi,
                                     size_t total, size_t visible, size_t offset) {
    m_sb_track = RECT();
    m_sb_thumb = RECT();
    if (total <= visible || visible == 0 || bottom <= top) return;

    const int width = MulDiv(12, dpi, 96);
    const int track_h = bottom - top;
    const size_t max_scroll = total - visible;
    const int min_thumb = (std::min)(track_h, MulDiv(24, dpi, 96));
    int thumb_h = static_cast<int>(static_cast<double>(track_h) * static_cast<double>(visible) / static_cast<double>(total));
    thumb_h = (std::min)(track_h, (std::max)(min_thumb, thumb_h));
    const int travel = track_h - thumb_h;
    const int thumb_top = top + static_cast<int>(
        static_cast<double>(travel) * static_cast<double>((std::min)(offset, max_scroll)) / static_cast<double>(max_scroll));

    m_sb_track = { left, top, left + width, bottom };
    m_sb_thumb = { left, thumb_top, left + width, thumb_top + thumb_h };
    m_sb_total = total;
    m_sb_visible = visible;

    HBRUSH track = CreateSolidBrush(mix(m_fg, m_bg, 7));
    FillRect(dc, &m_sb_track, track);
    DeleteObject(track);

    const int inset = (std::max)(1, MulDiv(2, dpi, 96));
    RECT thumb = { m_sb_thumb.left + inset, m_sb_thumb.top + 1, m_sb_thumb.right - inset, m_sb_thumb.bottom - 1 };
    const bool active = m_sb_dragging || m_sb_hot;
    HBRUSH fill = CreateSolidBrush(mix(m_fg, m_bg, active ? 55 : 35));
    FillRect(dc, &thumb, fill);
    DeleteObject(fill);
}

bool activity_window::scrollbar_mouse_down(POINT pt) {
    if (!PtInRect(&m_sb_track, pt)) return false;
    if (PtInRect(&m_sb_thumb, pt)) {
        m_sb_dragging = true;
        m_sb_grab = pt.y - m_sb_thumb.top; // keep the point you grabbed under the cursor
        SetCapture();
        Invalidate(FALSE);
        return true;
    }
    // Clicking the track pages towards the click, like a normal scrollbar.
    const long long page = static_cast<long long>((std::max)(static_cast<size_t>(1), m_sb_visible > 0 ? m_sb_visible - 1 : 1));
    const long long offset = static_cast<long long>(m_view == 1 ? m_ascroll : m_view == 3 ? m_pscroll : m_scroll);
    set_scroll(pt.y < m_sb_thumb.top ? offset - page : offset + page);
    return true;
}

void activity_window::scrollbar_mouse_move(POINT pt) {
    if (!m_sb_dragging) return;
    const int track_h = m_sb_track.bottom - m_sb_track.top;
    const int thumb_h = m_sb_thumb.bottom - m_sb_thumb.top;
    const int travel = track_h - thumb_h;
    if (travel <= 0) return;
    const int thumb_top = (std::max)(m_sb_track.top, (std::min)(pt.y - m_sb_grab, m_sb_track.bottom - thumb_h));
    const double fraction = static_cast<double>(thumb_top - m_sb_track.top) / static_cast<double>(travel);
    set_scroll(static_cast<long long>(fraction * static_cast<double>(scroll_max()) + 0.5));
}

void activity_window::scrollbar_mouse_up() {
    if (!m_sb_dragging) return;
    m_sb_dragging = false;
    ReleaseCapture();
    Invalidate(FALSE);
}

LRESULT activity_window::OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL& handled) {
    // Capture lost (another window took it, Alt+Tab, ...): never stay stuck in a drag.
    if (m_sb_dragging) { m_sb_dragging = false; Invalidate(FALSE); }
    handled = FALSE;
    return 0;
}
