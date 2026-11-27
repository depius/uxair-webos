#pragma once
#include <string>
#include "direct_native_diag.h"
#include <deque>
#include <mutex>
#include <cstdint>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <array>
enum class DirectCommandKind { Play, Pause, Resume, Seek, Stop };
enum class DirectOrigin { None, SenderPlay, InitialRate, SenderRate, SenderSeek, SenderStop, SenderDisconnect, ReceiverTimeout, Local, Buffering, Replacement, SenderRemove };
const char *direct_origin_name(DirectOrigin origin);
struct DirectCommand {
    DirectCommandKind kind;
    std::string uri;
    double position=0;
    bool position_seconds=false, encrypted=false, start_paused=false;
    uint64_t generation=0;
    int64_t admitted_ms=0;
    bool replacement=false;
    DirectOrigin origin=DirectOrigin::None;
};
enum class DirectState { Idle, Loading, Playing, Paused, Ended, Error, Buffering };
enum class DirectBackend { None, NativeUri, Bufferstream };
const char *direct_backend_name(DirectBackend);
enum class DirectStage { Idle, Queued, Dequeued, Relay, AcbCreate, AcbInitialize, Construct, MediaId, Load, AwaitLoad, Sink, LoadedState, Window, Seek, Play, AwaitPlaying, Playing, Paused, VideoInfo, Stopped, Fetch, BufferstreamLoad, PacketFeed, ManifestFetch, PlaylistParse, SegmentFetch, SegmentDemux, SegmentTimeline, Preroll, AwaitFirstFrame, InitSegment };
enum class DirectError { None, Busy, RelayStart, Payload, AcbCreate, AcbInitialize, Construct, MediaId, LoadRejected, NativeError, Sink, Seek, PlayRejected, LoadTimeout, DurationTimeout, PlayingTimeout, ProgressTimeout, ControlRejected, NoNativeProgress, CleanupFailed, FetchFailed, UnsupportedStream,
    InvalidStream, StreamLimit, Discontinuity, BufferstreamStart, BufferstreamFeed, NativeApiUnavailable,
    PlaylistSyntax, PlaylistVariant, PlaylistLive, PlaylistEncryption, PlaylistInitSegment, PlaylistByteRange,
    PlaylistTag, PlaylistNested, PlaylistDiscontinuity, SegmentContainer, DemuxOpen, DemuxRead,
    MissingH264, MissingAac, VideoCodec, AudioCodec, MultipleTracks, H264AnnexB, H264Keyframe,
    H264Configuration, H264Format, AacAdts, AacDecode, VideoTimestamp, AudioTimestamp, PacketCorrupt,
    TrackChange, DownloadWorker, FetchCancelled, AacNoPcm, StartupFetchTimeout, InitialFeedTimeout, FirstFrameTimeout,
    InitFetch, Mp4Boxes, Mp4Track, Mp4Config, SeparateAudio, EncryptedMedia, RangeResponse,
    HttpUnauthorized, HttpForbidden, FetchTimeout, HevcProfile, HevcConfiguration };
const char *direct_stage_name(DirectStage);
const char *direct_error_name(DirectError);
struct DirectSnapshot {
    DirectState state=DirectState::Idle; double position=0,duration=0;unsigned width=0,height=0;
    DirectStage stage=DirectStage::Idle;DirectError error=DirectError::None;int64_t native_result=0;
    DirectBackend backend=DirectBackend::None;
    DirectError fallback_reason=DirectError::None;
    bool seekable=false, backend_loaded=false, frame_observed=false, audio_fed=false, natural_end=false;
    bool ready_to_play=false, backend_running=false;
    bool audio_enabled=false;
    double loaded_start=0, loaded_end=0;
    uint64_t decoder_generation=0;
    int64_t frame_age_ms=-1, feed_age_ms=-1, active_ms=0;
    const char *codec="unknown", *container="unknown";
    int upstream_http=0;
    DirectOrigin stop_origin=DirectOrigin::None;
    uint64_t media_gets=0,buffer_bytes=0;
    uint64_t callbacks=0,relay_requests=0,upstream_fetches=0,downloaded_bytes=0,relay_failures=0;
    int last_callback=-1;int64_t last_callback_value=0;
    uint64_t generation=0;
};
const char *direct_state_name(DirectState state);
struct DirectReply {
    int status=400;
    std::string body,type="text/parameters";
    const char *descriptor="none";
    const char *reason=nullptr;
    bool queued=false;
};
bool direct_uri_valid(const std::string& uri);
bool direct_public_ipv4(uint32_t host_order);
std::string direct_load_payload(const std::string& uri,const std::string& app,bool internal=false,DirectLoadContract contract=DirectLoadContract::ArgsHls);
class DirectSession {
    std::thread reverse_thread_;std::atomic<bool> reverse_running_{false};int reverse_fd_=-1;uintptr_t reverse_token_=0;
    struct ReverseEvent { std::string body; uint64_t generation; };
    std::condition_variable event_cv_;std::deque<ReverseEvent> events_;
    int64_t last_request_ms_=0;
    unsigned request_id_=0,pending_id_=0;std::string pending_url_,pending_data_;bool pending_done_=false;
    void reverse_loop();void close_reverse();
    std::mutex mutex_;std::deque<DirectCommand> commands_;DirectSnapshot snapshot_;std::string owner_,controller_,media_uuid_;
    uintptr_t encrypted_reverse_=0, play_connection_=0;
    std::array<uintptr_t, 16> retired_play_connections_{};
    uint64_t generation_=0;
    int64_t handoff_until_ms_=0;
    bool stop_queued_=false;
    DirectOrigin pending_stop_origin_=DirectOrigin::None;
    unsigned lifecycle_records_=0, property_records_=0, control_connection_=0;
    bool reverse_failed_=false, terminal_reported_=true, initial_paused_=false;
    unsigned backend_attempts_=0, backend_loads_=0, backend_failures_=0, backend_cancellations_=0, video_starts_=0, audio_starts_=0;
    DirectBackend pending_backend_=DirectBackend::None;
    bool frame_reported_=false, audio_reported_=false;
    enum class BackendOutcome { Loaded, Failed, Cancelled };
    void backend_outcome_locked(BackendOutcome outcome);
    void event_locked(DirectState state);
    void stop_locked(DirectOrigin origin);
    void terminal_locked(const DirectSnapshot& status, DirectOrigin origin);
public:
    ~DirectSession();
    bool bind_reverse(const std::string&,int fd,uintptr_t token);
    bool bind_encrypted_reverse(const std::string& owner,const std::string& controller,uintptr_t token);
    int take_encrypted_event(uintptr_t token,std::string& body,uint64_t& generation);
    void cancel_fetch();
    bool check_timeout(int64_t now_ms);
    void connection_lost(uintptr_t token,bool reverse_failure=false);
    std::string sender_manifest(const std::string& uri);
    DirectReply request(const std::string& owner,const std::string& method,const std::string& target,
                        const std::string& type,const std::string& body,bool busy=false,
                        const std::string& controller={},uintptr_t token=0,unsigned connection=0);
    bool take(DirectCommand& command);
    bool is_current(uint64_t generation);
    bool publish(DirectSnapshot snapshot);
    void summarize();
    DirectSnapshot snapshot();
    void reset(bool keep_reverse=false);
};
