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
    };
    void download(DirectCommand play);
    bool take_next();
    void feed_packets(int64_t now);
    DirectError startup_error() const;
    bool fail(DirectError error);
    void set_paused(bool pause, int64_t now);
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
    };
    std::array<FeedTrack, 2> tracks_{};
    size_t preroll_bytes_ = 0;
    bool opened_ = false, presented_ = false, paused_ = false, buffering_ = false, eos_ = false;
    int64_t opened_at_ = 0, preroll_at_ = 0, clock_at_ = 0, clock_ns_ = 0, position_ns_ = 0;
    int64_t accepted_until_ns_ = 0, last_feed_at_ = 0, started_at_ = 0, eos_at_ = 0;
};
