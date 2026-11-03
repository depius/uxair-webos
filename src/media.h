#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include "settings.h"
#include "media_open_failure.h"
// Two players, chosen at build time (CMake UXAIR_MEDIA):
//  - starfish (webOS 3-4): Starfish media pipeline + ACB, src/media_starfish.cpp
//  - ndl (webOS 5+, the only one on webOS 10): LG's NDL direct media API,
//    src/media_ndl.cpp + src/ndl_backend.c. The Starfish/ACB names in the
//    counters are kept for the UI and logs: acb_window_* is the video
//    window, frames counts accepted video frames.
#if !defined(UXAIR_MEDIA_NDL)
class StarfishMediaAPIs;
#endif
enum class FeedResult { Accepted, Retry, Error };
class Media {
public:
    Media();
    ~Media();
    bool open(const char *app_id, int width, int height, bool audio, int sample_rate = 44100,
              int video_source_buffer_bytes = 1048576, int canvas_width = 0, int canvas_height = 0,
              MirrorCodec codec = MirrorCodec::H264, int max_fps = 30, bool audio_only = false);
    FeedResult feed(const void *data, size_t size, int64_t pts_ns, bool video);
    void tick();
    void eos();
    void pause(bool paused);
    void close();
    MediaOpenFailure open_failure() const { return open_failure_; }
    std::atomic<bool> ready{false}, playing{false}, ended{false}, failed{false};
    std::atomic<unsigned> frames{0};
    std::atomic<int64_t> last_frame_event_ns{0};
    std::atomic<int> video_info_width{0}, video_info_height{0};
    std::atomic<int> acb_window_width{0}, acb_window_height{0};
    std::atomic<uint64_t> feed_video_accepted{0}, feed_audio_accepted{0};
    std::atomic<uint64_t> feed_video_retries{0}, feed_audio_retries{0};
    std::atomic<uint64_t> feed_video_rejected{0};
    std::atomic<uint64_t> feed_calls{0};
    std::atomic<uint64_t> feed_time_ns{0}, feed_max_time_ns{0};
private:
#if defined(UXAIR_MEDIA_NDL)
    static void event(void *context, int type, long long value, const char *message);
    bool loaded_ = false;
    std::string window_id_;          // SDL exported window the video is bound to
    unsigned video_failures_ = 0;    // refused video frames in a row
#else
    static void event(int type, int64_t value, const char *message, void *user);
    StarfishMediaAPIs *api_ = nullptr;
    long acb_ = 0, task_ = 0;
    bool loaded_ = false, sink_attached_ = false, play_requested_ = false, acb_playing_ = false;
    unsigned play_attempts_ = 0;
    std::atomic<bool> video_info_pending_{false};
    // Firmware callbacks and Feed run on separate threads.
    struct Impl;
    Impl *impl_;
#endif
    MediaOpenFailure open_failure_ = MediaOpenFailure::None;
};
