#include "direct_bufferstream.h"
#include "audio.h"
#include "h264.h"

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
bool inject_retry = true;
int64_t last_video_pts = -1, last_audio_pts = -1;
}

Media::Media() : impl_(nullptr) {}
Media::~Media() { close(); }

bool Media::open(const char*, int width, int height, bool audio, int rate, int, int, int, MirrorCodec codec, int, bool) {
    assert(!decoder_open && width == 1280 && height == 720 && audio && rate == 44100 && codec == MirrorCodec::H264);
    decoder_open = true;
    ++opens;
    last_video_pts = last_audio_pts = -1;
    feed_audio_accepted = feed_video_accepted = frames = 0;
    failed = ended = false;
    ready = playing = true;
    return true;
}

FeedResult Media::feed(const void* bytes, size_t size, int64_t pts, bool video) {
    assert(decoder_open && size > 0 && pts >= 0);
    if (inject_retry) {
        inject_retry = false;
        ++retries;
        return FeedResult::Retry;
    }
    if (video) {
        const auto info = inspect_h264(static_cast<const uint8_t*>(bytes), size);
        assert(info.valid && info.picture);
        if (!feed_video_accepted) assert(info.keyframe && info.width == 1280 && !info.configuration.empty());
        assert(pts > last_video_pts); // fixture uses no B frames
        last_video_pts = pts;
        ++feed_video_accepted;
        ++frames;
        last_frame_event_ns = now;
    } else {
        assert(size % 4 == 0 && pts > last_audio_pts);
        last_audio_pts = pts;
        ++feed_audio_accepted;
    }
    return FeedResult::Accepted;
}

void Media::tick() {}
void Media::eos() { ++ends; ended = true; }
void Media::pause(bool paused) { if (paused) ++pauses; }
void Media::close() { if (decoder_open) { ++closes; decoder_open = false; } ready = playing = false; }

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string base = argv[1];
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
    assert(poll_for(DirectState::Error).error == DirectError::InvalidStream);
    player.close();

    play.uri = base + "/slow.m3u8";
    assert(player.open(play, now, nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto before = std::chrono::steady_clock::now();
    player.close();
    assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(4));

    play.uri = base + "/master.m3u8?token=PRIVATE";
    assert(player.open(play, now, nullptr));
    poll_for(DirectState::Playing, .1);
    player.close();
    assert(opens == closes);
    puts("PASS: real H.264/AAC MPEG-TS demux and PCM feed, bounded buffering, retry, seek, EOS, cancellation and recovery");
}
