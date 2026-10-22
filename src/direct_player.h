#pragma once
#include "direct_video.h"
#include "hls_relay.h"
#include <atomic>
class StarfishMediaAPIs;
class DirectPlayer {
    HlsRelay relay_;DirectSession *session_=nullptr;
    StarfishMediaAPIs *api_=nullptr;long acb_=0,task_=0;std::atomic<bool> loaded_{false};bool started_=false,acb_playing_=false,sink_attached_=false,reported_wait_=false;
    uintptr_t context_id_=0;std::mutex info_mutex_;std::string video_info_;bool info_pending_=false;
    std::atomic<unsigned> width_{0},height_{0};
    double start_position_=0;bool start_seconds_=false;int64_t opened_ns_=0,last_progress_ns_=0,last_position_ms_=-1;
    std::atomic<int> state_{int(DirectState::Idle)};
    std::atomic<int64_t> duration_ms_{0};
    std::atomic<int> stage_{int(DirectStage::Idle)},error_{int(DirectError::None)},last_callback_{-1};
    std::atomic<int64_t> native_result_{0},last_callback_value_{0};
    std::atomic<uint64_t> callbacks_{0};uint64_t event_types_=0;unsigned event_records_=0,phase_records_=0;
    void begin(DirectStage);
    void result(DirectStage,int64_t,int64_t);
    void fail(DirectError,int64_t=0);
    void cleanup();
    static void event(int,int64_t,const char*,void*);
public:
    ~DirectPlayer();
    bool open(const std::string& uri,double start_position,int64_t now,DirectSession *session=nullptr,bool seconds=false);
    bool control(const DirectCommand& command);
    DirectSnapshot tick(int64_t now);
    DirectSnapshot snapshot() const;
    void close();
};
