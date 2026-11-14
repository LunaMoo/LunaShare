// LunaShare - detailed logging for test builds (LUNASHARE_DIAG, see CMakeLists.txt): DIAG("fmt", ...) writes a line to
// stderr - the app keeps it in its diagnostic log. In ordinary builds it's nothing at all.
#pragma once

#include <cstdio>

#if defined(LUNASHARE_DIAG)
#define DIAG(...)                     \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr);          \
    } while (0)
#else
#define DIAG(...) \
    do {          \
    } while (0)
#endif
