#include <foobar2000.h>
#include <cstdarg>
#include <cstdio>
#include "debug_log.h"
#include "activity.h"
#include "preferences.h"

// console::print() is thread-safe, so this may be called from worker threads.
void mbdb_log_impl(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);

    // The activity panel always gets the event; the console only if enabled in Preferences > Tools > MusicBrainz DB.
    mbdb_activity::add_log_line(buf);
    if (!mbdb_prefs::debug_logging()) return;

    pfc::string_formatter message;
    message << "foo_musicbrainz_db [debug][t" << (unsigned)GetCurrentThreadId() << "]: " << buf;
    console::print(message.get_ptr());
}
