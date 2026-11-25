#include "direct_bufferstream.h"
#include "audio.h"
#include "identity.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int64_t second = 1000000000;
constexpr int64_t feed_lead = 300000000;
constexpr int64_t startup_timeout = 20 * second;
constexpr int64_t preroll_delay = 200000000;
constexpr int64_t preroll_duration = 2 * second;
constexpr size_t preroll_bytes = 8 * 1024 * 1024;
enum Track { Video, Audio };
constexpr unsigned preroll_packets[] = {60, 100};

DirectError fetch_error(const HlsFetch& fetch) {
    if (fetch.cancelled) return DirectError::FetchCancelled;
    return fetch.limit_exceeded ? DirectError::StreamLimit : DirectError::FetchFailed;
}

void log_segment(const DirectDemuxedSegment& media, DirectError result, size_t number) {
    std::fprintf(stderr, "[direct-hls] stage=segment-demux segment=%zu reason=%s container=%s probe-score=%d "
        "video-codec=%d audio-codec=%d h264-profile=%d pixel-format=%s geometry=%dx%d aac-rate=%d aac-channels=%d "
        "video-packets=%u audio-packets=%u idr=%d first-idr=%d sps-pps=%d first-video-au=%d first-pcm-bytes=%zu aac-without-pcm=%u "
        "timestamps=%s timestamp-problem=%s pts-before-dts=%u tied-dts=%u inferred-first-dts=%u ffmpeg-timestamp-warnings=%u\n",
        number, direct_error_name(result), media.container, media.probe_score, media.video_codec, media.audio_codec,
        media.h264_profile, media.pixel_format, media.width, media.height, media.aac_sample_rate, media.aac_channels,
        media.video_packets, media.audio_packets, int(media.idr), int(media.starts_with_idr), int(media.sps_pps),
        int(media.video_packets > 0), media.first_pcm_bytes, media.aac_without_pcm,
        result == DirectError::VideoTimestamp || result == DirectError::AudioTimestamp ? "invalid" :
            result == DirectError::None ? "valid" : "incomplete",
        media.timestamp_problem, media.reordered, media.tied_dts, media.inferred_dts, media.ffmpeg_timestamp_warnings);
}

bool fcup_enabled() {
    const char* value = std::getenv("UXAIR_DIRECT_FCUP");
    return value && std::string(value) == "1";
}
}

DirectBufferstream::~DirectBufferstream() { close(); }

bool DirectBufferstream::open(const DirectCommand& play, int64_t now, DirectSession* session) {
    const bool released = close();
    status_ = DirectSnapshot();
    status_.backend = DirectBackend::Bufferstream;
    status_.state = DirectState::Loading;
    status_.stage = DirectStage::ManifestFetch;
    opened_at_ = now;
    if (!released) return fail(DirectError::CleanupFailed);
    if (!network_.begin(!play.encrypted)) return fail(DirectError::RelayStart);
    session_ = session;
    if (session && !play.encrypted && fcup_enabled()) {
        network_.sender_manifest = [session](const std::string& url) { return session->sender_manifest(url); };
    }
    play_ = play;
    cancelled_ = false;
    finished_ = false;
    download_error_ = DirectError::None;
    download_stage_ = DirectStage::ManifestFetch;
    duration_ns_ = 0;
    try {
        worker_ = std::thread(&DirectBufferstream::download, this, play);
    } catch (...) {
        close();
        return fail(DirectError::BufferstreamStart);
    }
    return true;
}

void DirectBufferstream::download(DirectCommand play) {
    try {
        DirectPlaylist playlist;
        for (unsigned level = 0; ; ++level) {
            download_stage_ = DirectStage::ManifestFetch;
            const auto manifest = network_.fetch(play.uri, 256 * 1024, true);
            if (cancelled_) return;
            if (manifest.status != 200) { download_error_ = fetch_error(manifest); return; }
            download_stage_ = DirectStage::PlaylistParse;
            const auto parsed = direct_parse_playlist(manifest.data, manifest.url, playlist);
            const char* type = !playlist.variant.empty() || parsed == DirectError::PlaylistVariant ? "master" :
                !playlist.segments.empty() ? "media" : "unknown";
            std::fprintf(stderr, "[direct-hls] stage=playlist-parse depth=%u playlist=%s segments=%zu reason=%s tag=%s\n",
                         level, type, playlist.segments.size(), direct_error_name(parsed),
                         playlist.diagnostic_tag.empty() ? "none" : playlist.diagnostic_tag.c_str());
            if (parsed != DirectError::None) { download_error_ = parsed; return; }
            if (playlist.variant.empty()) break;
            if (level == 1) { download_error_ = DirectError::PlaylistNested; return; }
            play.uri = playlist.variant;
        }
        duration_ns_ = playlist.duration_ns;
        const double requested = play.position_seconds ? play.position * second : play.position * playlist.duration_ns;
        const int64_t target = int64_t(std::max(0.0, std::min(requested, double(playlist.duration_ns - 1))));
        auto segment = std::upper_bound(playlist.segments.begin(), playlist.segments.end(), target,
            [](int64_t time, const DirectSegment& item) { return time < item.start_ns; });
        --segment;
        AudioDecoder audio;
        int64_t origin = 0, previous_dts = 0;
        unsigned prepared = 0;
        int width = 0, height = 0, aac_rate = 0, aac_channels = 0;
        for (; segment != playlist.segments.end(); ++segment) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // A timed wait uses the pthread ABI supported by the B7 toolchain.
                while (!cancelled_ && pending_) slot_.wait_for(lock, std::chrono::milliseconds(250));
                if (cancelled_) return;
            }

            download_stage_ = DirectStage::SegmentFetch;
            const auto response = network_.fetch(segment->url, 8 * 1024 * 1024);
            if (cancelled_) return;
            if (response.status != 200) { download_error_ = fetch_error(response); return; }
            download_stage_ = DirectStage::SegmentDemux;
            std::unique_ptr<Job> job(new Job);
            const auto result = direct_demux_ts(response.data, cancelled_, audio, width, height, job->segment);
            if (cancelled_) return;
            const auto& media = job->segment;
            if (prepared < 3 || result != DirectError::None)
                log_segment(media, result, size_t(segment - playlist.segments.begin()));
            if (result != DirectError::None) { download_error_ = result; return; }
            ++prepared;
            download_stage_ = DirectStage::SegmentTimeline;
            if (!width) {
                origin = media.packets.front().dts_ns;
                for (const auto& packet : media.packets) origin = std::min(origin, packet.pts_ns);
                width = media.width;
                height = media.height;
                aac_rate = media.aac_sample_rate;
                aac_channels = media.aac_channels;
            } else {
                if (media.aac_sample_rate != aac_rate || media.aac_channels != aac_channels) {
                    download_error_ = DirectError::TrackChange;
                    return;
                }
                const auto gap = media.first_video_dts_ns - previous_dts;
                if (gap <= 0 || gap > 250000000) {
                    download_error_ = DirectError::Discontinuity;
                    return;
                }
            }
            previous_dts = media.last_video_dts_ns;
            job->position_ns = segment->start_ns + std::min(media.skipped_ns, segment->duration_ns);
            job->decoder_origin_ns = origin;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_ = std::move(job);
            }
        }
        finished_ = true;
    } catch (const std::bad_alloc&) {
        download_error_ = DirectError::StreamLimit;
    } catch (const std::exception&) {
        download_error_ = DirectError::DownloadWorker;
    }
}

bool DirectBufferstream::take_next() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(pending_);
    for (auto& track : tracks_) track.index = 0;
    slot_.notify_one();
    return bool(current_);
}

bool DirectBufferstream::fail(DirectError error) {
    status_.state = DirectState::Error;
    status_.error = error;
    std::fprintf(stderr, "[direct-bufferstream] stage=%s error=%s accepted-video=%llu accepted-audio=%llu\n",
        direct_stage_name(status_.stage), direct_error_name(error),
        static_cast<unsigned long long>(opened_ ? media_.feed_video_accepted.load() : 0),
        static_cast<unsigned long long>(opened_ ? media_.feed_audio_accepted.load() : 0));
    if (opened_ && !presented_) {
        std::fprintf(stderr, "[direct-player] stage=startup-failure owner=ui ready=%d play-attempts=%u play-accepted=%d playing=%d frames=%u preroll-bytes=%zu\n",
            int(media_.ready.load()), media_.play_attempts(), int(media_.play_accepted()), int(media_.playing.load()),
            media_.frames.load(), preroll_bytes_);
        for (unsigned kind = 0; kind < tracks_.size(); ++kind) {
            const auto& track = tracks_[kind];
            std::fprintf(stderr, "[direct-player] stage=feed-summary kind=%s attempts=%u accepted=%u retries=%u rejected=%u bytes=%zu pts-min-ns=%lld pts-max-ns=%lld\n",
                kind == Video ? "video" : "audio", track.attempts, track.accepted, track.retries, track.rejected,
                track.bytes, (long long)track.min_pts, (long long)track.max_pts);
        }
    }
    return false;
}

DirectError DirectBufferstream::startup_error() const {
    if (!opened_) return DirectError::StartupFetchTimeout;
    if (!tracks_[Video].accepted || !tracks_[Audio].accepted) return DirectError::InitialFeedTimeout;
    if (!media_.ready) return DirectError::LoadTimeout;
    if (!media_.play_accepted()) return DirectError::PlayRejected;
    if (!media_.playing) return DirectError::PlayingTimeout;
    return DirectError::FirstFrameTimeout;
}

void DirectBufferstream::feed_packets(int64_t now) {
    const bool preroll = !presented_;
    if (preroll && now < preroll_at_) return;
    const int64_t limit = preroll ? std::min(preroll_duration, now - preroll_at_ + feed_lead) :
        clock_ns_ + (paused_ || buffering_ ? 0 : now - clock_at_) + feed_lead;
    const auto& packets = current_->segment.packets;
    std::array<bool, 2> retry{};
    for (unsigned count = 0; count < 24; ++count) {
        // A full audio queue cannot prevent the decoder from receiving its IDR.
        // Each track still consumes its packets in decode order.
        unsigned next = tracks_.size();
        for (unsigned kind = 0; kind < tracks_.size(); ++kind) {
            auto& track = tracks_[kind];
            while (track.index < packets.size() && packets[track.index].video != (kind == Video))
                ++track.index;
            if (retry[kind] || track.index == packets.size()) continue;
            const auto& packet = packets[track.index];
            if (packet.dts_ns - current_->decoder_origin_ns > limit) continue;
            if (preroll && (track.accepted >= preroll_packets[kind] || packet.data.size() > preroll_bytes - preroll_bytes_))
                continue;
            if (preroll && kind == Video && !track.attempts) { next = kind; break; }
            if (next == tracks_.size() || packets[tracks_[next].index].dts_ns > packet.dts_ns) next = kind;
        }
        if (next == tracks_.size()) break;
        auto& track = tracks_[next];
        const auto& packet = packets[track.index];
        const auto pts = packet.pts_ns - current_->decoder_origin_ns;
        const auto dts = packet.dts_ns - current_->decoder_origin_ns;
        status_.stage = preroll ? DirectStage::Preroll : DirectStage::PacketFeed;
        const auto result = media_.feed(packet.data.data(), packet.data.size(), pts, packet.video);
        ++track.attempts;
        if (result == FeedResult::Retry) ++track.retries;
        if (result == FeedResult::Error) ++track.rejected;
        if (track.attempts == 1 || (result == FeedResult::Accepted && !track.accepted) ||
            (result == FeedResult::Error && preroll)) {
            size_t queued = 0;
            for (size_t i = 0; i < packets.size(); ++i)
                queued += i >= tracks_[packets[i].video ? Video : Audio].index;
            std::fprintf(stderr, "[direct-player] stage=%s owner=ui kind=%s result=%s attempts=%u accepted=%u rejected=%u retries=%u bytes=%zu queued=%zu pts-ns=%lld dts-ns=%lld ready=%d playing=%d\n",
                result == FeedResult::Accepted ? "first-feed-accepted" : "feed-attempt", packet.video ? "video" : "audio",
                result == FeedResult::Accepted ? "accepted" : result == FeedResult::Retry ? "backpressure" : "rejected",
                track.attempts, track.accepted + unsigned(result == FeedResult::Accepted), track.rejected, track.retries,
                packet.data.size(), queued, (long long)pts, (long long)dts,
                int(media_.ready.load()), int(media_.playing.load()));
        }
        if (result == FeedResult::Retry) {
            retry[next] = true;
            if (!preroll && now - last_feed_at_ > 5 * second) { fail(DirectError::ProgressTimeout); return; }
            continue;
        }
        if (result != FeedResult::Accepted) { fail(DirectError::BufferstreamFeed); return; }
        if (!track.accepted++) track.min_pts = track.max_pts = pts;
        track.min_pts = std::min(track.min_pts, pts);
        track.max_pts = std::max(track.max_pts, pts);
        track.bytes += packet.data.size();
        if (preroll) preroll_bytes_ += packet.data.size();
        accepted_until_ns_ = std::max(accepted_until_ns_, std::max(pts, dts) + packet.duration_ns);
        last_feed_at_ = now;
        ++track.index;
    }
    for (unsigned kind = 0; kind < tracks_.size(); ++kind) {
        auto& track = tracks_[kind];
        while (track.index < packets.size() && packets[track.index].video != (kind == Video)) ++track.index;
    }
    if (tracks_[Video].index == packets.size() && tracks_[Audio].index == packets.size()) current_.reset();
}

void DirectBufferstream::set_paused(bool pause, int64_t now) {
    if (paused_ == pause) return;
    if (pause && clock_at_ && !buffering_) clock_ns_ += now - clock_at_;
    clock_at_ = now;
    paused_ = pause;
    if (opened_ && presented_ && !buffering_) media_.pause(pause);
}

bool DirectBufferstream::control(const DirectCommand& command, int64_t now) {
    if (command.kind == DirectCommandKind::Pause || command.kind == DirectCommandKind::Resume) {
        set_paused(command.kind == DirectCommandKind::Pause, now);
        return true;
    }
    if (command.kind == DirectCommandKind::Seek) {
        DirectCommand restarted = play_;
        restarted.position = command.position;
        restarted.position_seconds = true;
        return open(restarted, now, session_);
    }
    return false;
}

void DirectBufferstream::update_snapshot() {
    status_.seekable=presented_&&duration_ns_>0;
    status_.backend_loaded=opened_&&media_.ready;
    status_.frame_observed=opened_&&media_.frames>0;
    status_.audio_fed=opened_&&media_.feed_audio_accepted>0;
    status_.upstream_fetches = network_.requests;
    status_.downloaded_bytes = network_.bytes;
    status_.relay_failures = network_.failures;
    status_.duration = double(duration_ns_.load()) / second;
    status_.position = std::min(status_.duration, double(position_ns_ + clock_ns_) / second);
    std::lock_guard<std::mutex> lock(mutex_);
    status_.buffer_bytes = pending_ ? pending_->segment.bytes : 0;
    if (current_) for (size_t i = 0; i < current_->segment.packets.size(); ++i) {
        const auto& packet = current_->segment.packets[i];
        if (i >= tracks_[packet.video ? Video : Audio].index) status_.buffer_bytes += packet.data.size();
    }
}

DirectSnapshot DirectBufferstream::tick(int64_t now) {
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }
    const auto download_error = download_error_.load();
    if (download_error != DirectError::None) {
        status_.stage = download_stage_.load();
        fail(download_error);
        update_snapshot();
        return status_;
    }

    // Take the last published batch even when the worker just reached EOF.
    const bool more_available = current_ || take_next();
    if (!opened_ && !more_available) status_.stage = download_stage_.load();
    if (!opened_ && more_available) {
        status_.stage = DirectStage::BufferstreamLoad;
        status_.width = current_->segment.width;
        status_.height = current_->segment.height;
        position_ns_ = current_->position_ns;
#ifdef UXAIR_DIRECT_BUILD
        media_.guard_direct_callbacks(direct_native_json_summary);
#endif
        const auto& media = current_->segment;
        const auto video_span = media.last_video_dts_ns - media.first_video_dts_ns;
        const int fps = media.video_packets > 1 && video_span < 25000000LL * (media.video_packets - 1) ? 60 : 30;
        std::fprintf(stderr, "[direct-player] stage=create owner=ui video=H264 max-fps=%d pcm-hz=44100 pcm-channels=2 pcm-format=S16LE\n", fps);
        opened_ = media_.open(UXAIR_APP_ID, status_.width, status_.height, true, 44100,
                              2 * 1024 * 1024, 1920, 1080, MirrorCodec::H264, fps);
        if (!opened_) fail(DirectError::BufferstreamStart);
        // The next tick's time is after the synchronous Load() call.
        update_snapshot();
        return status_;
    }
    if (opened_) {
        if (!preroll_at_) {
            preroll_at_ = now + preroll_delay;
            std::fprintf(stderr, "[direct-player] stage=feed-ready owner=ui gate=load-return delay-ms=200 ready=%d playing=%d\n",
                         int(media_.ready.load()), int(media_.playing.load()));
        }
        media_.tick();
    }
    if (opened_ && media_.failed) fail(DirectError::NativeError);
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }

    if (buffering_ && more_available) {
        buffering_ = false;
        clock_at_ = now;
        if (!paused_) media_.pause(false);
    }
    if (opened_ && more_available && (!paused_ || !presented_)) feed_packets(now);
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }

    if (opened_ && media_.ready && media_.playing && !presented_ &&
        media_.frames && media_.feed_video_accepted && media_.feed_audio_accepted) {
        presented_ = true;
        started_at_ = clock_at_ = now;
        if (paused_) media_.pause(true);
        std::fprintf(stderr, "[direct-bufferstream] decoder-progress geometry=%ux%u video=H264 audio=AAC-LC\n",
                     status_.width, status_.height);
    }
    if (!presented_ && now - opened_at_ > startup_timeout) {
        const auto reason = startup_error();
        if (reason == DirectError::InitialFeedTimeout) status_.stage = DirectStage::Preroll;
        else if (reason == DirectError::LoadTimeout) status_.stage = DirectStage::AwaitLoad;
        else if (reason == DirectError::PlayRejected) status_.stage = DirectStage::Play;
        else if (reason == DirectError::PlayingTimeout) status_.stage = DirectStage::AwaitPlaying;
        else if (reason == DirectError::FirstFrameTimeout) status_.stage = DirectStage::AwaitFirstFrame;
        fail(reason);
        update_snapshot();
        return status_;
    }

    if (presented_ && !paused_ && !buffering_) {
        clock_ns_ += now - clock_at_;
        clock_at_ = now;
        const auto frame = media_.last_frame_event_ns.load();
        if (!eos_ && now - std::max(started_at_, frame) > 8 * second) fail(DirectError::ProgressTimeout);
    }

    if (presented_ && !current_) {
        if (finished_) {
            if (!take_next() && !eos_) {
                eos_ = true;
                eos_at_ = now;
                media_.eos();
            }
        } else if (!paused_ && !buffering_ && clock_ns_ >= accepted_until_ns_) {
            buffering_ = true;
            media_.pause(true);
        }
    }
    if (status_.state != DirectState::Error) {
        if (eos_ && media_.ended) {
            status_.state = DirectState::Ended;
            clock_ns_ = std::max<int64_t>(0, duration_ns_ - position_ns_);
        } else if (eos_ && !paused_ && now - eos_at_ > 5 * second) {
            fail(DirectError::ProgressTimeout);
        } else if (paused_ && presented_) {
            status_.state = DirectState::Paused;
        } else if (buffering_) {
            status_.state = DirectState::Buffering;
        } else {
            status_.state = presented_ ? DirectState::Playing : DirectState::Loading;
        }
    }
    update_snapshot();
    return status_;
}

bool DirectBufferstream::close() {
    cancelled_ = true;
    network_.cancel();
    if (session_) session_->cancel_fetch();
    slot_.notify_all();
    if (worker_.joinable()) worker_.join();
    network_.sender_manifest = {};
    network_.stop();
    session_ = nullptr;
    pending_.reset();
    current_.reset();
    media_.close();
    opened_ = presented_ = paused_ = buffering_ = eos_ = false;
    tracks_ = {};
    preroll_bytes_ = 0;
    preroll_at_ = clock_at_ = clock_ns_ = position_ns_ = accepted_until_ns_ = last_feed_at_ = started_at_ = eos_at_ = 0;
#ifdef UXAIR_DIRECT_BUILD
    return media_.direct_released();
#else
    return true;
#endif
}
