#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Called only by the source-checked sender TEARDOWN instrumentation.
void uxair_sender_video_reset(void *receiver,int reason);
#ifdef __cplusplus
}
#endif
