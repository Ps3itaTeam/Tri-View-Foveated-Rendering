#pragma once

// [RELEASE SWITCH fix139/fix141 23.11.2026] Release or debug build of the mod, for every game (RE2, RE4, RE7, ...).
//
// REVR_RELEASE 1 (the default): the release. What the user took out for the release is cut out by the preprocessor:
// it is not compiled at all, not even in part.
//   * fix139/fix140: the menu options he marked "no" and the developer tools (#if !REVR_RELEASE), their values fixed
//     at their defaults when the configuration is read.
//   * fix141: every log (REFramework's re2_framework_log.txt and every other log file, the mod's foveation_debug.log,
//     the console and debugger output) and the debug functions (tests, dumps, video, captures, measurements, crash
//     dump). Log calls are written REVR_LOG(spdlog::info(...)): in the release the whole call, its arguments included,
//     is gone.
// For a debug build with all of it, define REVR_DEBUG_TOOLS: uncomment the line below, or pass /DREVR_DEBUG_TOOLS to the
// compiler.
//
// Every file that tests REVR_RELEASE or uses REVR_LOG includes this header (a file that does not would read
// REVR_RELEASE as 0, the debug build: the syntax check runs with -Wundef to find such a file). It lives in shared/ so
// that REFramework's SDK and utility libraries see it too.

// #define REVR_DEBUG_TOOLS

#if defined(REVR_DEBUG_TOOLS)
#define REVR_RELEASE 0
#define REVR_LOG(...) __VA_ARGS__
#else
#define REVR_RELEASE 1
#define REVR_LOG(...) ((void)0)
#endif
