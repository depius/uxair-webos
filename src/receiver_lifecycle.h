#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct raop_rtp_s;
struct raop_rtp_mirror_s;
unsigned uxair_admit_transport(void *receiver,const char *id,const char *model,const char *name,bool *admit);
void uxair_audio_set_epoch(struct raop_rtp_s *rtp,unsigned epoch);
void uxair_video_set_epoch(struct raop_rtp_mirror_s *rtp,unsigned epoch);
void uxair_sender_video_reset(void *receiver,int reason,unsigned epoch);
void uxair_rtp_reset(void *receiver,int video,int reason,unsigned epoch);
void uxair_rtp_packet(void *receiver,int video,unsigned codec,uint64_t timestamp,
                      const uint8_t *data,int size,unsigned epoch);
void uxair_rtp_audio_flush(void *receiver,unsigned epoch);
void uxair_rtp_video_state(void *receiver,unsigned epoch,int running_state,int value);
#ifdef __cplusplus
}
#endif
