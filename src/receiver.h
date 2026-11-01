#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "raop.h"
#include "diagnostics.h"
#include "settings.h"
#include "audio_session.h"
#include "session_lifecycle.h"
#include "pairing_ui.h"
#include "now_playing.h"
struct Packet {
    bool video;
    unsigned codec;
    unsigned audio_epoch=0, transport_epoch=0;
    uint64_t timestamp;
    int64_t arrival_ns = 0, enqueued_ns = 0, dequeued_ns = 0;
    std::vector<uint8_t> data;
};
struct ReceiverDiagnostics {
    size_t queue_packets = 0, queue_bytes = 0;
    uint64_t accepted_video_packets=0; // Since last receiver.clear(), including pre-Load ingress.
    size_t audio_queue_packets=0,audio_queue_bytes=0;
    uint64_t audio_pts_min=0,audio_pts_max=0;
    size_t peak_queue_packets = 0, peak_queue_bytes = 0;
    size_t video_queue_packets = 0, video_queue_bytes = 0;
    size_t peak_video_queue_packets = 0, peak_video_queue_bytes = 0;
    uint64_t dropped_packets = 0, dropped_video_frames = 0;
    DurationSample ingress_to_queue, ingress_copy, queue_lock_wait, queue_pop;
    DurationSample video_queue_residence, audio_queue_residence;
};
struct Receiver {
    ~Receiver();
    bool start(const std::string& directory, const Settings& settings);
    void stop();
    void disconnect();
    void begin_close();
    void request_reset(ResetOrigin origin,int reason);
    bool record_reset_locked(ResetOrigin origin,int reason,unsigned epoch); // mutex held
    ResetNotice take_reset();
    bool pop(Packet& packet, uint64_t latest = UINT64_MAX);
    void enqueue(bool video, unsigned codec, uint64_t time, const uint8_t *data, int size);
    void clear();
    void clear_audio_packets();
    ReceiverDiagnostics diagnostics();
    ReceiverDiagnostics queue_snapshot();
    VideoTimingSample take_video_timing();
    std::string status();
    void show(const std::string& text);
    std::string pin();
    NowPlaying now_playing();
    bool idle_for_profile_change();
    void adjust_audio_delay(int delta_ms);
    std::atomic<int> audio_delay_ms{100};
    std::atomic<unsigned> diagnostic_generation{0};
    std::atomic<bool> diagnostic_session_open{false};
    uint64_t accepted_video_packets=0; // protected by mutex, reset by clear()
    ResetNotice resets; // protected by mutex
    IngressState ingress; // protected by mutex
    std::atomic<bool> audio_flush{false}, paused{false}, has_audio{false}, mirror{false};
    std::atomic<bool> audio_using_screen{false}, audio_is_media{false};
    std::atomic<unsigned> audio_samples_per_frame{0}, audio_format{0};
    std::atomic<int> width{1280},height{720};
    std::atomic<float> volume{1};
    std::atomic<int64_t> activity{0};
    std::atomic<int64_t> last_audio_arrival{0};
    std::atomic<int64_t> last_video_arrival{0};
    std::atomic<uint64_t> total_video_arrivals{0};
    std::atomic<unsigned> last_audio_codec{0};
    std::atomic<unsigned> audio_epoch{0}, transport_epoch{0};
    std::atomic<unsigned> incoming_video_codec{0}; // 1 H.264, 2 HEVC
    std::atomic<unsigned> overflow{0};
    std::atomic<unsigned> session_resets{0};
    std::atomic<unsigned> stream_requests{0};   // admitted SETUPs: a device starts streaming
    size_t max_video_packet_bytes=1024*1024, max_queue_bytes=4*1024*1024;
    Settings active_profile;
    raop_t *raop = nullptr;
    dnssd_t *dnssd = nullptr;
    std::mutex mutex;
    // Serializes paired.keys snapshots without blocking UI reads of pairing state.
    std::mutex pairing_store_mutex;
    std::deque<Packet> packets;
    size_t bytes=0;
    size_t peak_packets=0, peak_bytes=0;
    size_t video_packets=0, video_bytes=0;
    size_t peak_video_packets=0, peak_video_bytes=0;
    uint64_t dropped_packets=0, dropped_video_frames=0;
    VideoTiming video_timing;
    DurationWindow ingress_to_queue, ingress_copy, queue_lock_wait, queue_pop;
    DurationWindow video_queue_residence, audio_queue_residence;
    std::string directory, message="Waiting for a connection", client, client_name;
    PairingOverlayState pairing;
    std::mutex meta_mutex;   // the sender's callbacks run on its threads
    NowPlaying meta;
    std::set<std::string> keys;
};
