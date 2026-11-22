// The panel's "Now playing" tab: the artist of the playing track, shown with the
// same sections as the artist info window (rendered by artist_info_window.cpp).
#include "activity_window.h"
#include "activity.h"
#include "activity_util.h"
#include "artist_info_view.h"
#include "artist_table.h"
#include "bg_tasks.h"
#include <memory>
#include <algorithm>

using namespace actutil;

// Used by the standalone "Now playing" panels (Default UI element / Columns UI panel).
void activity_window::show_now_playing_only() {
    m_np_only = true;
    m_view = 2;
    m_np_dirty = true;
    if (IsWindow()) Invalidate(FALSE);
}

void activity_window::create_info() {
    if (m_info.IsWindow()) return;
    m_info.Attach(mbdb_info_create_richedit(m_hWnd)); // null when Rich Edit is unavailable
}

void activity_window::position_info(const RECT& r) {
    if (!m_info.IsWindow()) return;
    const bool editing = m_host && m_host->is_edit_mode(); // never over the host's edit-mode clicks
    if (EqualRect(&r, &m_info_applied) && (editing || m_info.IsWindowVisible())) return;
    m_info_applied = r;
    m_info.SetWindowPos(nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOZORDER | SWP_NOACTIVATE | (editing ? 0 : SWP_SHOWWINDOW));
}

// Called from the window timer. Follows the playing track and loads its stored
// record off the UI thread.
bool activity_window::poll_nowplaying() {
    bool changed = false;
    const bool state_changed = mbdb_activity::now_playing_revision() != m_np_revision;
    const uint64_t db_rev = mbdb_activity::db_revision();

    // Also retry when the database changed while we are waiting for a record that was missing.
    const bool retry = !state_changed && m_np_state == mbdb_activity::np_found &&
        !m_np_has_record && !m_np_fetch && db_rev != m_np_db_revision;

    if (state_changed || retry) {
        const auto np = mbdb_activity::now_playing();
        m_np_revision = np.revision;
        m_np_db_revision = db_rev;
        m_np_state = np.state;
        m_np_label = np.label;
        m_np_mbid = np.mbid;
        if (state_changed) { m_np_has_record = false; m_np_content.reset(); }
        m_np_fetch.reset();

        if (np.state == mbdb_activity::np_found && !np.mbid.empty()) {
            auto fetch = std::make_shared<activity_np_fetch>();
            m_np_fetch = fetch;
            const std::string mbid = np.mbid;
            mbdb_run_async([fetch, mbid] {
                artist_record record;
                const bool ok = mbdb_artist_get(mbid, record);

                // Parsing and formatting the record is the slow part: do it here, not on the UI thread.
                std::shared_ptr<const mbdb_info_content> content;
                if (ok) {
                    artist_list_item item;
                    item.mbid = record.mbid;
                    item.name = record.name;
                    item.country = record.country;
                    item.type = record.type;
                    content = mbdb_info_build(item, &record);
                }

                std::lock_guard<std::mutex> lock(fetch->m);
                fetch->record = std::move(record);
                fetch->content = std::move(content);
                fetch->ok = ok;
                fetch->ready = true;
            });
        }
        m_np_dirty = true;
        changed = true;
    }

    if (m_np_fetch) {
        bool ready = false, ok = false;
        artist_record record;
        std::shared_ptr<const mbdb_info_content> content;
        {
            std::lock_guard<std::mutex> lock(m_np_fetch->m);
            if (m_np_fetch->ready) {
                ready = true;
                ok = m_np_fetch->ok;
                record = std::move(m_np_fetch->record);
                content = std::move(m_np_fetch->content);
            }
        }
        if (ready) {
            m_np_fetch.reset();
            m_np_has_record = ok && content;
            if (m_np_has_record) {
                m_np_record = std::move(record);
                m_np_content = std::move(content);
            }
            m_np_dirty = true;
            changed = true;
        }
    }
    return changed;
}

void activity_window::render_nowplaying() {
    if (!m_info.IsWindow()) return;

    if (m_np_state == mbdb_activity::np_found && m_np_has_record && m_np_content) {
        mbdb_info_render(m_info, *m_np_content, m_np_raw, m_dark, m_bg, m_fg, m_font);
        return;
    }

    std::string message;
    switch (m_np_state) {
    case mbdb_activity::np_idle:
        message = m_np_label.empty() ? "Nothing is playing." : m_np_label;
        break;
    case mbdb_activity::np_stopped:
        break; // blank: the first row says "Now Playing : Stopped"
    case mbdb_activity::np_searching:
        message = "Looking up the artist of " + m_np_label + "...";
        break;
    case mbdb_activity::np_not_found:
        message = m_np_label;
        break;
    case mbdb_activity::np_found:
        message = m_np_fetch ? "Loading the stored artist record..."
                             : "There is no stored record for this artist.";
        break;
    }
    mbdb_info_render_message(m_info, message, m_dark, m_bg, m_fg, m_font);
}

// Link clicks inside the Rich Edit arrive as WM_NOTIFY to the panel.
LRESULT activity_window::OnNotify(
    UINT,
    WPARAM,
    LPARAM lp,
    BOOL& handled
) {
    auto* hdr = reinterpret_cast<LPNMHDR>(lp);

    if (hdr &&
        m_info.IsWindow() &&
        hdr->hwndFrom == m_info.m_hWnd &&
        hdr->code == EN_LINK) {

        const std::wstring url = mbdb_info_get_link(hdr);

        if (!url.empty())
            m_webview.navigate(url);

        return 0;
    }

    handled = FALSE;
    return 0;
}

void activity_window::paint_nowplaying(HDC dc, const RECT& rc, int x, int right, int& y, int line_h,
                                       int gap, int pad, int dpi) {
    (void)gap;
    const int row_gap = MulDiv(4, dpi, 96);

    // Row 1: Now Playing : <artist>
    std::wstring status;
    COLORREF colour = m_dim;
    switch (m_np_state) {
    case mbdb_activity::np_idle:      status = widen(m_np_label); break;
    case mbdb_activity::np_stopped:   status = L"Now Playing : Stopped"; break;
    case mbdb_activity::np_searching: status = L"Looking up: " + widen(m_np_label); colour = m_accent; break;
    case mbdb_activity::np_found:
        // The stored artist name once the record is loaded; until then what is being looked up.
        status = L"Now Playing : " + widen(m_np_has_record && !m_np_record.name.empty() ? m_np_record.name : m_np_label);
        colour = m_fg;
        break;
    case mbdb_activity::np_not_found: status = widen(m_np_label); colour = m_warn; break;
    }
    draw_text(dc, x, y, right, line_h, status, colour);
    y += line_h + row_gap;

    // Row 2: [Show raw JSON]
    const int padx = MulDiv(10, dpi, 96);
    const int bw = (std::max)(text_width(dc, L"Show raw JSON"), text_width(dc, L"Hide raw JSON")) + 2 * padx;
    m_btn[5] = { x, y, x + bw, y + line_h };
    draw_button(dc, 5, m_np_raw ? L"Hide raw JSON" : L"Show raw JSON");
    y += line_h + row_gap;

const int available_width = (std::max)(0, right - x);
const int np_gap = MulDiv(8, dpi, 96);
const int split = available_width * 30 / 100;

const int left_right = (std::max)(x, x + split - np_gap / 2);
const int right_left = (std::min)(right, x + split + np_gap / 2);

const RECT info = {
    x,
    y,
    left_right,
    rc.bottom - pad
};

const RECT webpage = {
    right_left,
    y,
    right,
    rc.bottom - pad
};

const int min_width = MulDiv(20, dpi, 96);
const int min_height = MulDiv(20, dpi, 96);

const bool valid_info =
    info.right - info.left > min_width &&
    info.bottom - info.top > min_height;

const bool valid_webpage =
    webpage.right - webpage.left > min_width &&
    webpage.bottom - webpage.top > min_height;

if (m_info.IsWindow()) {
    if (valid_info)
        position_info(info);
}
else if (valid_info) {
    // Keep your existing unavailable-Rich-Edit message here,
    // but draw it using the left-hand "info" rectangle.
}

if (valid_webpage) {
    m_webview.set_bounds(webpage);

    const bool editing = m_host && m_host->is_edit_mode();
    m_webview.show(!editing && m_view == 2);
}
else {
    m_webview.show(false);
}
}
