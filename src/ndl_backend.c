// NDL API-v2 bridge. Lifecycle/Feed calls are serialized; callbacks may arrive
// on firmware threads. Retired Load entry points are never reused in a process.
#define _GNU_SOURCE
#define NDL_DIRECTMEDIA_API_VERSION 2
#include "ndl_backend.h"
#include <NDL_directmedia.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t api_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static int opened, load_attempted, poisoned, unload_failed, quit_failed;
static char bound_window[96];
static int window_bound;
static const char *last_error = "no NDL error";
static ndlb_event_fn on_event;
static void *event_context;
static int active_slot = -1;
static unsigned next_slot;

// Hold the callback gate throughout application dispatch. Retiring the target
// therefore waits for an in-flight application callback before its owner dies.
// The private Media callback must not reenter the bridge lifecycle/Feed API.
static void dispatch(unsigned slot, int type, long long value, const char *message) {
    pthread_mutex_lock(&callback_mutex);
    if (active_slot == (int)slot && on_event)
        on_event(event_context, type, value, message);
    pthread_mutex_unlock(&callback_mutex);
}

// No context token exists in the vendor ABI. A finite set of distinct immutable
// function addresses is safe even if firmware retains a retired callback forever.
// Never wrap/recycle the pool, including after Quit. Exhaustion requires restart.
#define FOUR(M,a,b) M(a,b,0) M(a,b,1) M(a,b,2) M(a,b,3)
#define ROW(M,a) FOUR(M,a,0) FOUR(M,a,1) FOUR(M,a,2) FOUR(M,a,3) \
    FOUR(M,a,4) FOUR(M,a,5) FOUR(M,a,6) FOUR(M,a,7) \
    FOUR(M,a,8) FOUR(M,a,9) FOUR(M,a,10) FOUR(M,a,11) \
    FOUR(M,a,12) FOUR(M,a,13) FOUR(M,a,14) FOUR(M,a,15)
#define SLOTS(M) ROW(M,0) ROW(M,1) ROW(M,2) ROW(M,3) \
    ROW(M,4) ROW(M,5) ROW(M,6) ROW(M,7) ROW(M,8) ROW(M,9) \
    ROW(M,10) ROW(M,11) ROW(M,12) ROW(M,13) ROW(M,14) ROW(M,15)
#define CALLBACK(a,b,c) static void callback_##a##_##b##_##c(int t,long long v,const char *m) { dispatch((a)*64+(b)*4+(c),t,v,m); }
SLOTS(CALLBACK)
#undef CALLBACK
#define ENTRY(a,b,c) callback_##a##_##b##_##c,
static NDLMediaLoadCallback const callbacks[] = { SLOTS(ENTRY) };
#undef ENTRY
#undef SLOTS
#undef ROW
#undef FOUR
_Static_assert(sizeof callbacks / sizeof callbacks[0] == NDLB_CALLBACK_LIMIT, "callback pool mismatch");

static void retire_target(void) {
    pthread_mutex_lock(&callback_mutex);
    active_slot = -1;
    on_event = NULL;
    event_context = NULL;
    pthread_mutex_unlock(&callback_mutex);
}
static void resource_released(const char *type) {
    // This process-level callback retains no Media pointer or session target.
    fprintf(stderr, "[ndl] resource released: %.80s\n", type ? type : "unknown");
}
static int failure(const char *phase, int driver_available) {
    last_error = phase;
    const char *detail = driver_available ? NDL_DirectMediaGetError() : NULL;
    fprintf(stderr, "[ndl] failure=%s%s%.160s\n", phase, detail ? " detail=" : "", detail ? detail : "");
    return -1;
}
const char *ndlb_error(void) {
    pthread_mutex_lock(&api_mutex);
    const char *result = last_error; // Immutable literals, never vendor-owned storage.
    pthread_mutex_unlock(&api_mutex);
    return result;
}

// api_mutex is held. Invalidate application callbacks before any vendor call,
// including a failed/partially initialized Load. Failed cleanup poisons reuse.
static int close_locked(void) {
    retire_target();
    if (unload_failed) return -1;
    if (load_attempted) {
        if (NDL_DirectMediaUnload()) {
            poisoned = unload_failed = 1;
            return failure("unload-failed-restart-required", 1);
        }
        load_attempted = 0;
        fprintf(stderr, "[ndl] unloaded\n");
    }
    return 0;
}

int ndlb_open(const char *app_id, const char *window_id, int video_codec,
              int width, int height, int audio_rate, ndlb_event_fn events, void *context) {
    pthread_mutex_lock(&api_mutex);
    int result = -1;
    if (close_locked() || poisoned) {
        failure("backend-unusable-restart-required", 0);
        goto done;
    }
    if (!app_id || !events || !context ||
        (video_codec != NDLB_VIDEO_NONE && (!window_id || !*window_id || strlen(window_id) >= sizeof bound_window)) ||
        (audio_rate && !NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(audio_rate))) {
        failure("invalid-configuration", 0);
        goto done;
    }
    if (next_slot == NDLB_CALLBACK_LIMIT) {
        failure("callback-identities-exhausted-restart-required", 0);
        goto done;
    }
    if (!opened) {
        if (!NDL_DirectMedia_DL_IsInitialized() && !NDL_DirectMedia_DL_Initialize()) {
            poisoned = 1;
            failure("dl-initialize-failed-restart-required", 0);
            goto done;
        }
        if (NDL_DirectMediaInit(app_id, resource_released)) {
            // Partial failed Init cleanup is unspecified. Never retry/assume a
            // fresh instance in this process or register a Media target.
            poisoned = 1;
            failure("initialize-failed-restart-required", 1);
            goto done;
        }
        opened = 1;
        window_bound = 0;
    }
    if (video_codec != NDLB_VIDEO_NONE && (!window_bound || strcmp(window_id, bound_window))) {
        int (*set_window_id)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "NDL_DirectMediaSetWindowId");
        if (!set_window_id) {
            failure("window-binding-unavailable", 0);
            goto done;
        }
        // Stable process-owned storage even if failed firmware cleanup retains
        // the ID. Do not pass a soon-to-be-destroyed Media string into NDL.
        snprintf(bound_window, sizeof bound_window, "%s", window_id);
        if (set_window_id(bound_window)) {
            poisoned = 1;
            failure("window-binding-failed-restart-required", 1);
            goto done;
        }
        window_bound = 1;
    }
    NDL_DIRECTMEDIA_DATA_INFO_T info;
    memset(&info, 0, sizeof info);
    if (video_codec != NDLB_VIDEO_NONE) {
        info.video.width = width;
        info.video.height = height;
        info.video.type = video_codec == NDLB_VIDEO_H265 ? NDL_VIDEO_TYPE_H265 : NDL_VIDEO_TYPE_H264;
    }
    if (audio_rate) {
        info.audio.pcm.type = NDL_AUDIO_TYPE_PCM;
        info.audio.pcm.format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE;
        info.audio.pcm.channelMode = NDL_DIRECTMEDIA_AUDIO_PCM_MODE_STEREO;
        info.audio.pcm.sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(audio_rate);
    }
    const unsigned slot = next_slot++;
    pthread_mutex_lock(&callback_mutex);
    event_context = context;
    on_event = events;
    active_slot = (int)slot;
    pthread_mutex_unlock(&callback_mutex);
    load_attempted = 1; // A failing Load may already own resources/callbacks.
    if (NDL_DirectMediaLoad(&info, callbacks[slot])) {
        failure("load-failed", 1);
        close_locked();
        goto done;
    }
    last_error = "no NDL error";
    result = 0;
done:
    pthread_mutex_unlock(&api_mutex);
    return result;
}

int ndlb_video(const void *data, unsigned size, long long pts_us) {
    pthread_mutex_lock(&api_mutex);
    int result = load_attempted && !poisoned ? NDL_DirectVideoPlay((void *)data, size, pts_us) : -1;
    if (result) last_error = "video-feed-rejected";
    pthread_mutex_unlock(&api_mutex);
    return result;
}
int ndlb_audio(const void *data, unsigned size, long long pts_us) {
    pthread_mutex_lock(&api_mutex);
    int result = load_attempted && !poisoned ? NDL_DirectAudioPlay((void *)data, size, pts_us) : -1;
    if (result) last_error = "audio-feed-rejected";
    pthread_mutex_unlock(&api_mutex);
    return result;
}
int ndlb_audio_free(void) {
    pthread_mutex_lock(&api_mutex);
    int available = -1;
    if (!load_attempted || poisoned || NDL_DirectAudioGetAvailableBufferSize(&available)) available = -1;
    pthread_mutex_unlock(&api_mutex);
    return available;
}
int ndlb_close(void) {
    pthread_mutex_lock(&api_mutex);
    const int result = close_locked();
    pthread_mutex_unlock(&api_mutex);
    return result;
}
int ndlb_quit(void) {
    pthread_mutex_lock(&api_mutex);
    int result = close_locked();
    if (quit_failed) {
        result = -1;
    } else if (opened) {
        if (NDL_DirectMediaQuit()) {
            poisoned = quit_failed = 1;
            result = failure("quit-failed-restart-required", 1);
        } else {
            opened = load_attempted = unload_failed = window_bound = 0;
            // Do not clear poisoned or recycle callback identities after Quit.
            result = 0; // Successful deinitialization confirms resource release.
            fprintf(stderr, "[ndl] quit\n");
        }
    }
    pthread_mutex_unlock(&api_mutex);
    return result;
}
