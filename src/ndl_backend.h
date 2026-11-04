#pragma once
// ndl_backend - LG's NDL direct media API (webOS 5+, the player on webOS 10)
// behind a small C interface: LG's NDL headers only compile as C. One player
// per process, as NDL itself.

#ifdef __cplusplus
extern "C" {
#endif

enum { NDLB_VIDEO_NONE = 0, NDLB_VIDEO_H264 = 1, NDLB_VIDEO_H265 = 2 };

// Player state events; NDL reports the same codes as Starfish (PF_EVENT_TYPE_*).
// Application callbacks execute under the retirement gate and must not reenter
// this bridge. Lifecycle/Feed APIs serialize vendor calls, never under that gate.
typedef void (*ndlb_event_fn)(void *context, int type, long long value, const char *message);
#define NDLB_CALLBACK_LIMIT 1024

// window_id: an SDL exported window (SDL_webOSCreateExportedWindow) the video is
// bound to, or NULL. audio_rate: 0 for no audio stream, else S16LE stereo PCM.
// Returns 0 on success.
int ndlb_open(const char *app_id, const char *window_id, int video_codec, int width, int height,
              int audio_rate, ndlb_event_fn events, void *context);
int ndlb_video(const void *data, unsigned size, long long pts_us);   // 0 = accepted
int ndlb_audio(const void *data, unsigned size, long long pts_us);   // 0 = accepted
int ndlb_audio_free(void);   // free bytes in the audio buffer, -1 if unknown
int ndlb_close(void);        // retire callbacks, then Unload; -1 poisons reuse
int ndlb_quit(void);         // at exit; failure means resources are unconfirmed
const char *ndlb_error(void);

#ifdef __cplusplus
}
#endif
