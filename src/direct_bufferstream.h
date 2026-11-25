#pragma once

#include "direct_hls.h"
#include "direct_video.h"
#include "hls_relay.h"
#include "media.h"

#include <condition_variable>
#include <array>
#include <memory>
#include <mutex>
#include <thread>

class DirectBufferstream {
public:
    ~DirectBufferstream();
    bool open(const DirectCommand& play, int64_t now, DirectSession* session);
    bool control(const DirectCommand& command, int64_t now);
    DirectSnapshot tick(int64_t now);
    bool close();

private:
    // One segment being fed and one being prepared. The worker waits before
    // allocating another segment until the slot has been taken by tick().
    struct Job {
        DirectDemuxedSegment segment;
        int64_t position_ns = 0;
        int64_t decoder_origin_ns = 0;
        bool decoder_boundary = false;
    };
    void download(DirectCommand play);
    bool take_next();
    void feed_packets(int64_t now);
    DirectError startup_error() const;
    bool fail(DirectError error);
    void set_paused(bool pause, DirectOrigin origin, int64_t now);
    void backend_pause(bool pause, DirectOrigin origin);
    void start_running(int64_t now);
    void progress_log(const char* stage, const char* reason, int64_t now);
    void update_snapshot();

    HlsRelay network_;
    DirectSession* session_ = nullptr;
    Media media_;
    DirectCommand play_;
    DirectSnapshot status_;
    std::thread worker_;
    std::atomic<bool> cancelled_{true}, finished_{false};
    std::atomic<int64_t> duration_ns_{0};
    std::atomic<DirectError> download_error_{DirectError::None};
    std::atomic<DirectStage> download_stage_{DirectStage::ManifestFetch};
    std::mutex mutex_;
    std::condition_variable slot_;
    std::unique_ptr<Job> pending_, current_;
    struct FeedTrack {
        size_t index = 0, bytes = 0;
        unsigned attempts = 0, accepted = 0, retries = 0, rejected = 0;
        int64_t min_pts = 0, max_pts = 0;
        int64_t last_pts = 0, last_dts = 0, last_accepted = 0, buffered_until = 0;
    };
    std::array<FeedTrack, 2> tracks_{};
    size_t preroll_bytes_ = 0;
    bool opened_ = false, presented_ = false, paused_ = false, buffering_ = false, eos_ = false;
    DirectOrigin pause_origin_ = DirectOrigin::None;
    uint64_t decoder_generation_ = 0;
    unsigned control_records_ = 0, progress_records_ = 0, reported_frames_ = 1, audio_underruns_ = 0;
    std::atomic<unsigned> segments_prepared_{0}, video_demuxed_{0}, audio_demuxed_{0};
    int64_t opened_at_ = 0, preroll_at_ = 0, clock_at_ = 0, clock_ns_ = 0, position_ns_ = 0;
    int64_t last_feed_at_ = 0, running_since_ = 0, eos_at_ = 0, next_progress_ns_ = 0;
};
