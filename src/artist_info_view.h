#pragma once
// Rich Edit rendering of an artist record, shared by the artist info window and
// the activity panel's "Now playing" tab (same sections, colours and links).
#include "stdafx.h"
#include <memory>
#include <string>
#include "../include/musicbrainz_db_types.h"

// Creates a read-only Rich Edit child (hidden, zero size, link clicks enabled).
// Returns nullptr when Rich Edit is not available.
HWND mbdb_info_create_richedit(HWND parent);

// Display content for one artist (sections plus highlighted raw JSON). Building it parses
// and formats the whole record, which can take a while for big records, so it is done
// separately from rendering: build on any thread, render on the UI thread.
struct mbdb_info_content;
std::shared_ptr<const mbdb_info_content> mbdb_info_build(const artist_list_item& item,
                                                         const artist_record* record);
bool mbdb_info_has_raw(const mbdb_info_content& content);

// UI thread only. Fills the control with the artist's sections; `show_raw` appends the
// highlighted raw JSON. `background`/`text` override the theme colours (CLR_INVALID keeps
// them) and `font` (the host's font, not owned) the text font; nullptr uses the active UI's font.
void mbdb_info_render(HWND edit, const mbdb_info_content& content, bool show_raw,
                      bool dark, COLORREF background, COLORREF text, HFONT font,
                      COLORREF link = CLR_INVALID); // link: hyperlink colour, CLR_INVALID = theme default

// Fills the control with a plain message (nothing playing, looking up, not found...).
void mbdb_info_render_message(HWND edit, const std::string& message, bool dark,
                              COLORREF background, COLORREF text, HFONT font,
                              COLORREF link = CLR_INVALID);

// Returns the validated URL represented by an EN_LINK notification.
// Returns an empty string if the notification is not a supported link.
std::wstring mbdb_info_get_link(LPNMHDR hdr);

// Opens the clicked link in the external browser.
void mbdb_info_open_link(LPNMHDR hdr);

