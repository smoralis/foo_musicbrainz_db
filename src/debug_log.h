#pragma once
// Debug logging to the foobar2000 console (View > Console).
// Set MBDB_DEBUG_LOG to 0 (here or via the project's preprocessor definitions)
// to compile all MBDB_LOG calls out.
#ifndef MBDB_DEBUG_LOG
#define MBDB_DEBUG_LOG 1
#endif

void mbdb_log_impl(const char* fmt, ...);

#if MBDB_DEBUG_LOG
#define MBDB_LOG(...) mbdb_log_impl(__VA_ARGS__)
#else
#define MBDB_LOG(...) ((void)0)
#endif
