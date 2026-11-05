#include <foobar2000.h>
#include <cstdarg>
#include <cstdio>
#include "debug_log.h"

namespace {
// Preferences > Advanced > Tools > MusicBrainz DB > Enable debug logging
const GUID guid_advconfig_branch = { 0xc8585048, 0xb16c, 0x4fac, { 0xb5, 0xbb, 0x3a, 0x7b, 0x68, 0x2b, 0x81, 0xf6 } };
// New GUID in 0.1.0 so values saved by earlier builds (which defaulted to on) are ignored.
const GUID guid_advconfig_debug_log = { 0x383fed18, 0x380a, 0x415e, { 0x82, 0xf8, 0x23, 0xdc, 0xd9, 0x80, 0x77, 0x34 } };

advconfig_branch_factory g_branch("MusicBrainz DB", guid_advconfig_branch,
    advconfig_branch::guid_branch_tools, 0.0);
advconfig_checkbox_factory g_debug_log_option("Enable debug logging to the console",
    guid_advconfig_debug_log, guid_advconfig_branch, 0.0, false);
}

// console::print() is thread-safe, so this may be called from worker threads.
void mbdb_log_impl(const char* fmt, ...) {
    if (!g_debug_log_option.get()) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);

    pfc::string_formatter message;
    message << "foo_musicbrainz_db [debug][t" << (unsigned)GetCurrentThreadId() << "]: " << buf;
    console::print(message.get_ptr());
}
