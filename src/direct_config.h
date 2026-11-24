#pragma once

#include <stdlib.h>
#include <string.h>

// Discovery and the generated HTTP dispatcher use the same build and runtime gate.
static inline int uxair_direct_enabled(void) {
#ifdef UXAIR_DIRECT_BUILD
    const char *setting = getenv("UXAIR_DIRECT_VIDEO");
    return !setting || strcmp(setting, "1") == 0;
#else
    return 0;
#endif
}

#ifdef __cplusplus
enum class DirectStrategy { Auto, NativeUri, Bufferstream };

inline DirectStrategy direct_strategy() {
    const char *setting = getenv("UXAIR_DIRECT_BACKEND");
    if (setting && strcmp(setting, "native") == 0) return DirectStrategy::NativeUri;
    if (setting && strcmp(setting, "auto") == 0) return DirectStrategy::Auto;
    // webOS 3–4 has a verified BUFFERSTREAM Load contract. URI probing is opt-in.
    return DirectStrategy::Bufferstream;
}
#endif
