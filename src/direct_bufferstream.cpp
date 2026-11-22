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

DirectError hls_error(DirectHlsResult result) {
    if (result == DirectHlsResult::Unsupported) return DirectError::UnsupportedStream;
    if (result == DirectHlsResult::Limit) return DirectError::StreamLimit;
    return DirectError::InvalidStream;
}

bool fcup_enabled() {
    const char* value = std::getenv("UXAIR_DIRECT_FCUP");
    return value && std::string(value) == "1";
}
}

DirectBufferstream::~DirectBufferstream() { close(); }

bool DirectBufferstream::open(const DirectCommand& play, int64_t now, DirectSession* session) {
    close();
    status_ = DirectSnapshot();
    status_.backend = DirectBackend::Bufferstream;
    status_.state = DirectState::Loading;
    status_.stage = DirectStage::Fetch;
    opened_at_ = now;
    if (!network_.begin()) return fail(DirectError::RelayStart);
    session_ = session;
    if (session && fcup_enabled()) {
        network_.sender_manifest = [session](const std::string& url) { return session->sender_manifest(url); };
    }
    play_ = play;
    cancelled_ = false;
    finished_ = false;
    download_error_ = DirectError::None;
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
            const auto manifest = network_.fetch(play.uri, 256 * 1024, true);
            if (cancelled_) return;
            if (manifest.status != 200) { download_error_ = DirectError::FetchFailed; return; }
            const auto parsed = direct_parse_playlist(manifest.data, manifest.url, playlist);
            if (parsed != DirectHlsResult::Ok) { download_error_ = hls_error(parsed); return; }
            if (playlist.variant.empty()) break;
            if (level == 1) { download_error_ = DirectError::UnsupportedStream; return; }
            play.uri = playlist.variant;
        }
        duration_ns_ = playlist.duration_ns;
        const double requested = play.position_seconds ? play.position * second : play.position * playlist.duration_ns;
        const int64_t target = int64_t(std::max(0.0, std::min(requested, double(playlist.duration_ns - 1))));
        auto segment = std::upper_bound(playlist.segments.begin(), playlist.segments.end(), target,
            [](int64_t time, const DirectSegment& item) { return time < item.start_ns; });
        --segment;
        AudioDecoder audio;
        int64_t origin = 0, previous_dts = 0, previous_duration = 0;
        int width = 0, height = 0, aac_rate = 0, aac_channels = 0;
        for (; segment != playlist.segments.end(); ++segment) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // A timed wait uses the pthread ABI supported by the B7 toolchain.
                while (!cancelled_ && pending_) slot_.wait_for(lock, std::chrono::milliseconds(250));
                if (cancelled_) return;
            }

            const auto response = network_.fetch(segment->url, 8 * 1024 * 1024);
            if (cancelled_) return;
            if (response.status != 200) { download_error_ = DirectError::FetchFailed; return; }
            std::unique_ptr<Job> job(new Job);
            const auto result = direct_demux_ts(response.data, cancelled_, audio, job->segment);
            if (cancelled_) return;
            if (result != DirectHlsResult::Ok) { download_error_ = hls_error(result); return; }

            const auto& media = job->segment;
            if (!width) {
                origin = media.packets.front().dts_ns;
                width = media.width;
                height = media.height;
                aac_rate = media.aac_sample_rate;
                aac_channels = media.aac_channels;
            } else if (media.width != width || media.height != height ||
                       media.aac_sample_rate != aac_rate || media.aac_channels != aac_channels ||
                       std::llabs(media.first_video_dts_ns - previous_dts - previous_duration) > 250000000) {
                download_error_ = DirectError::Discontinuity;
                return;
            }
            previous_dts = media.first_video_dts_ns;
            previous_duration = segment->duration_ns;
            job->position_ns = segment->start_ns;
            job->decoder_origin_ns = origin;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_ = std::move(job);
            }
        }
        finished_ = true;
    } catch (...) {
        download_error_ = DirectError::StreamLimit;
    }
}

bool DirectBufferstream::take_next() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(pending_);
    packet_index_ = 0;
    slot_.notify_one();
    return bool(current_);
}

bool DirectBufferstream::fail(DirectError error) {
    status_.state = DirectState::Error;
    status_.error = error;
    std::fprintf(stderr, "[direct-bufferstream] stage=%s error=%s accepted-video=%llu accepted-audio=%llu\n",
        direct_stage_name(status_.stage), direct_error_name(error),
        static_cast<unsigned long long>(media_.feed_video_accepted.load()),
        static_cast<unsigned long long>(media_.feed_audio_accepted.load()));
    return false;
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
    status_.upstream_fetches = network_.requests;
    status_.downloaded_bytes = network_.bytes;
    status_.relay_failures = network_.failures;
    status_.duration = double(duration_ns_.load()) / second;
    status_.position = std::min(status_.duration, double(position_ns_ + clock_ns_) / second);
    std::lock_guard<std::mutex> lock(mutex_);
    status_.buffer_bytes = pending_ ? pending_->segment.bytes : 0;
    if (current_) for (size_t i = packet_index_; i < current_->segment.packets.size(); ++i)
        status_.buffer_bytes += current_->segment.packets[i].data.size();
}

DirectSnapshot DirectBufferstream::tick(int64_t now) {
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }
    const auto download_error = download_error_.load();
    if (download_error != DirectError::None) { fail(download_error); update_snapshot(); return status_; }

    // Take the last published batch even when the worker just reached EOF.
    const bool more_available = current_ || take_next();
    if (!opened_ && more_available) {
        status_.stage = DirectStage::BufferstreamLoad;
        status_.width = current_->segment.width;
        status_.height = current_->segment.height;
        position_ns_ = current_->position_ns;
#ifdef UXAIR_DIRECT_BUILD
        media_.guard_direct_callbacks();
#endif
        opened_ = media_.open(UXAIR_APP_ID, status_.width, status_.height, true, 44100,
                              2 * 1024 * 1024, 1920, 1080, MirrorCodec::H264, 60);
        if (!opened_) fail(DirectError::BufferstreamStart);
        last_feed_at_ = now;
    }
    if (opened_) media_.tick();
    if (opened_ && media_.failed) fail(DirectError::NativeError);
    if (!presented_ && now - opened_at_ > startup_timeout) fail(DirectError::PlayingTimeout);
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }

    if (buffering_ && more_available) {
        buffering_ = false;
        clock_at_ = now;
        if (!paused_) media_.pause(false);
    }
    if (media_.ready && media_.playing && more_available && (!paused_ || !presented_)) {
        status_.stage = DirectStage::PacketFeed;
        if (!clock_at_) clock_at_ = now;
        const int64_t playhead = clock_ns_ + (paused_ || buffering_ ? 0 : now - clock_at_);
        for (unsigned count = 0; count < 24 && packet_index_ < current_->segment.packets.size(); ++count) {
            const auto& packet = current_->segment.packets[packet_index_];
            const int64_t dts = packet.dts_ns - current_->decoder_origin_ns;
            if (dts > playhead + feed_lead) break;
            const int64_t pts = std::max<int64_t>(0, packet.pts_ns - current_->decoder_origin_ns);
            const auto feed = media_.feed(packet.data.data(), packet.data.size(), pts, packet.video);
            if (feed == FeedResult::Retry) {
                if (now - last_feed_at_ > 5 * second) fail(DirectError::ProgressTimeout);
                break;
            }
            if (feed != FeedResult::Accepted) { fail(DirectError::BufferstreamFeed); break; }
            accepted_until_ns_ = std::max(accepted_until_ns_, pts + packet.duration_ns);
            last_feed_at_ = now;
            ++packet_index_;
        }
        if (packet_index_ == current_->segment.packets.size()) current_.reset();
    }
    if (status_.state == DirectState::Error) { update_snapshot(); return status_; }

    if (opened_ && media_.ready && media_.playing && !presented_ &&
        media_.frames && media_.feed_video_accepted && media_.feed_audio_accepted) {
        presented_ = true;
        started_at_ = now;
        if (paused_) media_.pause(true);
        std::fprintf(stderr, "[direct-bufferstream] decoder-progress geometry=%ux%u video=H264 audio=AAC-LC\n",
                     status_.width, status_.height);
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

void DirectBufferstream::close() {
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
    packet_index_ = 0;
    clock_at_ = clock_ns_ = position_ns_ = accepted_until_ns_ = last_feed_at_ = started_at_ = eos_at_ = 0;
}
