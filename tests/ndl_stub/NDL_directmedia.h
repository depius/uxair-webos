#pragma once
// Host-only surface used by the actual C bridge, not a firmware ABI assertion.
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*ndl_test_event_fn)(int, long long, const char *);
typedef ndl_test_event_fn NDLMediaLoadCallback;
typedef struct {
    struct { int width, height, type; } video;
    struct { struct { int type; const char *format, *channelMode; int sampleRate; } pcm; } audio;
} NDL_DIRECTMEDIA_DATA_INFO_T;
enum { NDL_VIDEO_TYPE_H264=1, NDL_VIDEO_TYPE_H265=2, NDL_AUDIO_TYPE_PCM=1 };
#define NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE "S16LE"
#define NDL_DIRECTMEDIA_AUDIO_PCM_MODE_STEREO "stereo"
#define NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(rate) ((rate)==44100?2:(rate)==48000?1:0)
bool NDL_DirectMedia_DL_IsInitialized(void);
bool NDL_DirectMedia_DL_Initialize(void);
int NDL_DirectMediaInit(const char *, void (*)(const char *));
int NDL_DirectMediaLoad(NDL_DIRECTMEDIA_DATA_INFO_T *, ndl_test_event_fn);
int NDL_DirectMediaUnload(void);
int NDL_DirectMediaQuit(void);
const char *NDL_DirectMediaGetError(void);
int NDL_DirectVideoPlay(void *, unsigned, long long);
int NDL_DirectAudioPlay(void *, unsigned, long long);
int NDL_DirectAudioGetAvailableBufferSize(int *);
#ifdef __cplusplus
}
#endif
