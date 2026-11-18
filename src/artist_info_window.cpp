#include "stdafx.h"

#include <helpers/DarkMode.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include <cwctype>
#include <cstring>

#include <richedit.h>
#include <shellapi.h>

#include "artist_info_window.h"
#include "artist_table.h"
#include "resource.h"
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")


using json = nlohmann::json;

namespace {

    // ------------------------------------------------------------- Rich Edit --

    HMODULE load_richedit() {
        static HMODULE module = LoadLibraryW(L"Msftedit.dll");
        return module;
    }

    std::wstring utf8_to_wide(const std::string& value) {
        const pfc::stringcvt::string_wide_from_utf8 wide(value.c_str());
        return std::wstring(wide.get_ptr());
    }

    bool is_web_url(const std::wstring& url) {
        return url.compare(0, 7, L"http://") == 0 ||
            url.compare(0, 8, L"https://") == 0;
    }

    bool is_mbid(const std::wstring& value) {
        if (value.size() != 36) return false;

        for (size_t i = 0; i < value.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (value[i] != L'-') return false;
            }
            else {
                const wchar_t c = value[i];

                if (!((c >= L'0' && c <= L'9') ||
                    (c >= L'a' && c <= L'f') ||
                    (c >= L'A' && c <= L'F'))) {
                    return false;
                }
            }
        }

        return true;
    }

    // Select a Rich Edit character range using its extended character positions.
    void set_rich_selection(HWND edit, LONG start, LONG end) {
        CHARRANGE range{};
        range.cpMin = start;
        range.cpMax = end;

        ::SendMessageW(
            edit,
            EM_EXSETSEL,
            0,
            reinterpret_cast<LPARAM>(&range)
        );
    }

    // Ask the Windows theme engine to use its dark scrollbar treatment.
    // DarkMode_Explorer is supported by current Windows 10/11 theme builds.
    void apply_rich_edit_scrollbar_theme(HWND edit, bool dark) {
        if (!edit || !::IsWindow(edit)) return;

        ::SetWindowTheme(
            edit,
            dark ? L"DarkMode_Explorer" : nullptr,
            nullptr
        );

        // Force the non-client scrollbar area to repaint after theme changes.
        ::SetWindowPos(
            edit,
            nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED
        );
        ::RedrawWindow(
            edit,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_FRAME | RDW_ERASE
        );
    }

    // ------------------------------------------------------------- JSON -------

    std::string json_string(const json& j, const char* key) {
        if (!j.is_object()) return {};

        const auto it = j.find(key);

        return (it != j.end() && it->is_string())
            ? it->get<std::string>()
            : std::string();
    }

    std::string join_counted(const json& arr) {
        std::string out;

        if (!arr.is_array()) return out;

        for (const auto& e : arr) {
            const std::string name = json_string(e, "name");
            if (name.empty()) continue;

            if (!out.empty()) out += ", ";

            out += name;

            const auto c = e.find("count");

            if (c != e.end() &&
                c->is_number_integer() &&
                c->get<long long>() > 0) {
                out += " (" + std::to_string(c->get<long long>()) + ")";
            }
        }

        return out;
    }

    std::string join_strings(const json& arr) {
        std::string out;

        if (!arr.is_array()) return out;

        for (const auto& e : arr) {
            if (!e.is_string()) continue;

            if (!out.empty()) out += ", ";

            out += e.get<std::string>();
        }

        return out;
    }

    // ------------------------------------------------------------- formatting -

    enum class format_kind {
        heading,
        label,
        hyperlink,
        json_key,
        json_string,
        json_number,
        json_literal
    };

    struct format_range {
        LONG start;
        LONG end;
        format_kind kind;
    };


    struct rich_document {
        std::wstring text;
        std::vector<format_range> ranges;

        LONG position() const {
            return static_cast<LONG>(text.length());
        }

        // RichEdit uses a single CR character for a paragraph break.
        // Normalize CRLF and LF to CR so range offsets stay aligned.
        void append_wide(const std::wstring& value) {
            for (size_t i = 0; i < value.length(); ++i) {
                const wchar_t ch = value[i];

                if (ch == L'\r') {
                    text += L'\r';

                    // Skip the LF in a CRLF pair.
                    if (i + 1 < value.length() && value[i + 1] == L'\n') {
                        ++i;
                    }
                }
                else if (ch == L'\n') {
                    text += L'\r';
                }
                else {
                    text += ch;
                }
            }
        }

        void append(const std::string& value) {
            append_wide(utf8_to_wide(value));
        }

        void plain(const std::string& value) {
            append(value);
        }

        void section(const char* title) {
            if (!text.empty()) {
                text += L'\r';
            }

            const LONG start = position();
            append(title);
            const LONG end = position();

            ranges.push_back({
                start,
                end,
                format_kind::heading
                });

            text += L'\r';
        }

        void line(const char* label, const std::string& value) {
            if (value.empty()) return;

            const LONG label_start = position();
            append(label);
            const LONG label_end = position();

            text += L": ";

            ranges.push_back({
                label_start,
                label_end,
                format_kind::label
                });

            const LONG value_start = position();
            append(value);
            const LONG value_end = position();

            if (strcmp(label, "MBID") == 0 &&
                is_mbid(utf8_to_wide(value))) {
                ranges.push_back({
                    value_start,
                    value_end,
                    format_kind::hyperlink
                    });
            }

            text += L'\r';
        }

        void link(const std::string& label, const std::string& url) {
            if (url.empty()) return;

            const LONG label_start = position();
            append(label);
            const LONG label_end = position();

            text += L": ";

            ranges.push_back({
                label_start,
                label_end,
                format_kind::label
                });

            const LONG link_start = position();
            append(url);
            const LONG link_end = position();

            ranges.push_back({
                link_start,
                link_end,
                format_kind::hyperlink
                });

            text += L'\r';
        }

        void merge(const rich_document& other) {
            const LONG offset = position();

            text += other.text;

            for (const auto& r : other.ranges) {
                ranges.push_back({
                    r.start + offset,
                    r.end + offset,
                    r.kind
                    });
            }
        }
    };



    // ------------------------------------------------------------- JSON syntax -

    void highlight_json(rich_document& doc, size_t start) {
        const std::wstring& s = doc.text;

        size_t i = start;

        auto add_range = [&](size_t begin, size_t end, format_kind kind) {
            if (end <= begin) return;

            doc.ranges.push_back({
                static_cast<LONG>(begin),
                static_cast<LONG>(end),
                kind
                });
            };

        while (i < s.size()) {
            const wchar_t c = s[i];

            if (iswspace(c)) {
                ++i;
                continue;
            }

            // JSON strings and property names.
            if (c == L'"') {
                const size_t begin = i++;

                while (i < s.size()) {
                    if (s[i] == L'\\') {
                        i += (i + 1 < s.size()) ? 2 : 1;
                        continue;
                    }

                    if (s[i++] == L'"') break;
                }

                size_t next = i;

                while (next < s.size() && iswspace(s[next])) {
                    ++next;
                }

                const bool is_key =
                    next < s.size() && s[next] == L':';

                add_range(
                    begin,
                    i,
                    is_key ? format_kind::json_key
                    : format_kind::json_string
                );

                continue;
            }

            // JSON numbers.
            if (c == L'-' || (c >= L'0' && c <= L'9')) {
                const size_t begin = i++;

                while (i < s.size()) {
                    const wchar_t n = s[i];

                    if ((n >= L'0' && n <= L'9') ||
                        n == L'.' || n == L'e' ||
                        n == L'E' || n == L'+' || n == L'-') {
                        ++i;
                    }
                    else {
                        break;
                    }
                }

                add_range(begin, i, format_kind::json_number);
                continue;
            }

            // true, false and null.
            if (c == L't' || c == L'f' || c == L'n') {
                const size_t begin = i;

                while (i < s.size() &&
                    ((s[i] >= L'a' && s[i] <= L'z') ||
                        (s[i] >= L'A' && s[i] <= L'Z'))) {
                    ++i;
                }

                add_range(begin, i, format_kind::json_literal);
                continue;
            }

            ++i;
        }
    }

    // ------------------------------------------------------------- artist data -

    struct artist_content {
        rich_document main;
        rich_document raw;
    };

    artist_content build_content(
        const artist_list_item& item,
        const artist_record* rec) {

        artist_content content;

        rich_document& out = content.main;

        const std::string& name =
            (rec && !rec->name.empty()) ? rec->name : item.name;

        json j;
        bool parsed = false;

        if (rec && !rec->raw_json.empty()) {
            try {
                j = json::parse(rec->raw_json);
                parsed = j.is_object();
            }
            catch (const std::exception&) {
                parsed = false;
            }
        }

        // Artist
        out.section("ARTIST");

        out.line("Name", name);

        if (parsed) {
            out.line("Sort name", json_string(j, "sort-name"));
        }

        out.line("MBID", item.mbid);
        out.line("Type", rec ? rec->type : item.type);

        if (parsed) {
            out.line("Gender", json_string(j, "gender"));
        }

        out.line("Country", rec ? rec->country : item.country);

        if (rec) {
            out.line("Disambiguation", rec->disambiguation);
        }

        // Location and activity
        if (rec) {
            out.section("LOCATION & ACTIVITY");

            out.line("Area", rec->area);
            out.line("Begin area", rec->begin_area);
            out.line("End area", rec->end_area);
            out.line("Begin date", rec->begin_date);
            out.line("End date", rec->end_date);
        }

        // Identifiers
        if (parsed) {
            out.section("IDENTIFIERS");

            const auto isnis = j.find("isnis");

            if (isnis != j.end()) {
                out.line("ISNI", join_strings(*isnis));
            }

            const auto ipis = j.find("ipis");

            if (ipis != j.end()) {
                out.line("IPI", join_strings(*ipis));
            }
        }

        // Music profile
        if (parsed) {
            std::string rating;

            const auto r = j.find("rating");

            if (r != j.end() && r->is_object()) {
                const auto v = r->find("value");

                if (v != r->end() && v->is_number()) {
                    rating = std::to_string(v->get<double>());

                    const auto last = rating.find_last_not_of('0');

                    if (last != std::string::npos) {
                        rating.erase(last + 1);
                    }

                    if (!rating.empty() && rating.back() == '.') {
                        rating.pop_back();
                    }

                    const auto votes = r->find("votes-count");

                    if (votes != r->end() && votes->is_number_integer()) {
                        rating += " (" +
                            std::to_string(votes->get<long long>()) +
                            " votes)";
                    }
                }
            }

            const std::string genres =
                j.contains("genres") ? join_counted(j["genres"]) : "";

            const std::string tags =
                j.contains("tags") ? join_counted(j["tags"]) : "";

            if (!rating.empty() || !genres.empty() || !tags.empty()) {
                out.section("MUSIC PROFILE");

                out.line("Rating", rating);
                out.line("Genres", genres);
                out.line("Tags", tags);
            }
        }

        // Aliases
        if (parsed) {
            const auto aliases = j.find("aliases");

            if (aliases != j.end() &&
                aliases->is_array() &&
                !aliases->empty()) {

                rich_document aliases_doc;

                for (const auto& a : *aliases) {
                    const std::string alias = json_string(a, "name");

                    if (alias.empty()) continue;

                    const std::string type = json_string(a, "type");

                    aliases_doc.line(
                        type.empty() ? "Alias" : type.c_str(),
                        alias
                    );
                }

                if (!aliases_doc.text.empty()) {
                    rich_document section;
                    section.section("ALIASES");
                    section.merge(aliases_doc);
                    out.merge(section);
                }
            }
        }

        // External links
        if (parsed) {
            const auto relations = j.find("relations");

            if (relations != j.end() && relations->is_array()) {
                rich_document links_doc;

                for (const auto& relation : *relations) {
                    if (!relation.is_object()) continue;

                    const auto u = relation.find("url");

                    if (u == relation.end() || !u->is_object()) {
                        continue;
                    }

                    const std::string url = json_string(*u, "resource");

                    if (url.empty()) continue;

                    std::string type = json_string(relation, "type");

                    if (type.empty()) type = "Link";

                    if (url.compare(0, 7, "http://") == 0 ||
                        url.compare(0, 8, "https://") == 0) {
                        links_doc.link(type, url);
                    }
                    else {
                        links_doc.line(type.c_str(), url);
                    }
                }

                if (!links_doc.text.empty()) {
                    rich_document section;
                    section.section("EXTERNAL LINKS");
                    section.merge(links_doc);
                    out.merge(section);
                }
            }
        }

        // Annotation
        if (parsed) {
            const std::string annotation = json_string(j, "annotation");

            if (!annotation.empty()) {
                out.section("ANNOTATION");
                out.plain(annotation);
                out.plain("\r\n");
            }
        }

        if (!parsed) {
            out.section("INFORMATION");
            out.plain("No full MusicBrainz record is stored for this artist.\r\n");
        }

        // Raw JSON
        if (parsed) {
            content.raw.section("RAW JSON");

            const size_t json_start = content.raw.text.size();

            content.raw.plain(
                j.dump(2, ' ', false, json::error_handler_t::replace)
            );

            content.raw.plain("\r\n");

            // JSON syntax highlighting is applied only to this document.
            highlight_json(content.raw, json_start);
        }

        return content;
    }

    // ------------------------------------------------------------- dialog ------

    class artist_info_dialog;

    std::map<std::string, artist_info_dialog*> g_open;

    // Hook that refreshes the Rich Edit when foobar2000 changes UI colors.
    class artist_info_dark_mode_hooks : public fb2k::CDarkModeHooks {
    public:
        std::function<void()> on_colors_changed;

    private:
        void ui_colors_changed() override {
            SetDark(fb2k::isDarkMode());

            if (on_colors_changed) {
                on_colors_changed();
            }
        }

        void ui_fonts_changed() override {
            // Font changes are not customized by this dialog.
        }
    };

    class artist_info_dialog : public CDialogImpl<artist_info_dialog> {
    public:
        enum { IDD = IDD_MBDB_ARTIST_INFO };

        explicit artist_info_dialog(std::string mbid)
            : m_mbid(std::move(mbid)) {
        }

        void set_content(
            const std::wstring& title,
            const artist_content& content) {

            m_title = title;
            m_main = content.main;
            m_raw = content.raw;

            if (m_hWnd) {
                apply_content();
            }
        }

        void SetForegroundWindow() {
            if (m_hWnd) {
                ::SetForegroundWindow(m_hWnd);
            }
        }

        BEGIN_MSG_MAP_EX(artist_info_dialog)
            MSG_WM_INITDIALOG(OnInitDialog)
            MSG_WM_SIZE(OnSize)
            MSG_WM_CLOSE(OnClose)
            MSG_WM_DESTROY(OnDestroy)

            COMMAND_ID_HANDLER_EX(IDCANCEL, OnCloseCommand)
            COMMAND_ID_HANDLER_EX(IDOK, OnCloseCommand)
            COMMAND_ID_HANDLER_EX(IDC_RAW_JSON, OnRawJson)

            NOTIFY_CODE_HANDLER_EX(EN_LINK, OnLink)
            END_MSG_MAP()

        void OnFinalMessage(HWND) override {
            delete this;
        }

    private:

        // --------------------------------------------------------- theme --------

        void update_theme() {
            const bool dark = fb2k::isDarkMode();

            if (dark) {
                m_background_color = RGB(30, 30, 30);
                m_text_color = RGB(220, 220, 220);
                m_heading_color = RGB(130, 180, 240);
                m_link_color = RGB(100, 175, 255);

                m_json_key_color = RGB(156, 220, 150);     // Corrected: strings/keys
                m_json_string_color = RGB(206, 145, 120);  // Corrected: string values
                m_json_number_color = RGB(181, 206, 168);  // Numbers
                m_json_literal_color = RGB(197, 134, 192); // Booleans/null
            }
            else {
                m_background_color = RGB(255, 255, 255);
                m_text_color = RGB(35, 35, 35);
                m_heading_color = RGB(50, 115, 190);
                m_link_color = RGB(35, 115, 205);

                m_json_key_color = RGB(0, 0, 255);         // Corrected: keys
                m_json_string_color = RGB(163, 21, 21);    // Corrected: strings
                m_json_number_color = RGB(9, 134, 88);     // Numbers
                m_json_literal_color = RGB(128, 0, 128);   // Booleans/null
            }

            HWND edit = GetDlgItem(IDC_INFO_TEXT);

            if (!edit || !::IsWindow(edit)) {
                return;
            }

            // CDarkModeHooks themes the dialog and standard controls; RichEdit
            // needs its own theme request for a dark scrollbar.
            apply_rich_edit_scrollbar_theme(edit, dark);

            CHARRANGE selection{};

            ::SendMessage(
                edit,
                EM_EXGETSEL,
                0,
                reinterpret_cast<LPARAM>(&selection)
            );

            apply_content();

            ::SendMessage(
                edit,
                EM_EXSETSEL,
                0,
                reinterpret_cast<LPARAM>(&selection)
            );

            ::InvalidateRect(edit, nullptr, TRUE);
        }

        // --------------------------------------------------------- initialization

        BOOL OnInitDialog(CWindow, LPARAM) {
            m_dark.on_colors_changed = [this]() {
                if (m_hWnd && ::IsWindow(m_hWnd)) {
                    update_theme();
                }
                };


            m_dark.AddDialogWithControls(*this);

            CRect margin(0, 0, 6, 6);
            MapDialogRect(&margin);
            m_margin = margin.right;

            CRect button;

            ::GetWindowRect(GetDlgItem(IDCANCEL), &button);
            m_button_w = button.Width();
            m_button_h = button.Height();

            ::GetWindowRect(GetDlgItem(IDC_RAW_JSON), &button);
            m_raw_button_w = button.Width();
            m_raw_button_h = button.Height();

            HWND edit = GetDlgItem(IDC_INFO_TEXT);

            ::SendMessage(edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);

            // Enable notifications for clickable hyperlinks.
            ::SendMessage(
                edit,
                EM_SETEVENTMASK,
                0,
                ::SendMessage(edit, EM_GETEVENTMASK, 0, 0) | ENM_LINK
            );

            update_theme();

            CRect client;
            GetClientRect(&client);

            layout(client.Width(), client.Height());

            // Assign focus explicitly so the dialog manager does not apply
            // its default RichEdit selection after this handler returns.
            ::SetFocus(edit);
            set_rich_selection(edit, 0, 0);

            // We assigned the initial focus and collapsed the selection.
            return FALSE;
        }

        // --------------------------------------------------------- formatting ----

        void apply_formatting(HWND edit, const rich_document& doc) {
            const LONG text_length = static_cast<LONG>(doc.text.length());
            if (text_length <= 0) return;

            // 1. Set base format for the entire document first
            CHARFORMAT2W base{};
            base.cbSize = sizeof(base);
            base.dwMask = CFM_COLOR | CFM_SIZE | CFM_FACE | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_LINK;
            base.dwEffects = 0;
            base.yHeight = 200; // 10pt
            base.crTextColor = m_text_color;
            wcscpy_s(base.szFaceName, L"Segoe UI");

            set_rich_selection(edit, 0, text_length);
            ::SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));

            // 2. Apply each range with its exact isolated mask and color
            for (const auto& r : doc.ranges) {
                if (r.start < 0 || r.end <= r.start || r.end > text_length) {
                    continue;
                }

                CHARFORMAT2W cf{};
                cf.cbSize = sizeof(cf);

                switch (r.kind) {
                case format_kind::heading:
                    cf.dwMask = CFM_BOLD | CFM_SIZE | CFM_COLOR;
                    cf.dwEffects = CFE_BOLD;
                    cf.yHeight = 240; // 12pt
                    cf.crTextColor = m_heading_color;
                    break;

                case format_kind::label:
                    cf.dwMask = CFM_BOLD | CFM_COLOR;
                    cf.dwEffects = CFE_BOLD;
                    cf.crTextColor = m_text_color; // Explicitly set to standard text color
                    break;

                case format_kind::hyperlink:
                    cf.dwMask = CFM_UNDERLINE | CFM_COLOR | CFM_LINK;
                    cf.dwEffects = CFE_UNDERLINE | CFE_LINK;
                    cf.crTextColor = m_link_color;
                    break;

                case format_kind::json_key:
                    cf.dwMask = CFM_COLOR;
                    cf.dwEffects = 0;
                    cf.crTextColor = m_json_key_color;
                    break;

                case format_kind::json_string:
                    cf.dwMask = CFM_COLOR;
                    cf.dwEffects = 0;
                    cf.crTextColor = m_json_string_color;
                    break;

                case format_kind::json_number:
                    cf.dwMask = CFM_COLOR;
                    cf.dwEffects = 0;
                    cf.crTextColor = m_json_number_color;
                    break;

                case format_kind::json_literal:
                    cf.dwMask = CFM_COLOR;
                    cf.dwEffects = 0;
                    cf.crTextColor = m_json_literal_color;
                    break;
                }

                set_rich_selection(edit, r.start, r.end);
                ::SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
            }

            // Restore caret to the beginning
            set_rich_selection(edit, 0, 0);
        }

        // --------------------------------------------------------- content -------

        void apply_content() {
            if (!m_hWnd || !::IsWindow(m_hWnd)) {
                return;
            }

            SetWindowText(m_title.c_str());

            HWND edit = GetDlgItem(IDC_INFO_TEXT);

            if (!edit || !::IsWindow(edit)) {
                return;
            }

            rich_document doc = m_main;

            if (m_show_raw && !m_raw.text.empty()) {
                doc.merge(m_raw);
            }

            ::SendMessage(edit, WM_SETREDRAW, FALSE, 0);

            // Clear existing content.
            set_rich_selection(edit, 0, -1);

            ::SendMessage(
                edit,
                EM_REPLACESEL,
                FALSE,
                reinterpret_cast<LPARAM>(L"")
            );

            // Insert the new UTF-16 text.
            if (!doc.text.empty()) {
                SETTEXTEX st{};
                st.flags = ST_DEFAULT;
                st.codepage = 1200;

                ::SendMessage(
                    edit,
                    EM_SETTEXTEX,
                    reinterpret_cast<WPARAM>(&st),
                    reinterpret_cast<LPARAM>(doc.text.c_str())
                );
            }

            ::SendMessage(
                edit,
                EM_SETBKGNDCOLOR,
                0,
                static_cast<LPARAM>(m_background_color)
            );

            apply_formatting(edit, doc);

            set_rich_selection(edit, 0, 0);
            ::SendMessage(edit, EM_SCROLLCARET, 0, 0);

            ::SetWindowText(
                GetDlgItem(IDC_RAW_JSON),
                m_show_raw ? L"Hide Raw JSON" : L"Show Raw JSON"
            );

            ::EnableWindow(
                GetDlgItem(IDC_RAW_JSON),
                !m_raw.text.empty()
            );

            ::SendMessage(edit, WM_SETREDRAW, TRUE, 0);
            ::InvalidateRect(edit, nullptr, TRUE);
        }

        // --------------------------------------------------------- layout --------

        void layout(int cx, int cy) {
            const int m = m_margin;

            const int button_top = cy - m - m_button_h;
            const int edit_bottom = button_top - m;

            ::MoveWindow(
                GetDlgItem(IDC_INFO_TEXT),
                m,
                m,
                (std::max)(0, cx - 2 * m),
                (std::max)(0, edit_bottom - m),
                TRUE
            );

            ::MoveWindow(
                GetDlgItem(IDCANCEL),
                cx - m - m_button_w,
                button_top,
                m_button_w,
                m_button_h,
                TRUE
            );

            ::MoveWindow(
                GetDlgItem(IDC_RAW_JSON),
                cx - m - m_button_w - m - m_raw_button_w,
                button_top,
                m_raw_button_w,
                m_raw_button_h,
                TRUE
            );
        }

        void OnSize(UINT, CSize size) {
            if (m_margin > 0) {
                layout(size.cx, size.cy);
            }
        }

        // --------------------------------------------------------- commands ------

        void OnRawJson(UINT, int, CWindow) {
            if (m_raw.text.empty()) return;

            m_show_raw = !m_show_raw;
            apply_content();
        }

        LRESULT OnLink(LPNMHDR hdr) {
            auto* link = reinterpret_cast<ENLINK*>(hdr);

            if (link->msg != WM_LBUTTONUP) {
                return 0;
            }

            const LONG len =
                link->chrg.cpMax - link->chrg.cpMin;

            if (len <= 0 || len > 2048) {
                return 0;
            }

            std::wstring url(static_cast<size_t>(len) + 1, L'\0');

            TEXTRANGEW range{};
            range.chrg = link->chrg;
            range.lpstrText = url.data();

            ::SendMessage(
                hdr->hwndFrom,
                EM_GETTEXTRANGE,
                0,
                reinterpret_cast<LPARAM>(&range)
            );

            url.resize(static_cast<size_t>(len));

            std::wstring target;

            if (is_web_url(url)) {
                target = url;
            }
            else if (is_mbid(url)) {
                target = L"https://musicbrainz.org/artist/" + url;
            }

            if (!target.empty()) {
                ::ShellExecuteW(
                    nullptr,
                    L"open",
                    target.c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL
                );
            }

            return 0;
        }

        void OnClose() {
            DestroyWindow();
        }

        void OnCloseCommand(UINT, int, CWindow) {
            DestroyWindow();
        }

        void OnDestroy() {
            m_dark.on_colors_changed = {};

            modeless_dialog_manager::g_remove(m_hWnd);
            g_open.erase(m_mbid);

            SetMsgHandled(FALSE);
        }

        // --------------------------------------------------------- members -------

        const std::string m_mbid;

        std::wstring m_title;

        rich_document m_main;
        rich_document m_raw;

        bool m_show_raw = false;

        int m_margin = 0;
        int m_button_w = 0;
        int m_button_h = 0;
        int m_raw_button_w = 0;
        int m_raw_button_h = 0;

        // Theme-dependent colors.
        COLORREF m_background_color = RGB(255, 255, 255);
        COLORREF m_text_color = RGB(35, 35, 35);
        COLORREF m_heading_color = RGB(50, 115, 190);
        COLORREF m_link_color = RGB(35, 115, 205);

        COLORREF m_json_key_color = RGB(40, 90, 170);
        COLORREF m_json_string_color = RGB(40, 125, 65);
        COLORREF m_json_number_color = RGB(150, 70, 160);
        COLORREF m_json_literal_color = RGB(175, 100, 35);

        artist_info_dark_mode_hooks m_dark;
    };

} // namespace

// ------------------------------------------------------------- public API --

void mbdb_show_artist_info(const artist_list_item& item) {
    if (!load_richedit()) {
        return;
    }

    artist_record record;
    const bool have = mbdb_artist_get(item.mbid, record);

    const std::string& name =
        (have && !record.name.empty()) ? record.name : item.name;

    const std::wstring title = utf8_to_wide(
        name + " - MusicBrainz artist"
    );

    const artist_content content =
        build_content(item, have ? &record : nullptr);

    const auto existing = g_open.find(item.mbid);

    if (existing != g_open.end()) {
        artist_info_dialog* dlg = existing->second;

        dlg->set_content(title, content);

        if (dlg->IsIconic()) {
            dlg->ShowWindow(SW_RESTORE);
        }

        dlg->SetForegroundWindow();
        return;
    }

    auto* dlg = new artist_info_dialog(item.mbid);

    dlg->set_content(title, content);

    if (!dlg->Create(core_api::get_main_window())) {
        delete dlg;
        return;
    }

    g_open[item.mbid] = dlg;

    modeless_dialog_manager::g_add(dlg->m_hWnd);

    dlg->ShowWindow(SW_SHOW);
}