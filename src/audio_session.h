#pragma once
#include <cstdint>

struct AirPlayAudioIntent {
    unsigned codec;
    unsigned samples_per_frame;
    bool using_screen;
    bool is_media;
    bool configured;

    AirPlayAudioIntent(unsigned codec_value = 0, unsigned samples = 0,
                       bool screen = false, bool media = false, bool is_configured = false)
        : codec(codec_value), samples_per_frame(samples), using_screen(screen),
          is_media(media), configured(is_configured) {}
};

inline bool standalone_audio_candidate(const AirPlayAudioIntent& intent, bool mirror_active) {
    const bool decodable = intent.codec == 1 || intent.codec == 2 || intent.codec == 4 || intent.codec == 8;
    return intent.configured && decodable && !intent.using_screen && !mirror_active;
}

inline bool standalone_audio_yield_to_video(bool audio_only_session, bool video_packet) {
    return audio_only_session && video_packet;
}

// FLUSH invalidates copied/dequeued audio as well as queued packets.
inline bool current_audio_epoch(unsigned packet,unsigned current) { return packet==current; }
inline void apply_audio_flush(bool audio_only,bool pending_video,bool& have_pending,
                              int64_t& retry_since,bool& flush_paused) {
    if(have_pending && !pending_video)have_pending=false;
    if(audio_only || !pending_video)retry_since=0;
    if(audio_only)flush_paused=true;
}

inline bool feed_retry_expired(int64_t since,int64_t now) {
    return since!=0 && now-since>3000000000LL;
}
