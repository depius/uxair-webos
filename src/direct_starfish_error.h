#pragma once
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>

inline void direct_starfish_exception(const char *backend, const char *stage, const std::exception& error) {
    // Firmware exception messages may include the source URI.
    std::fprintf(stderr, "[direct-player] api=StarfishMediaAPIs backend=%s stage=%s exception=%s\n",
                 backend, stage, std::strcmp(error.what(), "No such node (plugins)") == 0
                     ? "missing-plugins" : "std-exception");
}
