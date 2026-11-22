#define _POSIX_C_SOURCE 200809L
#include "direct_config.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
#ifdef UXAIR_DIRECT_BUILD
    const int enabled = 1;
#else
    const int enabled = 0;
#endif
    assert(unsetenv("UXAIR_DIRECT_VIDEO") == 0 && uxair_direct_enabled() == enabled);
    assert(setenv("UXAIR_DIRECT_VIDEO", "0", 1) == 0 && !uxair_direct_enabled());
    assert(setenv("UXAIR_DIRECT_VIDEO", "1", 1) == 0 && uxair_direct_enabled() == enabled);
    assert(setenv("UXAIR_DIRECT_VIDEO", "", 1) == 0 && !uxair_direct_enabled());
#ifdef __cplusplus
    assert(unsetenv("UXAIR_DIRECT_BACKEND") == 0 && direct_strategy() == DirectStrategy::Auto);
    assert(setenv("UXAIR_DIRECT_BACKEND", "native", 1) == 0 && direct_strategy() == DirectStrategy::NativeUri);
    assert(setenv("UXAIR_DIRECT_BACKEND", "bufferstream", 1) == 0 && direct_strategy() == DirectStrategy::Bufferstream);
    assert(setenv("UXAIR_DIRECT_BACKEND", "auto", 1) == 0 && direct_strategy() == DirectStrategy::Auto);
#endif
    puts("PASS: compiled Direct Video scope, defaults, and explicit overrides");
}
