#pragma once
#include "direct_video.h"
#include <atomic>
class StarfishMediaAPIs;
class DirectPlayer {
    StarfishMediaAPIs *api_=nullptr;long acb_=0,task_=0;std::atomic<bool> loaded_{false};bool started_=false,acb_playing_=false;
    double start_fraction_=0;int64_t opened_ns_=0;
    std::atomic<int> state_{int(DirectState::Idle)};
    std::atomic<int64_t> duration_ms_{0};
    static void event(int,int64_t,const char*,void*);
public:
    ~DirectPlayer();
    bool open(const std::string& uri,double start_fraction,int64_t now);
    bool control(const DirectCommand& command);
    DirectSnapshot tick(int64_t now);
    void close();
};
