#include "direct_playback.h"
#include "audio.h"
#include "h264.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
int64_t now = 1000000000;
unsigned opens = 0, closes = 0, retries = 0, pauses = 0, ends = 0;
bool decoder_open = false;
bool native_open = false, native_releases = true;
unsigned native_attempts = 0;
bool inject_retry = true;
bool reordered_pts = false, allow_reordering = false;
bool pcm_signal = false;
int expected_width = 1280, expected_height = 720;
int64_t last_video_pts = -1, last_audio_pts = -1;
enum class Startup { AfterPreroll, CallbackFirst, NoLoadCallback, NoPlaying, NoFrame, RejectVideo, NoFeed, PauseReplay };
Startup mode = Startup::AfterPreroll;
int64_t mock_opened_at = 0;
bool saw_early_video = false, saw_early_audio = false;
bool replay_resumed = false;
bool stall_frames = false, starve_audio = false;
int64_t replay_callback_at = 0;
}

// Mock the unavailable television URI player. The fallback uses real HLS,
// MPEG-TS demux and AAC decode, with the captured Feed calls below.
DirectPlayer::~DirectPlayer() { close(); }
bool DirectPlayer::open(const std::string&,double,int64_t,DirectSession*,bool,DirectLoadContract,bool,bool) {
    assert(!native_open && !decoder_open);
    ++native_attempts;
    native_open=true;
    return false;
}
DirectSnapshot DirectPlayer::snapshot() const {
    DirectSnapshot value;
    value.state=DirectState::Error;
    value.backend=DirectBackend::NativeUri;
    value.error=DirectError::LoadRejected;
    return value;
}
DirectSnapshot DirectPlayer::tick(int64_t) { return snapshot(); }
bool DirectPlayer::control(const DirectCommand&) { return false; }
bool DirectPlayer::close() { native_open=false; return native_releases; }

Media::Media() : impl_(nullptr) {}
Media::~Media() { close(); }

bool Media::open(const char*, int width, int height, bool audio, int rate, int, int, int, MirrorCodec codec, int, bool) {
    assert(!native_open && !decoder_open && width == expected_width && height == expected_height &&
           audio && rate == 44100 && codec == MirrorCodec::H264);
    decoder_open = true;
    ++opens;
    last_video_pts = last_audio_pts = -1;
    reordered_pts = false;
    pcm_signal = false;
    feed_audio_accepted = feed_video_accepted = frames = 0;
    failed = ended = ready = playing = false;
    play_requested_ = false;
    play_attempts_ = 0;
    mock_opened_at = now;
    replay_resumed = false;
    replay_callback_at = 0;
    saw_early_video = saw_early_audio = false;
    return true;
}

FeedResult Media::feed(const void* bytes, size_t size, int64_t pts, bool video) {
    assert(decoder_open && size > 0 && pts >= 0);
    assert(now >= mock_opened_at + 200000000LL && "preroll must follow the successful Load");
    if (mode == Startup::CallbackFirst) assert(ready && play_requested_);
    if (mode == Startup::RejectVideo && video) return FeedResult::Error;
    if (mode == Startup::NoFeed) return FeedResult::Retry;
    if (!video && starve_audio) return FeedResult::Retry;
    if (mode == Startup::PauseReplay && !replay_resumed &&
        (video ? feed_video_accepted.load() >= 12 : feed_audio_accepted.load() >= 11))
        return FeedResult::Retry;
    if (inject_retry) {
        inject_retry = false;
        ++retries;
        return FeedResult::Retry;
    }
    if (video) {
        saw_early_video |= !ready;
        const auto info = inspect_h264(static_cast<const uint8_t*>(bytes), size);
        assert(info.valid && info.picture);
        if (!feed_video_accepted)
            assert(info.keyframe && info.width == expected_width && info.pps && !info.configuration.empty());
        if (pts <= last_video_pts) {
            assert(allow_reordering);
            reordered_pts = true;
        }
        last_video_pts = pts;
        ++feed_video_accepted;
        if (playing && mode != Startup::NoFrame && !stall_frames && now >= replay_callback_at) {
            ++frames;
            last_frame_event_ns = now;
        }
    } else {
        saw_early_audio |= !ready;
        assert(size % 4 == 0 && pts > last_audio_pts);
        if (!pcm_signal) {
            const auto* pcm = static_cast<const uint8_t*>(bytes);
            pcm_signal = std::any_of(pcm, pcm + size, [](uint8_t byte) { return byte != 0; });
        }
        last_audio_pts = pts;
        ++feed_audio_accepted;
    }
    return FeedResult::Accepted;
}

void Media::tick() {
    if (!decoder_open) return;
    const auto age = now - mock_opened_at;
    const bool preroll = feed_video_accepted && feed_audio_accepted;
    if (mode == Startup::PauseReplay && !replay_resumed &&
        (feed_video_accepted < 12 || feed_audio_accepted < 11)) return;
    if (!ready && mode != Startup::NoLoadCallback &&
        ((mode == Startup::CallbackFirst && age >= 100000000) || (preroll && age >= 400000000))) ready = true;
    if (ready && !play_requested_) {
        ++play_attempts_;
        play_requested_ = true;
    }
    if (play_requested_ && preroll && mode != Startup::NoPlaying) {
        playing = true;
        if (!frames && mode != Startup::NoFrame) {
            ++frames;
            last_frame_event_ns = now;
        }
    }
}
void Media::eos() { ++ends; ended = true; }
void Media::pause(bool paused) {
    if (paused) {
        ++pauses;
        if (mode == Startup::PauseReplay && !replay_resumed) assert(feed_video_accepted == 12 && feed_audio_accepted == 11);
    } else if (mode == Startup::PauseReplay) {
        replay_resumed = true;
        replay_callback_at = now + 150000000;
    }
}
void Media::close() { if (decoder_open) { ++closes; decoder_open = false; } ready = playing = play_requested_ = false; }

static void public_samples(const std::string& base) {
    allow_reordering = true;
    inject_retry = false;
    HlsRelay fetch;
    assert(fetch.begin());
    const auto full_mux = fetch.fetch(base + "/mux/url_0/full-media.m3u8", 256 * 1024, true);
    DirectPlaylist playlist;
    assert(full_mux.status == 200 && direct_parse_playlist(full_mux.data, full_mux.url, playlist) == DirectError::None);
    assert(playlist.segments.size() > 2);
    fetch.stop();
    DirectBufferstream player;
    const auto run = [&](const char* path, int width, int height, DirectError error, double duration = 19.8) {
        expected_width = width;
        expected_height = height;
        DirectCommand play;
        play.kind = DirectCommandKind::Play;
        play.uri = base + path + "?token=PRIVATE";
        assert(player.open(play, now, nullptr));
        bool played = false;
        const auto previous_opens = opens;
        for (unsigned tick = 0; tick < 4000; ++tick) {
            const auto snapshot = player.tick(now += 20000000);
            assert(snapshot.buffer_bytes <= 16 * 1024 * 1024);
            if (snapshot.state == DirectState::Playing) {
                assert(snapshot.backend_loaded && snapshot.frame_observed && snapshot.audio_fed);
                played = true;
            }
            if (snapshot.state == DirectState::Error) {
                assert(snapshot.error == error);
                assert(player.close());
                return;
            }
            if (snapshot.state == DirectState::Ended) {
                assert(error == DirectError::None && played && snapshot.position >= duration && reordered_pts && pcm_signal);
                assert(opens == previous_opens + 1);
                assert(player.close());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        assert(false && "public HLS pipeline did not reach a terminal outcome");
    };
    run("/apple/master.m3u8", 960, 720, DirectError::None);
    // Reproduce the B7's pause at the first frame with both TS segments in
    // flight and the exact accepted packet counts from the hardware trace.
    mode = Startup::PauseReplay;
    expected_width = 640;
    expected_height = 480;
    DirectCommand play;
    play.kind = DirectCommandKind::Play;
    play.uri = base + "/apple/gear2/prog_index.m3u8";
    assert(player.open(play, now, nullptr));
    DirectCommand rate;
    rate.kind = DirectCommandKind::Pause;
    rate.origin = DirectOrigin::SenderRate;
    assert(player.control(rate, now));
    DirectSnapshot frozen;
    for (unsigned i = 0; i < 400; ++i) {
        frozen = player.tick(now += 20000000);
        assert(frozen.state != DirectState::Error);
        if (frozen.state == DirectState::Paused) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    assert(frozen.state == DirectState::Paused && frozen.frame_observed && frozen.audio_fed && frozen.buffer_bytes);
    const auto after_pause = player.tick(now += 12000000000LL);
    assert(after_pause.state == DirectState::Paused && after_pause.position == frozen.position);
    rate.kind = DirectCommandKind::Resume;
    assert(player.control(rate, now));
    auto resumed = player.tick(now += 8000000);
    assert(resumed.state != DirectState::Error && "first tick after long pause must not reuse the old frame deadline");
    const auto playback_opens = opens;
    for (unsigned i = 0; i < 1400 && resumed.state != DirectState::Ended; ++i) {
        resumed = player.tick(now += 20000000);
        assert(resumed.state != DirectState::Error && opens == playback_opens);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    assert(resumed.state == DirectState::Ended && resumed.position >= 19.8 && reordered_pts && pcm_signal);
    assert(player.close());
    puts("PASS: 640x480 resumes after a long pause at 12 video and 11 audio feeds, with no decoder reopen");
    mode = Startup::AfterPreroll;
    run("/apple/gear2/prog_index.m3u8", 640, 480, DirectError::None);
    run("/mux/master.m3u8", 1280, 720, DirectError::None, 60.0);
    run("/apple/bad-timestamp.m3u8", 640, 480, DirectError::VideoTimestamp);
    run("/apple/discontinuous.m3u8", 640, 480, DirectError::PlaylistDiscontinuity);
    run("/apple/timeline-reset.m3u8", 640, 480, DirectError::Discontinuity);
    run("/apple/init-map.m3u8", 0, 0, DirectError::Mp4Boxes);
    run("/apple/container.m3u8", 0, 0, DirectError::SegmentContainer);
    assert(opens == closes);
    puts("PASS: Apple BipBop (22.05 kHz AAC) and 60+ seconds of 720p Mux MPEG-TS feed without a decoder restart");
}

static void fmp4_samples(const std::string& base) {
    expected_width = 640;
    expected_height = 480;
    allow_reordering = true;
    inject_retry = false;
    DirectBufferstream player;
    const auto run = [&](const char* path, DirectError error, unsigned expected_opens, bool seek = false) {
        const unsigned before = opens;
        bool played = false, sought = false;
        DirectCommand command;
        command.kind = DirectCommandKind::Play;
        command.uri = base + path;
        assert(player.open(command, now, nullptr));
        for (unsigned i = 0; i < 1800; ++i) {
            const auto snapshot = player.tick(now += 20000000);
            assert(snapshot.buffer_bytes <= 16 * 1024 * 1024);
            if (snapshot.state == DirectState::Playing && snapshot.position > (sought ? 4.3 : .5)) {
                assert(snapshot.backend_loaded && snapshot.frame_observed && snapshot.audio_fed);
                played = true;
                if (seek && !sought) {
                    command.kind = DirectCommandKind::Pause;
                    assert(player.control(command, now));
                    const auto frozen = player.tick(now += 12000000000LL);
                    assert(frozen.state == DirectState::Paused && frozen.position == snapshot.position);
                    command.kind = DirectCommandKind::Resume;
                    assert(player.control(command, now));
                    assert(player.tick(now += 8000000).state != DirectState::Error);
                    command.kind = DirectCommandKind::Seek;
                    command.position = 4.25;
                    assert(player.control(command, now));
                    sought = true;
                    played = false;
                }
            }
            if (snapshot.state == DirectState::Error || snapshot.state == DirectState::Ended) {
                if (error == DirectError::None) {
                    assert(snapshot.state == DirectState::Ended && played && snapshot.position >= 7.9);
                    assert(reordered_pts && pcm_signal && std::llabs(last_video_pts - last_audio_pts) < 160000000);
                } else {
                    assert(snapshot.state == DirectState::Error && snapshot.error == error);
                }
                assert(opens == before + expected_opens);
                assert(player.close() && opens == closes);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        assert(false && "fMP4 pipeline did not reach a terminal outcome");
    };
    run("/master.m3u8", DirectError::None, 1);
    run("/ranges.m3u8", DirectError::None, 1);
    run("/clip/media.m3u8", DirectError::None, 2, true);
    run("/boundary.m3u8", DirectError::None, 2);
    run("/changed-map.m3u8", DirectError::None, 2);
    run("/missing-map.m3u8", DirectError::PlaylistInitSegment, 0);
    run("/missing-init.m3u8", DirectError::InitFetch, 0);
    run("/malformed.m3u8", DirectError::Mp4Boxes, 0);
    run("/encrypted.m3u8", DirectError::EncryptedMedia, 0);
    run("/encrypted-playlist.m3u8", DirectError::PlaylistEncryption, 0);
    run("/unsupported.m3u8", DirectError::VideoCodec, 0);
    run("/audio-only.m3u8", DirectError::Mp4Track, 0);
    run("/separate.m3u8", DirectError::SeparateAudio, 0);
    run("/invalid-range.m3u8", DirectError::PlaylistByteRange, 0);
    run("/invalid-map-range.m3u8", DirectError::PlaylistByteRange, 0);
    run("/bad-range-response.m3u8", DirectError::RangeResponse, 0);
    run("/ignored-range.m3u8", DirectError::RangeResponse, 0);
    run("/oversized.m3u8", DirectError::StreamLimit, 0);
    puts("PASS: fMP4 H.264/AAC-LC, B-frames, byte ranges, pause, seek, discontinuity, map change and rejection paths");
}

int main(int argc, char** argv) {
    assert(argc == 2 || (argc == 3 && (std::string(argv[2]) == "--public" || std::string(argv[2]) == "--fmp4")));
    direct_hls_log_setup();
    const std::string base = argv[1];
    if (argc == 3) {
        if (std::string(argv[2]) == "--fmp4") fmp4_samples(base);
        else public_samples(base);
        return 0;
    }
    DirectPlaylist selected;
    const std::string master = "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=1280x720,CODECS=\"avc1.42c01f,mp4a.40.2\"\n"
        "variants/main.m3u8?token=PRIVATE\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=8000000,RESOLUTION=3840x2160,CODECS=\"hvc1.1.6,mp4a.40.2\"\n"
        "ultra.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720,CODECS=\"avc1.42c01f,mp4a.40.29\"\n"
        "he-aac.m3u8\n";
    assert(direct_parse_playlist(master, base + "/master.m3u8", selected) == DirectError::None);
    assert(selected.variant == base + "/variants/main.m3u8?token=PRIVATE");
    assert(direct_parse_playlist("#EXTM3U\n#EXT-X-DISCONTINUITY\n", base + "/media.m3u8", selected)
           == DirectError::PlaylistDiscontinuity);
    assert(direct_parse_playlist("#EXTM3U\n#EXT-X-DATERANGE:ID=\"PRIVATE\"\n#EXT-X-DISCONTINUITY-SEQUENCE:0\n"
        "#EXTINF:2,\nmedia.ts\n#EXT-X-ENDLIST\n", base + "/media.m3u8", selected) == DirectError::None);
    assert(selected.diagnostic_tag == "#EXT-X-DATERANGE");
    assert(direct_parse_playlist("#EXTM3U\n#EXT-X-PART:URI=\"PRIVATE\"\n", base + "/media.m3u8", selected)
           == DirectError::PlaylistTag && selected.diagnostic_tag=="#EXT-X-PART");
    DirectBufferstream player;
    DirectCommand play;
    play.kind = DirectCommandKind::Play;
    play.uri = base + "/redirect";
    auto poll_for = [&](DirectState expected, double minimum = 0) {
        for (unsigned tick = 0; tick < 1200; ++tick) {
            now += 20000000;
            const auto status = player.tick(now);
            assert(status.buffer_bytes <= 16 * 1024 * 1024);
            if (status.state == expected && status.position >= minimum) return status;
            if (status.state == DirectState::Error) {
                std::fprintf(stderr, "Unexpected bufferstream error: %s\n", direct_error_name(status.error));
                assert(false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        assert(false && "playback timed out");
        return DirectSnapshot();
    };

    assert(player.open(play, now, nullptr));
    const auto playing = poll_for(DirectState::Playing, .3);
    assert(playing.duration == 12 && playing.width == 1280 && playing.downloaded_bytes > 0 && retries == 1);
    assert(saw_early_video && saw_early_audio);
    DirectCommand control;
    control.kind = DirectCommandKind::Pause;
    assert(player.control(control, now));
    const auto paused = player.tick(now);
    const auto later = player.tick(now + 1000000000);
    assert(paused.state == DirectState::Paused && paused.position == later.position && pauses == 1);
    now += 1000000000;
    control.kind = DirectCommandKind::Resume;
    assert(player.control(control, now));
    poll_for(DirectState::Playing, paused.position + .1);
    control.kind = DirectCommandKind::Seek;
    control.position = 5;
    assert(player.control(control, now));
    const auto seeking = player.tick(now);
    assert(seeking.state == DirectState::Loading || opens == 2);
    const auto sought = poll_for(DirectState::Playing, 4);
    assert(sought.position < 5 && opens == 2 && closes == 1);
    const auto ended = poll_for(DirectState::Ended);
    assert(ended.position == 12 && ends == 1 && std::llabs(last_video_pts - last_audio_pts) < 100000000);
    player.close();
    assert(opens == closes);

    play.uri = base + "/missing-segment.m3u8";
    assert(player.open(play, now, nullptr));
    assert(poll_for(DirectState::Error).error == DirectError::FetchFailed);
    player.close();

    play.uri = base + "/bad.m3u8";
    assert(player.open(play, now, nullptr));
    assert(poll_for(DirectState::Error).error == DirectError::PlaylistSyntax);
    player.close();

    play.uri = base + "/slow.m3u8";
    DirectSession cancelled;
    const std::string controller(32, 'c');
    assert(cancelled.request("owner", "POST", "/play", "text/parameters",
        "Content-Location: https://media.example/slow.m3u8\r\n", false, controller, 1).status == 200);
    DirectCommand queued;
    assert(cancelled.take(queued) && queued.kind == DirectCommandKind::Play);
    assert(player.open(play, now, &cancelled));
    cancelled.publish(player.tick(now));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cancelled.connection_lost(1);
    assert(cancelled.take(queued) && queued.kind == DirectCommandKind::Stop);
    const auto before = std::chrono::steady_clock::now();
    auto stopped = player.tick(now);
    player.close();
    assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(4));
    stopped.state = DirectState::Ended;
    cancelled.publish(stopped);
    cancelled.summarize();

    play.uri = base + "/master.m3u8?token=PRIVATE";
    assert(player.open(play, now, nullptr));
    poll_for(DirectState::Playing, .1);
    player.close();
    assert(opens == closes);
    DirectSession session;
    DirectPlayback automatic;
    assert(!unsetenv("UXAIR_DIRECT_BACKEND"));
    assert(automatic.open(play,now,session));
    for (unsigned i=0; i<450; ++i) {
        const auto selected = automatic.tick(now+=20000000);
        assert(selected.backend==DirectBackend::Bufferstream && selected.fallback_reason==DirectError::None);
        assert(!native_attempts && selected.state!=DirectState::Error);
        if (selected.state==DirectState::Playing) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    assert(automatic.snapshot().state==DirectState::Playing);
    assert(automatic.close() && opens==closes);
    puts("PASS: launcher default starts BUFFERSTREAM without calling experimental native URI Load");

    assert(!setenv("UXAIR_DIRECT_BACKEND","auto",1));
    assert(automatic.open(play,now,session));
    DirectSnapshot routed;
    for(unsigned i=0;i<450;++i) {
        routed=automatic.tick(now+=20000000);
        assert(routed.backend==DirectBackend::Bufferstream && routed.fallback_reason==DirectError::LoadRejected);
        assert(routed.state!=DirectState::Error);
        if(routed.state==DirectState::Playing)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    assert(routed.state==DirectState::Playing && native_attempts==1 && !native_open);
    assert(automatic.close() && opens==closes);

    native_releases=false;
    assert(!automatic.open(play,now,session));
    assert(automatic.tick(now).error==DirectError::CleanupFailed && opens==closes);
    puts("PASS: auto fallback after native teardown, no decoder overlap or retry after cleanup failure");

    for (const auto& fault : {std::make_pair(Startup::NoLoadCallback, DirectError::LoadTimeout),
             std::make_pair(Startup::NoPlaying, DirectError::PlayingTimeout),
             std::make_pair(Startup::NoFrame, DirectError::FirstFrameTimeout),
             std::make_pair(Startup::RejectVideo, DirectError::BufferstreamFeed),
             std::make_pair(Startup::NoFeed, DirectError::InitialFeedTimeout)}) {
        mode = fault.first;
        assert(player.open(play, now, nullptr));
        for (unsigned i=0; i<200 && !decoder_open; ++i) {
            player.tick(now += 20000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        assert(decoder_open);
        for (unsigned i=0; i<55; ++i) player.tick(now += 20000000);
        const auto failed = player.tick(now += 21000000000LL);
        assert(failed.state == DirectState::Error && failed.error == fault.second && !failed.frame_observed);
        if (mode == Startup::NoLoadCallback) assert(saw_early_video && saw_early_audio);
        assert(player.close() && opens == closes);
    }
    mode = Startup::CallbackFirst;
    assert(player.open(play, now, nullptr));
    poll_for(DirectState::Playing);
    assert(!saw_early_video && !saw_early_audio);
    assert(player.close() && opens == closes);
    mode = Startup::AfterPreroll;
    assert(player.open(play, now, nullptr));
    for (unsigned i=0; i<200 && !decoder_open; ++i) {
        player.tick(now += 20000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    assert(decoder_open);
    const auto before_feed = player.tick(now += 20000000);
    assert(!before_feed.backend_loaded && !before_feed.audio_fed && !before_feed.frame_observed);
    assert(player.close());
    assert(player.open(play, now, nullptr));
    poll_for(DirectState::Playing);
    assert(player.close() && opens == closes);

    for (bool audio_stall : {false, true}) {
        assert(player.open(play, now, nullptr));
        poll_for(DirectState::Playing, .4);
        const auto active_decoder = opens;
        starve_audio = audio_stall;
        stall_frames = !audio_stall;
        const auto timeout = poll_for(DirectState::Error);
        assert(timeout.error == DirectError::ProgressTimeout && opens == active_decoder);
        assert(player.close() && opens == closes);
        stall_frames = starve_audio = false;
    }

    play.uri = base + "/buffering.m3u8";
    assert(player.open(play, now, nullptr));
    const auto buffering = poll_for(DirectState::Buffering);
    const auto pausing_decoder = opens;
    const auto backend_pauses = pauses;
    control.kind = DirectCommandKind::Pause;
    control.origin = DirectOrigin::SenderRate;
    assert(player.control(control, now));
    assert(player.tick(now += 12000000000LL).state == DirectState::Paused);
    assert(pauses == backend_pauses);
    HlsRelay release;
    assert(release.begin() && release.fetch(base + "/release-segment", 256).status == 200);
    release.stop();
    const auto still_paused = poll_for(DirectState::Paused, buffering.position);
    assert(still_paused.position == buffering.position && opens == pausing_decoder);
    control.kind = DirectCommandKind::Resume;
    assert(player.control(control, now));
    poll_for(DirectState::Playing, buffering.position + .2);
    assert(opens == pausing_decoder && player.close() && opens == closes);

    puts("PASS: bounded preroll resolves delayed load and Playing, reports startup faults, and supports stop and reopen");
    puts("PASS: pause while buffering, delayed resume frames, decoder stall and independent audio backpressure");
    puts("PASS: real H.264/AAC MPEG-TS demux and PCM feed, bounded buffering, retry, seek, EOS, cancellation and recovery");
}
