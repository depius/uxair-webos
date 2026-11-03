#pragma once
#include <SDL.h>
// Host-only function stubs. The ARM build still compiles against real SDK headers.
#ifdef __cplusplus
extern "C" {
#endif
const char *SDL_webOSCreateExportedWindow(int);
void SDL_webOSDestroyExportedWindow(const char *);
bool SDL_webOSSetExportedWindow(const char *, const SDL_Rect *, const SDL_Rect *);
#ifdef __cplusplus
}
#endif
