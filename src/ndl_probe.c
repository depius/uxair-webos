/* ndl_probe.c - plays an H.264 Annex-B file through LG's NDL direct media API
 * (version 2, webOS 5 and later), the player that replaces Starfish/ACB on
 * webOS 10. Runs as its own small native app (packaging/ndl-probe-appinfo.json),
 * since the TV only lets installed apps onto the system bus.
 *
 * As on webOS 6+, the video is bound to an "exported window" of the app's SDL
 * window: SDL_webOSCreateExportedWindow -> NDL_DirectMediaSetWindowId.
 *
 *   launched by the TV:   plays test.h264 next to the binary, logs to /tmp/ndl-probe.log
 *   by hand:              ndl-probe <file.h264> [fps] [width] [height] [pts-divisor]
 *
 * The file must start every frame with an access unit delimiter (x264: aud=1).
 * pts-divisor: frame time in nanoseconds divided by this (1 = ns, 1000 = us).
 * (C, not C++: LG's NDL headers only compile as C.) */
#define _GNU_SOURCE    /* RTLD_DEFAULT */
#define NDL_DIRECTMEDIA_API_VERSION 2
#include <NDL_directmedia.h>
#include <SDL.h>
#include <SDL_webOS.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

/* exported by webOS 6+ firmware, but not by the SDK's stub library: looked up at run time */
typedef int (*set_window_id_fn)(const char *window_id);

/* a number from the launch parameters ({"params":{"divisor":1000}}), or fallback */
static long long launch_number(const char *json, const char *key, long long fallback) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = json ? strstr(json, pattern) : NULL;
    if (!p || !(p = strchr(p + strlen(pattern), ':'))) return fallback;
    return atoll(p + 1);
}

static double t0;
static FILE *out;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define LOG(...) do { fprintf(out, "[%7.3f] ", now_s() - t0); fprintf(out, __VA_ARGS__); fputc('\n', out); fflush(out); } while (0)

static void on_load(int type, long long value, const char *text) { LOG("load-callback type=0x%x value=%lld %s", type, value, text ? text : ""); }
static void on_released(const char *type) { LOG("resource released: %s", type ? type : ""); }

static void present(SDL_Renderer *renderer) {
    /* transparent, so the video plane behind the window shows */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
}

int main(int argc, char **argv) {
    t0 = now_s();
    int by_hand = argc > 1 && argv[1][0] != '{';    /* the TV passes its launch params as JSON */
    out = by_hand ? stderr : fopen("/tmp/ndl-probe.log", "w");
    if (!out) out = stderr;

    char file[PATH_MAX];
    if (by_hand) snprintf(file, sizeof file, "%s", argv[1]);
    else {
        char self[PATH_MAX] = "";
        ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
        self[l > 0 ? l : 0] = 0;
        snprintf(file, sizeof file, "%s/test.h264", dirname(self));
    }
    const char *params = !by_hand && argc > 1 ? argv[1] : NULL;
    int fps = by_hand && argc > 2 ? atoi(argv[2]) : 30;
    int width = by_hand && argc > 3 ? atoi(argv[3]) : 1280, height = by_hand && argc > 4 ? atoi(argv[4]) : 720;
    long long divisor = by_hand && argc > 5 ? atoll(argv[5]) : launch_number(params, "divisor", 1);
    if (params) LOG("launch params: %.300s", params);
    const char *app_id = getenv("APPID") ? getenv("APPID") : "com.github.depius.uxair.ndlprobe";
    LOG("app %s, file %s, %dx%d @ %d fps, pts divisor %lld", app_id, file, width, height, fps, divisor);

    FILE *f = fopen(file, "rb");
    if (!f) { LOG("cannot open %s", file); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    unsigned char *data = malloc(size);
    if (!data || fread(data, 1, size, f) != (size_t)size) { LOG("read failed"); return 1; }
    fclose(f);
    /* offsets of the access unit delimiters (00 00 00 01 09) that start each frame */
    size_t *at = malloc(sizeof *at * (size / 8 + 2)), n = 0;
    for (long i = 0; i + 4 < size; ++i)
        if (!data[i] && !data[i + 1] && !data[i + 2] && data[i + 3] == 1 && (data[i + 4] & 0x1f) == 9) at[n++] = i;
    LOG("%ld bytes, %zu frames", size, n);
    if (!n) { LOG("no access unit delimiters found (encode with aud=1)"); return 1; }
    at[n] = size;

    setenv("EGL_PLATFORM", "wayland", 0);    /* as src/shell.cpp */
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { LOG("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_Window *window = SDL_CreateWindow("NDL probe", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    if (!renderer) { LOG("SDL window/renderer: %s", SDL_GetError()); return 1; }
    present(renderer);
    const char *window_id = SDL_webOSCreateExportedWindow(0);
    LOG("exported window: %s", window_id ? window_id : SDL_GetError());

    if (!NDL_DirectMedia_DL_IsInitialized()) LOG("DL_Initialize: %s", NDL_DirectMedia_DL_Initialize() ? "ok" : NDL_DirectMediaGetError());
    int r = NDL_DirectMediaInit(app_id, on_released);
    LOG("Init: %d %s", r, r ? NDL_DirectMediaGetError() : "");
    if (r) return 1;
    set_window_id_fn set_window_id = (set_window_id_fn)dlsym(RTLD_DEFAULT, "NDL_DirectMediaSetWindowId");
    if (window_id && set_window_id) LOG("SetWindowId: %d", set_window_id(window_id));
    else LOG("SetWindowId: %s", window_id ? "not exported by the firmware" : "no window id");

    NDL_DIRECTMEDIA_DATA_INFO_T info;
    memset(&info, 0, sizeof info);
    info.video.width = width;
    info.video.height = height;
    info.video.type = NDL_VIDEO_TYPE_H264;
    /* audio: 0 = no audio stream, 1 = PCM fed with silence alongside the video, 2 = PCM never fed */
    int audio = (int)launch_number(params, "audio", 1);
    if (audio) {
        info.audio.pcm.type = NDL_AUDIO_TYPE_PCM;
        info.audio.pcm.format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE;
        info.audio.pcm.channelMode = NDL_DIRECTMEDIA_AUDIO_PCM_MODE_STEREO;
        info.audio.pcm.sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(44100);
    }
    LOG("audio mode %d", audio);
    /* cycles: Load, play, Unload this many times on one Init and window, as a
     * receiver does when the sender changes resolution; each replays from frame 0 */
    int cycles = (int)launch_number(params, "cycles", 1);
    if (cycles < 1) cycles = 1;
    size_t per = n / cycles / fps * fps;
    if (!per) per = n;
    const double frame_s = 1.0 / fps;
    /* one frame's worth of stereo S16 silence at 44.1 kHz */
    const unsigned silence_bytes = (unsigned)(44100 / fps) * 4;
    unsigned char *silence = calloc(1, silence_bytes);
    int ok = 0, failed = 0, audio_failed = 0;
  for (int cycle = 1; cycle <= cycles; ++cycle) {
    r = NDL_DirectMediaLoad(&info, on_load);
    LOG("cycle %d/%d Load: %d %s", cycle, cycles, r, r ? NDL_DirectMediaGetError() : "");
    if (r) { NDL_DirectMediaQuit(); return 1; }
    if (window_id) {
        SDL_Rect src = { 0, 0, width, height }, dst = { 0, 0, 1920, 1080 };
        LOG("SetExportedWindow: %d", SDL_webOSSetExportedWindow(window_id, &src, &dst));
    } else LOG("SetArea(full screen): %d", NDL_DirectVideoSetArea(0, 0, 1920, 1080));
    double start = now_s();
    for (size_t i = 0; i < per; ++i) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) i = per;
        if (i >= per) break;
        long long pts = (long long)(i * frame_s * 1e9) / divisor;
        if (audio == 1 && NDL_DirectAudioPlay(silence, silence_bytes, pts) && audio_failed++ < 5)
            LOG("frame %zu: AudioPlay failed %s", i, NDL_DirectMediaGetError());
        r = NDL_DirectVideoPlay(&data[at[i]], (unsigned)(at[i + 1] - at[i]), pts);
        if (r) { if (failed++ < 5) LOG("frame %zu: VideoPlay %d %s", i, r, NDL_DirectMediaGetError()); }
        else ++ok;
        if (i % fps == 0) {
            int length = -1, avail = -1;
            NDL_DirectVideoGetRenderBufferLength(&length);
            if (audio) NDL_DirectAudioGetAvailableBufferSize(&avail);
            LOG("frame %zu sent (ok %d, failed %d, render buffer %d, audio free %d)", i, ok, failed, length, avail);
            present(renderer);
        }
        double wait = start + (i + 1) * frame_s - now_s();
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
    }
    if (cycle == cycles) { LOG("done: %d frames accepted, %d refused; holding 2 s", ok, failed); sleep(2); }
    LOG("cycle %d Unload: %d", cycle, NDL_DirectMediaUnload());
  }
    LOG("Quit: %d", NDL_DirectMediaQuit());
    if (window_id) SDL_webOSDestroyExportedWindow(window_id);
    SDL_Quit();
    return failed ? 1 : 0;
}
