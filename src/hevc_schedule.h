#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>

// A decoder reopen resets decoder PTS, not the sender's wall-clock mapping.
// Keep the first-feed delay separate so repeated reopens do not add latency.
class StarfishHevcSchedule {
public:
    static const int64_t kPrerollDelayNs=200000000LL;
    void reset(){not_before_ns_=0;}
    int64_t reinitialize(int64_t previous_start,int64_t keyframe_pts,int64_t load_done) {
        not_before_ns_=load_done+kPrerollDelayNs;
        return previous_start+keyframe_pts;
    }
    int64_t feed_due(int64_t timeline_due) const {return std::max(timeline_due,not_before_ns_);}
private:
    int64_t not_before_ns_=0;
};

// One validated video AU may wait at the preroll bound. Audio remains serviceable
// while callbacks progress; this is not an extra receiver queue or a frame drop.
// Transport invalidation and session finish must also retire this pending AU.
template<class Packet> class PrerollVideoHold {
public:
    bool pending() const {return held_;}
    void clear(){held_=false;packet_=Packet();}
    void defer(Packet&& packet){packet_=std::move(packet);held_=true;}
    bool restore_if_ready(Packet& packet,bool ready,unsigned transport_epoch) {
        if(held_ && packet_.transport_epoch!=transport_epoch)clear();
        if(!held_ || !ready)return false;
        packet=std::move(packet_);held_=false;return true;
    }
private:
    bool held_=false;
    Packet packet_;
};
