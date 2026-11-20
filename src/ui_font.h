#pragma once
#include "stdafx.h"

// The default font of whichever UI is active (Default UI or Columns UI), or
// nullptr if it cannot be determined. Owned by the UI: never delete it, and
// re-query it when the UI reports a font change (it can be replaced).
inline HFONT mbdb_ui_default_font() {
    try {
        static_api_ptr_t<ui_config_manager> config;
        // Fix: Call query_font with only one argument and assign the return value to font
        HFONT font = config->query_font(ui_font_default);
        return font;
    } catch (...) {
        return nullptr;
    }
}
