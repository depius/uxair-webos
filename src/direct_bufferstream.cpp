#include "direct_bufferstream.h"
#include "audio.h"
#include "identity.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int64_t second = 1000000000;
constexpr int64_t feed_lead = 300000000;
constexpr int64_t startup_timeout = 20 * second;
constexpr int64_t preroll_delay = 200000000;
constexpr int64_t preroll_duration = 2 * second;
constexpr int64_t feed_timeout = 5 * second;
constexpr int64_t frame_timeout = 8 * second;
constexpr int64_t startup_seek_quiet = 120000000;
constexpr int64_t startup_seek_max_wait = 400000000;
constexpr size_t preroll_bytes = 8 * 1024 * 1024;
enum Track { Video, Audio };
constexpr unsigned preroll_packets[] = {60, 100};

DirectError fetch_error(const HlsFetch& fetch) {
    if (fetch.cancelled) return DirectError::FetchCancelled;
    if (fetch.range_rejected) return DirectError::RangeResponse;
    return fetch.limit_exceeded ? DirectError::StreamLimit : DirectError::FetchFailed;
}

void log_segment(const DirectDemuxedSegment& media, DirectError result, size_t number) {
    std::fprintf(stderr, "[direct-hls] stage=%s segment=%zu reason=%s container=%s probe-score=%d "
        "video-codec=%d audio-codec=%d h264-profile=%d pixel-format=%s geometry=%dx%d aac-rate=%d aac-channels=%d "
        "video-packets=%u audio-packets=%u idr=%d first-idr=%d sps-pps=%d first-video-au=%d first-pcm-bytes=%zu aac-without-pcm=%u "
        "timestamps=%s timestamp-problem=%s pts-before-dts=%u tied-dts=%u inferred-first-dts=%u ffmpeg-timestamp-warnings=%u "
        "tracks=%u fragments=%u h264-level=%d\n",
        media.container[0] == 'f' ? "fragment-demux" : "segment-demux",
        number, direct_error_name(result), media.container, media.probe_score, media.video_codec, media.audio_codec,
        media.h264_profile, media.pixel_format, media.width, media.height, media.aac_sample_rate, media.aac_channels,
        media.video_packets, media.audio_packets, int(media.idr), int(media.starts_with_idr), int(media.sps_pps),
        int(media.video_packets > 0), media.first_pcm_bytes, media.aac_without_pcm,
        result == DirectError::VideoTimestamp || result == DirectError::AudioTimestamp ? "invalid" :
            result == DirectError::None ? "valid" : "incomplete",
        media.timestamp_problem, media.reordered, media.tied_dts, media.inferred_dts, media.ffmpeg_timestamp_warnings,
        media.tracks, media.fragments, media.h264_level);
}

bool fcup_enabled() {
    const char* value = std::getenv("UXAIR_DIRECT_FCUP");
    return value && std::string(value) == "1";
}
}

DirectBufferstream::~DirectBufferstream() { close(); }

bool DirectBufferstream::open(const DirectCommand& play, int64_t now, DirectSession* session) {
    const auto prior_pause = pause_origin_;
    const bool released = close();
    status_ = DirectSnapshot();
    status_.backend = DirectBackend::Bufferstream;
    status_.state = DirectState::Loading;
    status_.stage = DirectStage::ManifestFetch;
    status_.generation = play.generation;
    opened_at_ = now;
    if (!released) return fail(DirectError::CleanupFailed);
    if (!network_.begin(!play.encrypted)) return fail(DirectError::RelayStart);
    session_ = session;
    if (session && !play.encrypted && fcup_enabled()) {
        network_.sender_manifest = [session](const std::string& url) { return session->sender_manifest(url); };
    }
    if (play.generation != play_.generation || play.origin != DirectOrigin::SenderSeek)
        replacement_reported_ = false;
    play_ = play;
    ++decoder_generation_;
    paused_ = play.start_paused;
    pause_origin_ = !paused_ ? DirectOrigin::None :
        play.origin == DirectOrigin::SenderSeek ? prior_pause : DirectOrigin::InitialRate;
    fprintf(stderr, "[direct-session] stage=decoder-queued generation=%llu decoder=%llu reason=%s requested-rate=%d\n",
            (unsigned long long)play.generation,(unsigned long long)decoder_generation_,
            direct_origin_name(play.origin),int(!paused_));
    cancelled_ = false;
    finished_ = false;
    download_error_ = DirectError::None;
    download_stage_ = DirectStage::ManifestFetch;
    duration_ns_ = 0;
    segments_prepared_=video_demuxed_=audio_demuxed_=0;
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
        fprintf(stderr,"[direct-session] stage=playlist-ready generation=%llu segments=%zu\n",
                (unsigned long long)play.generation,playlist.segments.size());
        duration_ns_ = playlist.duration_ns;
        const double requested = play.position_seconds ? play.position * second : play.position * playlist.duration_ns;
        const int64_t target = int64_t(std::max(0.0, std::min(requested, double(playlist.duration_ns - 1))));
        auto segment = std::upper_bound(playlist.segments.begin(), playlist.segments.end(), target,
            [](int64_t time, const DirectSegment& item) { return time < item.start_ns; });
        --segment;
        AudioDecoder audio;
        int64_t origin = 0, previous_dts = 0, previous_audio_pts = 0;
        unsigned prepared = 0;
        int width = 0, height = 0, aac_rate = 0, aac_channels = 0;
        unsigned loaded_map=0, loaded_period=0;
        std::string init;
        for (; segment != playlist.segments.end(); ++segment) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // A timed wait uses the pthread ABI supported by the B7 toolchain.
                while (!cancelled_ && pending_) slot_.wait_for(lock, std::chrono::milliseconds(250));
                if (cancelled_) return;
            }

            bool boundary=prepared&&segment->period!=loaded_period;
            if (segment->init!=loaded_map) {
                download_stage_=DirectStage::InitSegment;
                const auto& map=playlist.maps[segment->init-1];
                auto response=network_.fetch(map.url,1024*1024,false,map.range);
                if(cancelled_) return;
                const auto error=response.status!=200
                    ? (response.range_rejected||response.limit_exceeded?fetch_error(response):DirectError::InitFetch)
                    : response.data.empty()?DirectError::PlaylistInitSegment:DirectError::None;
                if(prepared<3||error!=DirectError::None)
                    fprintf(stderr,"[direct-hls] stage=init-segment generation=%llu map=%u bytes=%zu range=%d reason=%s\n",
                            (unsigned long long)play.generation,segment->init,response.data.size(),
                            int(map.range.length!=0),direct_error_name(error));
                if(error!=DirectError::None){download_error_=error;return;}
                boundary|=prepared&&init!=response.data;
                init.swap(response.data);
                loaded_map=segment->init;
            }
            loaded_period=segment->period;
            if (boundary) {
                audio.reset();
                width=height=aac_rate=aac_channels=0;
            }
            download_stage_ = DirectStage::SegmentFetch;
            const auto response = network_.fetch(segment->url, 8 * 1024 * 1024,false,segment->range);
            if (cancelled_) return;
            if (response.status != 200) { download_error_ = fetch_error(response); return; }
            download_stage_ = DirectStage::SegmentDemux;
            std::unique_ptr<Job> job(new Job);
            const auto result = direct_demux_segment(response.data,init,cancelled_,audio,width,height,job->segment);
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
                if (!init.empty()) {
                    const auto first_audio=std::find_if(media.packets.begin(),media.packets.end(),[](const DirectAccessUnit& packet){return !packet.video;});
                    const auto audio_gap=first_audio->pts_ns-previous_audio_pts;
                    if(audio_gap<=0||audio_gap>250000000) { download_error_=DirectError::AudioTimestamp;return; }
                }
            }
            if (!init.empty()) {
                const auto last_audio=std::find_if(media.packets.rbegin(),media.packets.rend(),[](const DirectAccessUnit& packet){return !packet.video;});
                previous_audio_pts=last_audio->pts_ns;
            }
            previous_dts = media.last_video_dts_ns;
            job->position_ns = segment->start_ns + std::min(media.skipped_ns, segment->duration_ns);
            job->interval_start_ns = segment->start_ns;
            job->interval_end_ns = segment->start_ns + segment->duration_ns;
            job->decoder_origin_ns = origin;
            job->decoder_boundary=boundary;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                video_demuxed_ += media.video_packets;
                audio_demuxed_ += media.audio_packets;
                pending_ = std::move(job);
                ++segments_prepared_;
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

void DirectBufferstream::start_running(int64_t now) {
    clock_at_ = now;
    running_since_ = now;
}

void DirectBufferstream::backend_pause(bool pause, DirectOrigin origin) {
#ifdef UXAIR_DIRECT_BUILD
    const bool accepted = media_.direct_set_paused(pause);
#else
    media_.pause(pause);
    const bool accepted = true;
#endif
    if (control_records_++ < 32 || !accepted)
        fprintf(stderr,"[direct-session] stage=backend-rate generation=%llu decoder=%llu initiator=%s requested-rate=%d accepted=%d\n",
                (unsigned long long)status_.generation,(unsigned long long)decoder_generation_,
                direct_origin_name(origin),int(!pause),int(accepted));
    if (!accepted) fail(DirectError::ControlRejected);
}

void DirectBufferstream::progress_log(const char* stage, const char* reason, int64_t now) {
    std::array<size_t, 2> queued{};
    std::array<int64_t, 2> next_dts{{-1, -1}};
    if (current_) {
        const auto& packets = current_->segment.packets;
        for (size_t i = 0; i < packets.size(); ++i) {
            const auto kind = packets[i].video ? Video : Audio;
            if (i >= tracks_[kind].index) {
                if (!queued[kind]) next_dts[kind] = packets[i].dts_ns - current_->decoder_origin_ns;
                ++queued[kind];
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            queued[Video] += pending_->segment.video_packets;
            queued[Audio] += pending_->segment.audio_packets;
        }
    }
    const auto frame = media_.last_frame_event_ns.load();
    const auto age_ms = [now](int64_t at) { return at ? (long long)std::max(int64_t(0),now-at)/1000000 : -1LL; };
    const auto& video = tracks_[Video];
    const auto& audio = tracks_[Audio];
#ifdef UXAIR_DIRECT_BUILD
    const bool backend_paused = media_.direct_backend_paused();
#else
    const bool backend_paused = false;
#endif
    const auto cover = [this](const FeedTrack& track) { return (long long)std::max(int64_t(0),track.buffered_until-clock_ns_)/1000000; };
    fprintf(stderr,"[direct-session] stage=%s reason=%s generation=%llu decoder=%llu desired-rate=%d pause=%s backend=%s "
            "frames=%u frame-age-ms=%lld feed-age-ms=%lld active-ms=%lld playhead-ms=%lld video-buffer-ms=%lld audio-buffer-ms=%lld "
            "segments-prepared=%u v-demuxed=%u a-demuxed=%u fetch=%s fetch-done=%d v-queue=%zu a-queue=%zu v-submitted=%u a-submitted=%u v-accepted=%u a-accepted=%u "
            "v-retries=%u a-retries=%u next-v-dts-ns=%lld next-a-dts-ns=%lld last-v-dts-ns=%lld last-v-pts-ns=%lld last-a-pts-ns=%lld audio-underruns=%u\n",
            stage,reason,(unsigned long long)status_.generation,(unsigned long long)decoder_generation_,
            int(!paused_),direct_origin_name(paused_?pause_origin_:buffering_?DirectOrigin::Buffering:DirectOrigin::None),
            backend_paused?"paused":media_.playing?"playing":media_.ready?"ready":"starting",
            media_.frames.load(),age_ms(frame),age_ms(last_feed_at_),age_ms(running_since_),(long long)(clock_ns_/1000000),
            cover(video),cover(audio),segments_prepared_.load(),video_demuxed_.load(),audio_demuxed_.load(),
            direct_stage_name(download_stage_.load()),int(finished_.load()),
            queued[Video],queued[Audio],video.attempts,audio.attempts,video.accepted,audio.accepted,video.retries,audio.retries,
            (long long)next_dts[Video],(long long)next_dts[Audio],
            (long long)video.last_dts,(long long)video.last_pts,(long long)audio.last_pts,audio_underruns_);
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
            if (!preroll && now - std::max(running_since_,track.last_accepted) > feed_timeout) {
                progress_log("stall",packet.video?"video-backpressure":"audio-backpressure",now);
                fail(DirectError::ProgressTimeout); return;
            }
            continue;
        }
        if (result != FeedResult::Accepted) { fail(DirectError::BufferstreamFeed); return; }
        if (!track.accepted++) track.min_pts = track.max_pts = pts;
        track.min_pts = std::min(track.min_pts, pts);
        track.max_pts = std::max(track.max_pts, pts);
        track.bytes += packet.data.size();
        if (preroll) preroll_bytes_ += packet.data.size();
        track.buffered_until = std::max(track.buffered_until, std::max(pts, dts) + packet.duration_ns);
        track.last_pts = pts;
        track.last_dts = dts;
        track.last_accepted = last_feed_at_ = now;
        ++track.index;
    }
    for (unsigned kind = 0; kind < tracks_.size(); ++kind) {
        auto& track = tracks_[kind];
        while (track.index < packets.size() && packets[track.index].video != (kind == Video)) ++track.index;
    }
    if (tracks_[Video].index == packets.size() && tracks_[Audio].index == packets.size()) current_.reset();
}

void DirectBufferstream::set_paused(bool pause, DirectOrigin origin, int64_t now) {
    if (paused_ == pause) return;
    if (pause && presented_ && !buffering_) clock_ns_ += now - clock_at_;
    paused_ = pause;
    pause_origin_ = pause ? origin : DirectOrigin::None;
    if (control_records_++ < 32)
        fprintf(stderr,"[direct-session] stage=%s generation=%llu decoder=%llu initiator=%s requested-rate=%d buffered=%d\n",
                pause?"pause":"resume",(unsigned long long)status_.generation,(unsigned long long)decoder_generation_,
                direct_origin_name(origin),int(!pause),int(buffering_));
    if (opened_ && presented_ && !buffering_) {
        if (!pause) start_running(now);
        backend_pause(pause, origin);
    } else if (opened_ && !presented_) {
        if (!pause && startup_paused_) opened_at_ = now;
        if (media_.play_accepted()) backend_pause(pause, origin);
    }
}

bool DirectBufferstream::control(const DirectCommand& command, int64_t now) {
    if (command.generation && command.generation!=play_.generation) return false;
    if (command.kind == DirectCommandKind::Pause || command.kind == DirectCommandKind::Resume) {
        set_paused(command.kind == DirectCommandKind::Pause, command.origin, now);
        return true;
    }
    if (command.kind == DirectCommandKind::Seek) {
        if (presented_) return seek(command.position, now);
        seek_position_ = command.position;
        if (!seek_deadline_) seek_deadline_ = now + startup_seek_max_wait;
        seek_due_ = std::min(now + startup_seek_quiet, seek_deadline_);
        return true;
    }
    return false;
}

bool DirectBufferstream::seek(double position, int64_t now) {
    DirectCommand restarted = play_;
    restarted.position = position;
    restarted.position_seconds = true;
    restarted.start_paused = paused_;
    restarted.origin = DirectOrigin::SenderSeek;
    return open(restarted, now, session_);
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
    if (seek_due_) {
        if (now < seek_due_) { update_snapshot(); return status_; }
        seek_due_ = seek_deadline_ = 0;
        if (!current_) take_next();
        const double original = play_.position_seconds ? play_.position : play_.position * duration_ns_ / second;
        const double target = seek_position_ * second;
        const int64_t start = opened_ ? opened_interval_start_ns_ : current_ ? current_->interval_start_ns : 0;
        const int64_t end = opened_ ? opened_interval_end_ns_ : current_ ? current_->interval_end_ns : 0;
        const bool same_fragment = target >= start && target < end;
        const bool unchanged = same_fragment || ((play_.position_seconds || !play_.position || duration_ns_ > 0) &&
                                                std::abs(seek_position_ - original) < .001);
        if (control_records_++ < 32)
            fprintf(stderr, "[direct-session] stage=startup-seek generation=%llu decoder=%llu position-ms=%lld result=%s\n",
                    (unsigned long long)status_.generation, (unsigned long long)decoder_generation_,
                    (long long)(seek_position_*1000), unchanged?"retained":"restarted");
        if (!unchanged && !seek(seek_position_, now)) { update_snapshot(); return status_; }
    }
    const auto download_error = download_error_.load();
    if (download_error != DirectError::None) {
        status_.stage = download_stage_.load();
        fail(download_error);
        update_snapshot();
        return status_;
    }

    // Take the last published batch even when the worker just reached EOF.
    const bool more_available = current_ || take_next();
    if (opened_ && current_ && current_->decoder_boundary && presented_ && !paused_) {
        const auto timeline_end=std::min(current_->position_ns-position_ns_,
            std::max(tracks_[Video].buffered_until,tracks_[Audio].buffered_until));
        if (clock_ns_+(buffering_?0:now-clock_at_)>=timeline_end) {
            media_.close();
#ifdef UXAIR_DIRECT_BUILD
            if (!media_.direct_released()) {
                fail(DirectError::CleanupFailed);
                update_snapshot();
                return status_;
            }
#endif
            ++decoder_generation_;
            opened_=presented_=buffering_=eos_=false;
            tracks_={};
            preroll_bytes_=0;
            reported_frames_=1;
            clock_at_=clock_ns_=preroll_at_=last_feed_at_=running_since_=eos_at_=next_progress_ns_=0;
            opened_at_=now;
            status_.state=DirectState::Loading;
            fprintf(stderr,"[direct-session] stage=decoder-queued generation=%llu decoder=%llu reason=discontinuity\n",
                    (unsigned long long)status_.generation,(unsigned long long)decoder_generation_);
        }
    }
    if (!opened_ && !more_available) status_.stage = download_stage_.load();
    if (!opened_ && more_available) {
        status_.stage = DirectStage::BufferstreamLoad;
        status_.width = current_->segment.width;
        status_.height = current_->segment.height;
        position_ns_ = current_->position_ns;
        opened_interval_start_ns_ = current_->interval_start_ns;
        opened_interval_end_ns_ = current_->interval_end_ns;
#ifdef UXAIR_DIRECT_BUILD
        media_.guard_direct_callbacks(direct_native_json_summary);
#endif
        const auto& media = current_->segment;
        current_->decoder_boundary=false;
        const auto video_span = media.last_video_dts_ns - media.first_video_dts_ns;
        const int fps = media.video_packets > 1 && video_span < 25000000LL * (media.video_packets - 1) ? 60 : 30;
        std::fprintf(stderr, "[direct-player] stage=create owner=ui generation=%llu decoder=%llu video=H264 max-fps=%d pcm-hz=44100 pcm-channels=2 pcm-format=S16LE\n",
                     (unsigned long long)status_.generation,(unsigned long long)decoder_generation_,fps);
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
#ifdef UXAIR_DIRECT_BUILD
        media_.direct_hold_start(paused_ && !presented_);
#endif
        media_.tick();
    }
    if (opened_ && media_.failed) fail(DirectError::NativeError);
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }

    if (buffering_ && more_available) {
        buffering_ = false;
        if (!paused_) {
            start_running(now);
            backend_pause(false,DirectOrigin::Buffering);
        }
    }
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }
    if (opened_ && more_available && !current_->decoder_boundary && (!paused_ || !presented_)) feed_packets(now);
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }

    if (opened_ && media_.ready && media_.playing && !presented_ &&
        media_.frames && media_.feed_video_accepted && media_.feed_audio_accepted) {
        presented_ = true;
        start_running(now);
        if (paused_) backend_pause(true,pause_origin_);
        std::fprintf(stderr, "[direct-bufferstream] decoder-progress geometry=%ux%u video=H264 audio=AAC-LC\n",
                     status_.width, status_.height);
        if (play_.replacement && !replacement_reported_) {
            replacement_reported_ = true;
            fprintf(stderr,"[direct-session] stage=replace-completed generation=%llu decoder=%llu elapsed-ms=%lld rate=%d\n",
                    (unsigned long long)status_.generation,(unsigned long long)decoder_generation_,
                    (long long)std::max(int64_t(0),now/1000000-play_.admitted_ms),int(!paused_));
        }
    }
    startup_paused_ = paused_ && opened_ && !presented_ && media_.ready &&
                      tracks_[Video].accepted && tracks_[Audio].accepted;
    if (!presented_ && !startup_paused_ && now - opened_at_ > startup_timeout) {
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
    }

    if (presented_ && !current_) {
        if (finished_) {
            if (!take_next() && !eos_) {
                eos_ = true;
                eos_at_ = now;
                media_.eos();
            }
        } else if (!paused_ && !buffering_ && clock_ns_ >= std::min(tracks_[Video].buffered_until,tracks_[Audio].buffered_until)) {
            buffering_ = true;
            if (clock_ns_ >= tracks_[Audio].buffered_until) ++audio_underruns_;
            backend_pause(true,DirectOrigin::Buffering);
            if (control_records_ < 32) progress_log("buffering","waiting-for-segment",now);
        }
    }
    if (status_.state != DirectState::Error && presented_ && !paused_ && !buffering_ && !eos_) {
        const auto frame = media_.last_frame_event_ns.load();
        const auto frames = media_.frames.load();
        if (now - std::max(running_since_, frame) > frame_timeout) {
            const char* reason = current_?"no-decoder-frames":"no-decoder-frames-while-draining";
#ifdef UXAIR_DIRECT_BUILD
            if (media_.direct_backend_paused()) reason="backend-paused";
#endif
            progress_log("stall",reason,now);
            fail(DirectError::ProgressTimeout);
        } else if (progress_records_ < 7 && frames > reported_frames_ && clock_ns_ >= next_progress_ns_ + second) {
            progress_log("continuous-playback","decoder-callbacks",now);
            reported_frames_ = frames;
            ++progress_records_;
            next_progress_ns_ = clock_ns_ + 9 * second;
        }
    }
    if (status_.state != DirectState::Error) {
        if (eos_ && media_.ended) {
            status_.state = DirectState::Ended;
            clock_ns_ = std::max<int64_t>(0, duration_ns_ - position_ns_);
        } else if (eos_ && !paused_ && now - std::max(eos_at_,running_since_) > feed_timeout) {
            progress_log("stall","await-eos",now);
            fail(DirectError::ProgressTimeout);
        } else if (paused_ && (presented_ || startup_paused_)) {
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
    startup_paused_ = false;
    tracks_ = {};
    preroll_bytes_ = 0;
    pause_origin_ = DirectOrigin::None;
    control_records_ = progress_records_ = audio_underruns_ = 0;
    reported_frames_ = 1;
    seek_due_ = seek_deadline_ = 0;
    preroll_at_ = clock_at_ = clock_ns_ = position_ns_ = last_feed_at_ = running_since_ = eos_at_ = next_progress_ns_ = 0;
#ifdef UXAIR_DIRECT_BUILD
    return media_.direct_released();
#else
    return true;
#endif
}
