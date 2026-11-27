// The panel's "Artists" tab: a table of the artists in the database with
// multi-select, delete and JSON export.
#include "activity_window.h"
#include "artist_info_window.h"
#include "activity.h"
#include "activity_util.h"
#include "artist_table.h"
#include "bg_tasks.h"
#include "lookup_queue.h"
#include "listenbrainz_similar_artists.h"
#include "preferences.h"
#include <windowsx.h>
#include <commdlg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_set>
#include <nlohmann/json.hpp>

using namespace actutil;

namespace {
bool ci_less(const std::string& a, const std::string& b) {
    const size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}
} // namespace

// ------------------------------------------------------------------ data ---

size_t activity_window::selected_count() const {
    size_t n = 0; // only rows that pass the filter count (hidden rows are never selected)
    for (size_t id : m_order) if (id < m_sel.size() && m_sel[id]) ++n;
    return n;
}

std::vector<std::string> activity_window::selected_mbids() const {
    std::vector<std::string> out;
    for (size_t pos = 0; pos < m_order.size(); ++pos) {
        const size_t id = m_order[pos];
        if (id < m_sel.size() && m_sel[id]) out.push_back(m_rows[id].mbid);
    }
    return out;
}

void activity_window::rebuild_haystack() {
    m_haystack.resize(m_rows.size());
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_haystack[i] = mbdb_search::make_row(m_rows[i]);
}

// Applies the filter and the sort order to build the display order.
void activity_window::sort_artists() {
    m_order.clear();
    std::vector<char> visible(m_rows.size(), 0);
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_query.empty() || (i < m_haystack.size() && mbdb_search::matches(m_query, m_haystack[i]))) {
            m_order.push_back(i);
            visible[i] = 1;
        }
    }
    // Rows hidden by the filter are deselected, so delete/export never touch what you cannot see.
    for (size_t i = 0; i < m_sel.size() && i < visible.size(); ++i) if (!visible[i]) m_sel[i] = 0;

    const int col = m_sort_col;
    const bool asc = m_sort_asc;
    std::stable_sort(m_order.begin(), m_order.end(), [&](size_t a, size_t b) {
        const artist_list_item& x = m_rows[a];
        const artist_list_item& y = m_rows[b];
        auto key = [col](const artist_list_item& a) -> const std::string& {
            switch (col) {
            case 0: return a.name;
            case 1: return a.type;
            case 2: return a.country;
            case 3: return a.area;
            case 4: return a.begin;
            case 5: return a.end;
            default: return a.mbid;
            }
        };
        const std::string& kx = key(x);
        const std::string& ky = key(y);
        if ((col == 4 || col == 5) && kx.empty() != ky.empty()) return ky.empty(); // no date: last, either way
        if (ci_less(kx, ky)) return asc;
        if (ci_less(ky, kx)) return !asc;
        return ci_less(x.mbid, y.mbid); // stable tie-break
    });
    m_focus = m_anchor = -1;
    request_similar_artists();
    if (m_ascroll > m_order.size()) m_ascroll = 0;
}

// Listing happens on the background thread; the window's timer installs the result.
void activity_window::reload_artists() {
    if (m_reload) return; // one is already running; the timer asks again if the database moved on
    auto state = std::make_shared<activity_reload_state>();
    state->revision = mbdb_activity::db_revision(); // read first: a change during the load triggers another reload
    m_reload = state;
    mbdb_run_async([state] {
        std::vector<artist_list_item> rows;
        mbdb_artist_list(rows);
        std::lock_guard<std::mutex> lock(state->m);
        state->rows = std::move(rows);
        state->ready = true;
    });
}

bool activity_window::poll_reload() {
    if (!m_reload) return false;
    std::vector<artist_list_item> rows;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(m_reload->m);
        if (!m_reload->ready) return false;
        rows = std::move(m_reload->rows);
        revision = m_reload->revision;
    }
    m_reload.reset();

    std::set<std::string> keep; // keep the selection across reloads
    for (size_t i = 0; i < m_rows.size() && i < m_sel.size(); ++i) if (m_sel[i]) keep.insert(m_rows[i].mbid);

    m_rows.swap(rows);
    m_rows_revision = revision;
    m_sel.assign(m_rows.size(), 0);
    for (size_t i = 0; i < m_rows.size(); ++i) if (keep.count(m_rows[i].mbid)) m_sel[i] = 1;
    rebuild_haystack();
    sort_artists();
    m_rows_loaded = true;
    return true;
}

void activity_window::select_range(long long a, long long b, bool clear_first) {
    if (clear_first) std::fill(m_sel.begin(), m_sel.end(), 0);
    if (a > b) std::swap(a, b);
    for (long long p = a; p <= b; ++p)
        if (p >= 0 && p < static_cast<long long>(m_order.size())) m_sel[m_order[static_cast<size_t>(p)]] = 1;
}

void activity_window::ensure_visible(long long pos) {
    if (pos < 0 || m_visible_rows == 0) return;
    const size_t p = static_cast<size_t>(pos);
    if (p < m_ascroll) m_ascroll = p;
    else if (p >= m_ascroll + m_visible_rows) m_ascroll = p - m_visible_rows + 1;
}

// ------------------------------------------------ ListenBrainz Similar Artists ---

void activity_window::request_similar_artists() {
    if (m_focus < 0 || static_cast<size_t>(m_focus) >= m_order.size()) {
        m_similar_requested_mbid.clear();
        m_similar_rows.clear();
        m_similar_error.clear();
        m_similar_loading = false;
        m_similar_fetch.reset();
        return;
    }
    const size_t row_id = m_order[static_cast<size_t>(m_focus)];
    if (row_id >= m_rows.size()) return;
    const std::string mbid = m_rows[row_id].mbid;
    if (mbid.empty() || mbid == m_similar_requested_mbid) return;

    m_similar_requested_mbid = mbid;
    m_similar_rows.clear();
    m_similar_error.clear();
    m_similar_scroll = 0;
    m_similar_selected = -1;
    m_similar_loading = true;
    auto state = std::make_shared<activity_similar_artists_state>();
    state->artist_mbid = mbid;
    m_similar_fetch = state;
    const std::string algorithm = mbdb_prefs::listenbrainz_similar_artists_algorithm();
    mbdb_run_async([state, mbid, algorithm] {
        listenbrainz_similar_artists provider;
        std::vector<listenbrainz_similar_artist> results;
        std::string error;
        const bool ok = provider.fetch(mbid, algorithm, results, &error);
        std::lock_guard<std::mutex> lock(state->m);
        state->ok = ok;
        state->artists = std::move(results);
        state->error = std::move(error);
        state->ready = true;
    });
}

std::vector<std::string> activity_window::similar_missing_mbids() const {
    std::vector<std::string> out;
    if (m_similar_rows.empty()) return out;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::unordered_set<std::string> stored;
    stored.reserve(m_rows.size());
    for (const auto& r : m_rows) stored.insert(lower(r.mbid));
    std::unordered_set<std::string> seen;
    for (const auto& a : m_similar_rows) {
        if (a.mbid.empty()) continue;
        const std::string id = lower(a.mbid);
        if (stored.count(id) || !seen.insert(id).second) continue;
        out.push_back(a.mbid);
    }
    return out;
}

// Queues every similar artist that is not stored yet; the lookup queue fetches them one
// at a time, obeying the MusicBrainz rate limit, and the table reloads as they arrive.
void activity_window::download_similar_artists() {
    const auto mbids = similar_missing_mbids();
    if (mbids.empty()) return;
    mbdb_search_artist_mbids(mbids);
    Invalidate(FALSE);
}

// For every selected artist: fetch its ListenBrainz similar artists (on the background
// thread), then queue the ones that are not stored yet for download.
void activity_window::download_similar_of_selected() {
    const auto mbids = selected_mbids();
    if (mbids.empty()) return;
    const std::string algorithm = mbdb_prefs::listenbrainz_similar_artists_algorithm();
    mbdb_run_async([mbids, algorithm] {
        mbdb_activity::task_scope scope(("Fetching similar artists of " + std::to_string(mbids.size()) + " artist(s)...").c_str());
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::unordered_set<std::string> known; // stored artists, plus ones already collected
        std::vector<artist_list_item> stored;
        mbdb_artist_list(stored);
        for (const auto& r : stored) known.insert(lower(r.mbid));

        listenbrainz_similar_artists provider;
        std::vector<std::string> wanted;
        size_t failed = 0;
        for (const auto& mbid : mbids) {
            std::vector<listenbrainz_similar_artist> results;
            std::string error;
            if (!provider.fetch(mbid, algorithm, results, &error)) { ++failed; continue; }
            for (const auto& a : results) {
                if (a.mbid.empty()) continue;
                if (known.insert(lower(a.mbid)).second) wanted.push_back(a.mbid);
            }
        }
        char text[200];
        snprintf(text, sizeof(text), "Similar artists of %zu artist(s): %zu new, %zu request(s) failed",
            mbids.size(), wanted.size(), failed);
        mbdb_activity::add_event(failed ? mbdb_activity::lvl_warn : mbdb_activity::lvl_info, text);
        if (!wanted.empty()) mbdb_search_artist_mbids(wanted);
    });
}

bool activity_window::poll_similar_artists() {
    if (!m_similar_fetch) return false;
    std::vector<listenbrainz_similar_artist> results;
    std::string error, mbid;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_similar_fetch->m);
        if (!m_similar_fetch->ready) return false;
        results = std::move(m_similar_fetch->artists);
        error = std::move(m_similar_fetch->error);
        mbid = m_similar_fetch->artist_mbid;
        ok = m_similar_fetch->ok;
    }
    m_similar_fetch.reset();
    // Ignore a late response if the user selected a different artist meanwhile.
    if (mbid != m_similar_requested_mbid) return false;
    m_similar_loading = false;
    m_similar_rows = std::move(results);
    m_similar_error = ok ? std::string() : (error.empty() ? "ListenBrainz request failed" : error);
    return true;
}

void activity_window::paint_similar_artists(HDC dc, const RECT& pane, int line_h, int gap, int dpi) {
    m_similar_row_top = 0;
    m_similar_row_height = 0;
    m_similar_visible_rows = 0;
    m_similar_max_scroll = 0;
    m_similar_sb_track = RECT();
    m_similar_sb_thumb = RECT();
    m_btn[6] = RECT();
    if (pane.right <= pane.left) return;

    HPEN pen = CreatePen(PS_SOLID, 1, mix(m_fg, m_bg, 25));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, pane.left, pane.top, nullptr);
    LineTo(dc, pane.left, pane.bottom);
    SelectObject(dc, old_pen);
    DeleteObject(pen);

    const int x = pane.left + gap;
    const int right = pane.right - gap;
    int y = pane.top + MulDiv(3, dpi, 96);
    SelectObject(dc, m_bold ? m_bold : m_font);
    draw_text(dc, x, y, right, line_h, L"Similar artists · ListenBrainz", m_accent);
    SelectObject(dc, m_font);
    y += line_h + MulDiv(5, dpi, 96);

    if (!m_similar_requested_mbid.empty() && !m_similar_loading && m_similar_error.empty() && !m_similar_rows.empty()) {
        const size_t missing = similar_missing_mbids().size();
        const std::wstring label = missing > 0
            ? L"Download similar (" + format_count(missing) + L")"
            : std::wstring(L"All similar artists stored");
        const int padx = MulDiv(10, dpi, 96);
        const int w = (std::min)(text_width(dc, label) + 2 * padx, static_cast<int>((std::max)(0L, static_cast<LONG>(right - x))));
        m_btn[6] = { x, y, x + w, y + line_h };
        draw_button(dc, 6, label);
        y += line_h + MulDiv(5, dpi, 96);
    }

    std::wstring status;
    if (m_similar_requested_mbid.empty()) status = L"Select an artist to find similar artists.";
    else if (m_similar_loading) status = L"Loading similar artists...";
    else if (!m_similar_error.empty()) status = widen(m_similar_error);
    else if (m_similar_rows.empty()) status = L"No similar artists were returned.";
    if (!status.empty()) {
        RECT r = { x, y, right, pane.bottom - gap };
        DrawTextW(dc, status.c_str(), static_cast<int>(status.size()), &r,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        return;
    }

    const int row_h = line_h + MulDiv(3, dpi, 96);
    m_similar_row_top = y;
    m_similar_row_height = row_h;
    const int sbw = MulDiv(12, dpi, 96);
    const int sbgap = MulDiv(2, dpi, 96);
    const int track_left = (std::max)(x, right - sbw);
    const int rows_right = m_similar_rows.size() > 0 ? track_left - sbgap : right;
    const int avail = static_cast<int>((std::max)(0L, static_cast<LONG>(pane.bottom - gap - y)));
    m_similar_visible_rows = static_cast<size_t>(avail / row_h);
    m_similar_max_scroll = m_similar_rows.size() > m_similar_visible_rows
        ? m_similar_rows.size() - m_similar_visible_rows : 0;
    if (m_similar_scroll > m_similar_max_scroll) m_similar_scroll = m_similar_max_scroll;

    const size_t count = (std::min)(m_similar_visible_rows, m_similar_rows.size() - (std::min)(m_similar_scroll, m_similar_rows.size()));
    // ListenBrainz scores are often large whole numbers (hundreds of thousands), sometimes
    // fractions: show whole numbers without decimals, and size the column from the widest score
    // so it is never cut off with "...".
    auto format_score = [](double v, wchar_t* out, size_t size) {
        if (std::fabs(v - std::round(v)) < 1e-9 || std::fabs(v) >= 1000.0) swprintf_s(out, size, L"%.0f", v);
        else swprintf_s(out, size, L"%.3f", v);
    };
    int score_w = text_width(dc, L"0.000") + gap;
    for (const auto& r : m_similar_rows) {
        wchar_t text[48] = {};
        format_score(r.score, text, 48);
        score_w = (std::max)(score_w, text_width(dc, text) + gap);
    }
    score_w = (std::min)(score_w, (std::max)(1, static_cast<int>(rows_right - x) * 2 / 5));
    for (size_t i = 0; i < count; ++i) {
        const size_t index = m_similar_scroll + i;
        const auto& a = m_similar_rows[index];
        const int ry = y + static_cast<int>(i) * row_h;
        RECT row = { x, ry, rows_right, ry + row_h };
        if (static_cast<long long>(index) == m_similar_selected) {
            HBRUSH selected = CreateSolidBrush(mix(m_accent, m_bg, 35));
            FillRect(dc, &row, selected);
            DeleteObject(selected);
        }
        draw_text(dc, x + gap / 2, ry, rows_right - score_w, line_h, widen(a.name), m_fg);
        wchar_t score[48] = {};
        format_score(a.score, score, 48);
        draw_text(dc, rows_right - score_w, ry, rows_right, line_h, score, m_dim);
    }

    if (m_similar_max_scroll > 0 && m_similar_visible_rows > 0 && pane.bottom > y) {
        const int track_h = pane.bottom - gap - y;
        const int min_thumb = (std::min)(track_h, MulDiv(24, dpi, 96));
        int thumb_h = static_cast<int>(static_cast<double>(track_h) * m_similar_visible_rows / m_similar_rows.size());
        thumb_h = (std::min)(track_h, (std::max)(min_thumb, thumb_h));
        const int travel = track_h - thumb_h;
        const int thumb_top = y + static_cast<int>(static_cast<double>(travel) * m_similar_scroll / m_similar_max_scroll);
        m_similar_sb_track = { track_left, y, right, pane.bottom - gap };
        m_similar_sb_thumb = { track_left, thumb_top, right, thumb_top + thumb_h };
        HBRUSH track = CreateSolidBrush(mix(m_fg, m_bg, 7));
        FillRect(dc, &m_similar_sb_track, track);
        DeleteObject(track);
        RECT thumb = m_similar_sb_thumb;
        InflateRect(&thumb, -MulDiv(2, dpi, 96), -1);
        HBRUSH fill = CreateSolidBrush(mix(m_fg, m_bg, m_similar_sb_dragging ? 55 : 35));
        FillRect(dc, &thumb, fill);
        DeleteObject(fill);
    }
}
// --------------------------------------------------------------- actions ---

void activity_window::delete_selected() {
    const auto mbids = selected_mbids();
    if (mbids.empty()) return;
    wchar_t msg[320];
    swprintf_s(msg, L"Delete %zu artist(s) from the database and the JSON files?\n\n"
        L"They will be downloaded again from MusicBrainz if they are looked up later.", mbids.size());
    if (::MessageBoxW(m_hWnd, msg, L"MusicBrainz DB", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;

    // The table reloads by itself once the database revision changes.
    mbdb_run_async([mbids] {
        mbdb_activity::task_scope scope(("Deleting " + std::to_string(mbids.size()) + " artist(s)...").c_str());
        const size_t deleted = mbdb_artist_delete(mbids);
        char text[160];
        snprintf(text, sizeof(text), "Deleted %zu of %zu selected artist(s)", deleted, mbids.size());
        mbdb_activity::add_event(deleted == mbids.size() ? mbdb_activity::lvl_info : mbdb_activity::lvl_warn, text);
    });
}

void activity_window::export_json(bool list_only) {
    const auto mbids = selected_mbids(); // empty = everything
    wchar_t file[MAX_PATH] = {};
    wcscpy_s(file, list_only ? L"musicbrainz_artist_list.json" : L"musicbrainz_artists.json");
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = list_only ? L"Export artist list (name + MBID)" : L"Export artists (full MusicBrainz records)";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    const std::wstring path = file;
    mbdb_run_async([mbids, path, list_only] {
        mbdb_activity::task_scope scope("Exporting artists...");
        size_t count = 0;
        std::string error;
        const bool ok = mbdb_artist_export(mbids, path, list_only, count, error);
        std::string text;
        if (ok) text = "Exported " + std::to_string(count) + (list_only ? " artist(s) (name + MBID) to " : " artist(s) (full records) to ") + narrow(path);
        else text = "Export failed: " + error;
        mbdb_activity::add_event(ok ? mbdb_activity::lvl_good : mbdb_activity::lvl_error, text.c_str());
    });
}

void activity_window::copy_mbids() {
    const auto mbids = selected_mbids();
    if (mbids.empty()) return;
    std::wstring text;
    for (const auto& id : mbids) text += widen(id) + L"\r\n";
    set_clipboard_text(m_hWnd, text);
}

void activity_window::artists_context_menu(POINT screen_pt) {
    POINT client = screen_pt;
    if (screen_pt.x != -1 || screen_pt.y != -1) {
        ScreenToClient(&client);
        // Right-click on an unselected row selects it first, like a list view.
        if (PtInRect(&m_list_rc, client) && m_row_h > 0) {
            const size_t pos = m_ascroll + static_cast<size_t>((client.y - m_list_rc.top) / m_row_h);
            if (pos < m_order.size() && !m_sel[m_order[pos]]) {
                std::fill(m_sel.begin(), m_sel.end(), 0);
                m_sel[m_order[pos]] = 1;
                m_focus = m_anchor = static_cast<long long>(pos);
                request_similar_artists();
                Invalidate(FALSE);
            }
        }
    } else {
        GetCursorPos(&screen_pt);
    }
    const bool any = selected_count() > 0;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (any ? 0 : MF_GRAYED), 1, L"Delete selected\tDel");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_rows.empty() ? MF_GRAYED : 0), 2,
        any ? L"Export selected (full records)..." : L"Export all (full records)...");
    AppendMenuW(menu, MF_STRING | (m_rows.empty() ? MF_GRAYED : 0), 3,
        any ? L"Export selected list (name + MBID)..." : L"Export all as list (name + MBID)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (any ? 0 : MF_GRAYED), 4, L"Copy MBID(s)\tCtrl+C");
    AppendMenuW(menu, MF_STRING | (m_rows.empty() ? MF_GRAYED : 0), 5, L"Select all\tCtrl+A");
    AppendMenuW(menu, MF_STRING, 6, L"Refresh\tF5");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x, screen_pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case 1: delete_selected(); break;
    case 2: export_json(false); break;
    case 3: export_json(true); break;
    case 4: copy_mbids(); break;
    case 5: select_range(0, static_cast<long long>(m_order.size()) - 1, true); Invalidate(FALSE); break;
    case 6: reload_artists(); break;
    default: break;
    }
}

// Right-click menu of the similar artists pane: acts on the clicked row (which it selects)
// or on the whole list.
void activity_window::similar_context_menu(POINT screen_pt) {
    POINT client = screen_pt;
    ScreenToClient(&client);
    if (m_similar_row_height > 0 && client.y >= m_similar_row_top && !PtInRect(&m_similar_sb_track, client)) {
        const size_t index = m_similar_scroll + static_cast<size_t>((client.y - m_similar_row_top) / m_similar_row_height);
        if (index < m_similar_rows.size()) {
            m_similar_selected = static_cast<long long>(index);
            Invalidate(FALSE);
        }
    }

    const bool has_row = m_similar_selected >= 0 && static_cast<size_t>(m_similar_selected) < m_similar_rows.size();
    const listenbrainz_similar_artist row = has_row ? m_similar_rows[static_cast<size_t>(m_similar_selected)] : listenbrainz_similar_artist();
    bool stored = false;
    if (has_row) {
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string id = lower(row.mbid);
        for (const auto& r : m_rows) if (lower(r.mbid) == id) { stored = true; break; }
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (has_row ? 0 : MF_GRAYED), 1, L"Open artist information");
    AppendMenuW(menu, MF_STRING | (has_row && !stored && !row.mbid.empty() ? 0 : MF_GRAYED), 2,
        stored ? L"Download this artist (already stored)" : L"Download this artist");
    AppendMenuW(menu, MF_STRING | (has_row ? 0 : MF_GRAYED), 3, L"Copy MBID");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_similar_rows.empty() ? MF_GRAYED : 0), 4, L"Export similar artists list...");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x, screen_pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case 1: {
        artist_list_item item;
        item.mbid = row.mbid;
        item.name = row.name;
        item.type = row.type;
        mbdb_show_artist_info(item);
        break;
    }
    case 2:
        mbdb_search_artist_mbids({ row.mbid });
        Invalidate(FALSE);
        break;
    case 3:
        set_clipboard_text(m_hWnd, widen(row.mbid));
        break;
    case 4:
        export_similar_list();
        break;
    default:
        break;
    }
}

// Saves the similar artists shown in the pane (name, MBID, score, ...) as a JSON file.
void activity_window::export_similar_list() {
    if (m_similar_rows.empty()) return;
    wchar_t file[MAX_PATH] = {};
    wcscpy_s(file, L"musicbrainz_similar_artists.json");
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Export similar artists";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    nlohmann::json out;
    out["source_artist_mbid"] = m_similar_requested_mbid;
    out["algorithm"] = mbdb_prefs::listenbrainz_similar_artists_algorithm();
    out["similar_artists"] = nlohmann::json::array();
    for (const auto& a : m_similar_rows) {
        nlohmann::json item;
        item["name"] = a.name;
        item["mbid"] = a.mbid;
        item["score"] = a.score;
        item["type"] = a.type;
        item["gender"] = a.gender;
        item["comment"] = a.comment;
        out["similar_artists"].push_back(std::move(item));
    }

    const std::wstring path = file;
    bool ok = false;
    try {
        std::ofstream stream{ std::filesystem::path(path), std::ios::binary | std::ios::trunc };
        if (stream) {
            stream << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
            ok = static_cast<bool>(stream);
        }
    } catch (...) {
        ok = false;
    }
    const std::string text = ok
        ? "Exported " + std::to_string(m_similar_rows.size()) + " similar artist(s) to " + narrow(path)
        : "Export failed: could not write " + narrow(path);
    mbdb_activity::add_event(ok ? mbdb_activity::lvl_good : mbdb_activity::lvl_error, text.c_str());
}

// ----------------------------------------------------------------- input ---

// Tabs left to right: Activity | Artists | Selected Artist | Now playing. The view numbers
// (0 activity, 1 artists, 2 now playing, 3 selected item) are fixed, so this maps position to view.
static const int k_tab_views[4] = { 0, 1, 3, 2 };

bool activity_window::tab_mouse_down(POINT pt) {
    for (int pos = 0; pos < 4; ++pos) {
        if (!PtInRect(&m_tab[pos], pt)) continue;
        const int i = k_tab_views[pos];
        if (m_view != i) {
            const bool source_changed = (m_view == 3) != (i == 3); // Selected Artist <-> the other views
            m_view = i;
            if (source_changed) reset_info_source();
            // The Selected Artist tab shows a row of the Artists table, so that must be loaded.
            if ((i == 1 || i == 3) && (!m_rows_loaded || m_rows_revision != mbdb_activity::db_revision())) reload_artists();
            if (i == 3) adopt_table_focus();
            if (i == 2 || i == 3) m_np_dirty = true; // render the artist when the tab opens
            Invalidate(FALSE);
        }
        return true;
    }
    return false;
}

bool activity_window::artists_mouse_down(POINT pt) {
    if (m_view != 1) return false;
    SetFocus();
    if (PtInRect(&m_similar_rc, pt)) {
        if (PtInRect(&m_similar_sb_track, pt)) {
            if (PtInRect(&m_similar_sb_thumb, pt)) {
                m_similar_sb_dragging = true;
                m_similar_sb_grab = pt.y - m_similar_sb_thumb.top;
                SetCapture();
            } else {
                const long long page = static_cast<long long>((std::max)(size_t(1), m_similar_visible_rows > 0 ? m_similar_visible_rows - 1 : 1));
                const long long target = static_cast<long long>(m_similar_scroll) + (pt.y < m_similar_sb_thumb.top ? -page : page);
                m_similar_scroll = static_cast<size_t>((std::max)(0LL, (std::min)(target, static_cast<long long>(m_similar_max_scroll))));
            }
            Invalidate(FALSE);
            return true;
        }
        if (m_similar_row_height > 0 && pt.y >= m_similar_row_top) {
            const size_t index = m_similar_scroll + static_cast<size_t>((pt.y - m_similar_row_top) / m_similar_row_height);
            if (index < m_similar_rows.size()) m_similar_selected = static_cast<long long>(index);
            else m_similar_selected = -1;
            Invalidate(FALSE);
            return true;
        }
        return true;
    }
    if (PtInRect(&m_header_rc, pt)) {
        for (int c = 0; c < 7; ++c) {
            if (pt.x >= m_col_x[c] && pt.x < m_col_x[c + 1]) {
                if (m_sort_col == c) m_sort_asc = !m_sort_asc;
                else { m_sort_col = c; m_sort_asc = true; }
                sort_artists();
                m_focus = m_anchor = -1;
                Invalidate(FALSE);
                return true;
            }
        }
        return true;
    }
    if (PtInRect(&m_list_rc, pt) && m_row_h > 0) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        const size_t pos = m_ascroll + static_cast<size_t>((pt.y - m_list_rc.top) / m_row_h);
        if (pos < m_order.size()) {
            const long long p = static_cast<long long>(pos);
            if (shift && m_anchor >= 0) {
                select_range(m_anchor, p, !ctrl);
            } else if (ctrl) {
                m_sel[m_order[pos]] = m_sel[m_order[pos]] ? 0 : 1;
                m_anchor = p;
            } else {
                std::fill(m_sel.begin(), m_sel.end(), 0);
                m_sel[m_order[pos]] = 1;
                m_anchor = p;
            }
            m_focus = p;
        } else if (!ctrl && !shift) {
            std::fill(m_sel.begin(), m_sel.end(), 0); // click below the last row
        }
        request_similar_artists();
        Invalidate(FALSE);
        return true;
    }
    return false;
}

bool activity_window::artists_dbl_click(POINT pt) {
    if (m_view != 1) return false;
    if (PtInRect(&m_similar_rc, pt) && m_similar_row_height > 0 && pt.y >= m_similar_row_top) {
        const size_t index = m_similar_scroll + static_cast<size_t>((pt.y - m_similar_row_top) / m_similar_row_height);
        if (index < m_similar_rows.size() && index < m_similar_scroll + m_similar_visible_rows) {
            const auto& similar = m_similar_rows[index];
            artist_list_item item;
            item.mbid = similar.mbid;
            item.name = similar.name;
            item.type = similar.type;
            mbdb_show_artist_info(item);
            return true;
        }
        return false;
    }
    if (m_row_h <= 0 || !PtInRect(&m_list_rc, pt)) return false;
    const size_t pos = m_ascroll + static_cast<size_t>((pt.y - m_list_rc.top) / m_row_h);
    if (pos >= m_order.size()) return false;
    const size_t id = m_order[pos];
    if (id >= m_rows.size()) return false;
    const artist_list_item item = m_rows[id]; // copy: the window can outlive a list reload
    mbdb_show_artist_info(item);
    return true;
}

void activity_window::artists_wheel(int steps) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(&pt);
    if (PtInRect(&m_similar_rc, pt)) {
        long long target = static_cast<long long>(m_similar_scroll) - static_cast<long long>(steps) * 3;
        target = (std::max)(0LL, (std::min)(target, static_cast<long long>(m_similar_max_scroll)));
        m_similar_scroll = static_cast<size_t>(target);
    } else {
        long long target = static_cast<long long>(m_ascroll) - static_cast<long long>(steps) * 3;
        target = (std::max)(0LL, (std::min)(target, static_cast<long long>(m_amax_scroll)));
        m_ascroll = static_cast<size_t>(target);
    }
    Invalidate(FALSE);
}

LRESULT activity_window::OnKeyDown(UINT, WPARAM wp, LPARAM, BOOL& handled) {
    if (m_view != 1) { handled = FALSE; return 0; }
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    const long long n = static_cast<long long>(m_order.size());
    const long long page = (std::max)(1LL, static_cast<long long>(m_visible_rows) - 1);
    auto move_to = [&](long long pos) {
        if (n == 0) return;
        pos = (std::max)(0LL, (std::min)(pos, n - 1));
        if (shift && m_anchor >= 0) {
            select_range(m_anchor, pos, true);
        } else {
            std::fill(m_sel.begin(), m_sel.end(), 0);
            m_sel[m_order[static_cast<size_t>(pos)]] = 1;
            m_anchor = pos;
        }
        m_focus = pos;
        ensure_visible(pos);
        request_similar_artists();
        Invalidate(FALSE);
    };
    switch (wp) {
    case VK_DELETE: delete_selected(); return 0;
    case VK_UP:     move_to(m_focus < 0 ? 0 : m_focus - 1); return 0;
    case VK_DOWN:   move_to(m_focus < 0 ? 0 : m_focus + 1); return 0;
    case VK_PRIOR:  move_to(m_focus < 0 ? 0 : m_focus - page); return 0;
    case VK_NEXT:   move_to(m_focus < 0 ? 0 : m_focus + page); return 0;
    case VK_HOME:   move_to(0); return 0;
    case VK_END:    move_to(n - 1); return 0;
    case VK_F5:     reload_artists(); return 0;
    case VK_ESCAPE: std::fill(m_sel.begin(), m_sel.end(), 0); m_focus = -1; request_similar_artists(); Invalidate(FALSE); return 0;
    case 'A':
        if (ctrl) { select_range(0, n - 1, true); Invalidate(FALSE); return 0; }
        break;
    case 'C':
        if (ctrl) { copy_mbids(); return 0; }
        break;
    case 'F':
        if (ctrl && m_search.IsWindow()) { m_search.SetFocus(); m_search.SendMessage(EM_SETSEL, 0, -1); return 0; }
        break;
    default: break;
    }
    handled = FALSE;
    return 0;
}

LRESULT activity_window::OnGetDlgCode(UINT, WPARAM, LPARAM, BOOL&) {
    return DLGC_WANTARROWS; // arrow keys scroll/select in the artists table
}

LRESULT activity_window::OnFocusChange(UINT, WPARAM, LPARAM, BOOL& handled) {
    Invalidate(FALSE); // focus rectangle
    handled = FALSE;
    return 0;
}

// ----------------------------------------------------------------- paint ---

void activity_window::draw_button(HDC dc, int index, const std::wstring& label) {
    const bool enabled = button_enabled(index);
    const RECT& r = m_btn[index];
    const int pct = !enabled ? 8 : (m_down == index && m_hot == index ? 32 : (m_hot == index ? 22 : 12));
    HBRUSH fill = CreateSolidBrush(mix(m_fg, m_bg, pct));
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    HBRUSH border = CreateSolidBrush(mix(m_fg, m_bg, enabled ? 35 : 18));
    FrameRect(dc, &r, border);
    DeleteObject(border);
    SetTextColor(dc, enabled ? m_fg : m_dim);
    RECT t = r;
    DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &t, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_CENTER);
}

void activity_window::paint_tabs(HDC dc, int x, int right, int& y, int line_h, int gap, int dpi, long long db_artists) {
    const std::wstring labels[4] = {
        L"Activity",
        db_artists >= 0 ? L"Artists (" + format_count(static_cast<unsigned long long>(db_artists)) + L")" : std::wstring(L"Artists"),
        L"Selected Artist",
        L"Now playing" };
    const int padx = MulDiv(12, dpi, 96);
    int tx = x;
    for (int i = 0; i < 4; ++i) {
        const bool active = m_view == k_tab_views[i];
        SelectObject(dc, active && m_bold ? m_bold : m_font);
        const int w = text_width(dc, labels[i]) + 2 * padx;
        m_tab[i] = { tx, y, tx + w, y + line_h };
        draw_text(dc, tx + padx, y, tx + w, line_h, labels[i], active ? m_accent : m_dim);
        if (active) {
            HBRUSH bar = CreateSolidBrush(m_accent);
            RECT u = { tx, y + line_h - MulDiv(2, dpi, 96), tx + w, y + line_h };
            FillRect(dc, &u, bar);
            DeleteObject(bar);
        }
        tx += w + gap / 2;
    }
    SelectObject(dc, m_font);
    y += line_h;
    HPEN pen = CreatePen(PS_SOLID, 1, mix(m_fg, m_bg, 25));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, x, y, nullptr);
    LineTo(dc, right, y);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    y += MulDiv(4, dpi, 96);
}

void activity_window::paint_artists(HDC dc, const RECT& rc, int x, int right, int& y, int line_h, int gap, int pad, int dpi) {
    // Toolbar
    {
        const std::wstring labels[3] = { L"Delete selected", L"Export JSON...", L"Refresh" };
        const int padx = MulDiv(10, dpi, 96);
        int bx = x;
        for (int i = 0; i < 3; ++i) {
            const int w = text_width(dc, labels[i]) + 2 * padx;
            m_btn[2 + i] = { bx, y, bx + w, y + line_h };
            draw_button(dc, 2 + i, labels[i]);
            bx += w + gap / 2;
        }
        {
            const std::wstring label = L"Download similar";
            const int w = text_width(dc, label) + 2 * padx;
            m_btn[7] = { bx, y, bx + w, y + line_h };
            draw_button(dc, 7, label);
            bx += w + gap / 2;
        }
        std::wstring info = format_count(selected_count()) + L" selected, ";
        info += m_filter.empty() ? format_count(m_rows.size()) + L" total"
                                 : format_count(m_order.size()) + L" of " + format_count(m_rows.size()) + L" shown";
        draw_text(dc, (std::max)(bx + gap, right - text_width(dc, info)), y, right, line_h, info, m_dim);
        y += line_h + MulDiv(4, dpi, 96);
    }

    // Search row: our own frame, with the EDIT control inside it
    {
        const std::wstring label = L"Search";
        const int lw = text_width(dc, label) + gap;
        const int sh = line_h + MulDiv(4, dpi, 96);
        draw_text(dc, x, y, x + lw, sh, label, m_dim);
        RECT frame = { x + lw, y, right, y + sh };
        if (frame.right > frame.left + MulDiv(40, dpi, 96)) {
            HBRUSH fill = CreateSolidBrush(m_edit_bg);
            FillRect(dc, &frame, fill);
            DeleteObject(fill);
            HBRUSH border = CreateSolidBrush(mix(m_fg, m_bg, 35));
            FrameRect(dc, &frame, border);
            DeleteObject(border);
            const int inset = MulDiv(4, dpi, 96);
            RECT inner = { frame.left + inset, frame.top + inset / 2 + 1, frame.right - inset, frame.bottom - inset / 2 - 1 };
            position_search(inner);
        }
        y += sh + MulDiv(4, dpi, 96);
    }

    // Reserve room for the scrollbar when the table is longer than the panel.
    const int split_x = x + (right - x) * 70 / 100; // table 70%, similar artists 30%
    m_similar_rc = { split_x, y, right, rc.bottom - pad };
    const int table_right = split_x - MulDiv(8, dpi, 96);
    const int sb_width = MulDiv(12, dpi, 96), sb_gap = MulDiv(2, dpi, 96);
    const int first_row_top = y + line_h; // the header row sits above the rows
    const int avail_rows = rc.bottom - pad - first_row_top;
    const size_t fit_rows = avail_rows > 0 ? static_cast<size_t>(avail_rows / line_h) : 0;
    const bool need_sb = m_order.size() > fit_rows;
    const int cols_right = need_sb ? table_right - sb_width - sb_gap : table_right;

    // Columns: Name | Type | Country | Area | Begin | End | MBID. The others take their natural
    // width (or less when the table is narrow, never leaving Name under ~28%); Name gets the rest.
    const int table_width = (std::max)(0, cols_right - x);
    const wchar_t* const col_names[7] = { L"Name", L"Type", L"Country", L"Area", L"Begin", L"End", L"MBID" };
    int natural[7] = {};
    natural[1] = (std::max)(text_width(dc, L"Orchestra"), text_width(dc, L"Type") + gap) + gap;
    natural[2] = text_width(dc, L"Country") + 2 * gap;
    natural[3] = text_width(dc, L"United Kingdom") + gap;
    natural[4] = (std::max)(text_width(dc, L"0000"), text_width(dc, L"Begin") + gap) + gap;
    natural[5] = (std::max)(text_width(dc, L"0000"), text_width(dc, L"End") + gap) + gap;
    natural[6] = text_width(dc, L"00000000-0000-0000-0000-000000000000") + gap;
    int others = 0;
    for (int c = 1; c < 7; ++c) others += natural[c];
    const int name_min = table_width * 28 / 100;
    int widths[7] = {};
    if (others > table_width - name_min) {
        const int room = (std::max)(0, table_width - name_min);
        for (int c = 1; c < 7; ++c) widths[c] = others > 0 ? static_cast<int>(static_cast<long long>(natural[c]) * room / others) : 0;
    } else {
        for (int c = 1; c < 7; ++c) widths[c] = natural[c];
    }
    int used = 0;
    for (int c = 1; c < 7; ++c) used += widths[c];
    widths[0] = (std::max)(1, table_width - used);
    m_col_x[0] = x;
    for (int c = 0; c < 7; ++c) m_col_x[c + 1] = m_col_x[c] + widths[c];
    m_col_x[7] = cols_right;

    // Header (click to sort)
    m_header_rc = { x, y, cols_right, y + line_h };
    {
        HBRUSH hb = CreateSolidBrush(mix(m_fg, m_bg, 10));
        FillRect(dc, &m_header_rc, hb);
        DeleteObject(hb);
        SelectObject(dc, m_bold ? m_bold : m_font);
        for (int c = 0; c < 7; ++c) {
            std::wstring label = col_names[c];
            if (m_sort_col == c) label += m_sort_asc ? L" \u25B2" : L" \u25BC";
            draw_text(dc, m_col_x[c] + gap / 2, y, m_col_x[c + 1], line_h, label, m_sort_col == c ? m_accent : m_fg);
        }
        SelectObject(dc, m_font);
    }
    y += line_h;

    // Rows
    m_row_h = line_h;
    m_list_rc = { x, y, cols_right, rc.bottom - pad };
    const int avail = m_list_rc.bottom - m_list_rc.top;
    m_visible_rows = avail > 0 ? static_cast<size_t>(avail / line_h) : 0;
    const size_t n = m_order.size();
    m_amax_scroll = n > m_visible_rows ? n - m_visible_rows : 0;
    if (m_ascroll > m_amax_scroll) m_ascroll = m_amax_scroll;

    if (n == 0) {
        const wchar_t* msg = !m_rows_loaded ? L"Loading artists..."
            : m_rows.empty() ? L"No artists in the database yet. Play or search some tracks to add artists."
            : L"No artists match the search.";
        draw_text(dc, x + gap / 2, y, table_right, line_h, msg, m_dim);
        paint_similar_artists(dc, m_similar_rc, line_h, gap, dpi);
        return;
    }
    const bool focused = GetFocus() == m_hWnd;
    for (size_t i = 0; i < m_visible_rows && m_ascroll + i < n; ++i) {
        const size_t pos = m_ascroll + i;
        const size_t id = m_order[pos];
        const artist_list_item& a = m_rows[id];
        const int ry = m_list_rc.top + static_cast<int>(i) * line_h;
        RECT row = { x, ry, cols_right, ry + line_h };
        const bool selected = m_sel[id] != 0;
        if (selected) {
            HBRUSH sb = CreateSolidBrush(mix(m_accent, m_bg, 35));
            FillRect(dc, &row, sb);
            DeleteObject(sb);
        }
        // Begin and End are stored as YYYY[-MM[-DD]]; the table shows the year only.
        const std::string begin_year = a.begin.substr(0, a.begin.find('-'));
        const std::string end_year = a.end.substr(0, a.end.find('-'));
        const std::string* cells[7] = { &a.name, &a.type, &a.country, &a.area, &begin_year, &end_year, &a.mbid };
        for (int c = 0; c < 7; ++c)
            draw_text(dc, m_col_x[c] + gap / 2, ry, m_col_x[c + 1], line_h, widen(*cells[c]), c == 0 || selected ? m_fg : m_dim);
        if (focused && static_cast<long long>(pos) == m_focus) DrawFocusRect(dc, &row);
    }
    if (need_sb)
        draw_scrollbar(dc, table_right - sb_width, m_list_rc.top, m_list_rc.bottom, dpi, n, m_visible_rows, m_ascroll);
    paint_similar_artists(dc, m_similar_rc, line_h, gap, dpi);
}

// ---------------------------------------------------------- search box -----

void activity_window::create_search() {
    if (m_search.IsWindow()) return;
    HWND h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, m_hWnd, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtr(GWLP_HINSTANCE)), nullptr);
    m_search.Attach(h);
    if (h && m_font) m_search.SetFont(m_font);
    if (h) SendMessageW(h, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search"));
}

void activity_window::position_search(const RECT& r) {
    if (!m_search.IsWindow()) return;
    if (EqualRect(&r, &m_search_applied) && m_search.IsWindowVisible()) return;
    m_search_applied = r;
    m_search.SetWindowPos(nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // The grey hint (shown while the box is empty, also when it has focus) lists what can be
    // typed. Use the longest version that fits the box, down to a plain "Search".
    static const wchar_t* const hints[] = {
        L"Search: name, alias, country (ES, spain), area:  genre:  tag:  type:  mbid:",
        L"name, alias, country (ES, spain), area: genre: tag:",
        L"name, alias, country, genre: tag:",
        L"name, alias, country",
        L"Search",
    };
    const int count = static_cast<int>(sizeof(hints) / sizeof(hints[0]));
    const int available = (r.right - r.left) - 2 * ::GetSystemMetrics(SM_CXEDGE) - 8;
    int pick = count - 1;
    if (HDC dc = m_search.GetDC()) {
        HGDIOBJ old_font = ::SelectObject(dc, reinterpret_cast<HGDIOBJ>(m_search.GetFont()));
        for (int i = 0; i < count; ++i) {
            SIZE size = {};
            ::GetTextExtentPoint32W(dc, hints[i], static_cast<int>(wcslen(hints[i])), &size);
            if (size.cx <= available) { pick = i; break; }
        }
        ::SelectObject(dc, old_font);
        m_search.ReleaseDC(dc);
    }
    if (pick != m_search_cue) {
        m_search_cue = pick;
        ::SendMessageW(m_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(hints[pick]));
    }
}

LRESULT activity_window::OnCommand(UINT, WPARAM wp, LPARAM lp, BOOL& handled) {
    if (m_search.IsWindow() && reinterpret_cast<HWND>(lp) == m_search.m_hWnd && HIWORD(wp) == EN_CHANGE) {
        const int len = ::GetWindowTextLengthW(m_search);
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        ::GetWindowTextW(m_search, &text[0], len + 1);
        text.resize(static_cast<size_t>(len));
        std::string f = narrow(text);
        while (!f.empty() && std::isspace(static_cast<unsigned char>(f.back()))) f.pop_back();
        while (!f.empty() && std::isspace(static_cast<unsigned char>(f.front()))) f.erase(f.begin());
        if (f != m_filter) {
            m_filter = f;
            m_query = mbdb_search::parse_query(f);
            m_ascroll = 0;
            sort_artists();
            Invalidate(FALSE);
        }
        return 0;
    }
    handled = FALSE;
    return 0;
}

// Draws the EDIT in the panel's colours so it follows dark and light schemes.
LRESULT activity_window::OnCtlColorEdit(UINT, WPARAM wp, LPARAM lp, BOOL& handled) {
    if (!m_search.IsWindow() || reinterpret_cast<HWND>(lp) != m_search.m_hWnd || !m_edit_brush) {
        handled = FALSE;
        return 0;
    }
    HDC dc = reinterpret_cast<HDC>(wp);
    SetTextColor(dc, m_fg);
    SetBkColor(dc, m_edit_bg);
    return reinterpret_cast<LRESULT>(m_edit_brush);
}
