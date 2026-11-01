#pragma once
#include <cstdint>

enum class ResetOrigin { SenderTeardown=0, VideoCallback, TransportCallback, Capacity, Oversize };
inline const char *reset_origin_name(ResetOrigin origin) {
    switch(origin) {
    case ResetOrigin::SenderTeardown:return "sender-teardown";
    case ResetOrigin::VideoCallback:return "video-callback";
    case ResetOrigin::TransportCallback:return "transport-callback";
    case ResetOrigin::Capacity:return "queue-capacity";
    case ResetOrigin::Oversize:return "oversized-packet";
    }
    return "unknown";
}
struct ResetNotice {
    unsigned sources=0,epoch=0;
    int video_reason=-1,transport_reason=-1;
    bool pending() const {return sources!=0;}
    bool failure() const {return (sources&~1u)!=0;}
    // Called under the receiver mutex; simultaneous failures take priority.
    bool add(ResetOrigin origin,int reason,unsigned generation=0) {
        if(sources && epoch!=generation)*this=ResetNotice();
        epoch=generation;
        const bool first=!pending();sources|=1u<<unsigned(origin);
        if(origin==ResetOrigin::VideoCallback || origin==ResetOrigin::SenderTeardown)video_reason=reason;
        if(origin==ResetOrigin::TransportCallback)transport_reason=reason;
        return first;
    }
};
// Receiver mutex protects this gate and the queue. Epoch comparisons additionally
// reject a copy begun before teardown, even after a subsequent setup is admitted.
struct IngressState {
    bool accepting=true,closing=false;
    void close(){accepting=false;closing=true;}
    void disconnected(){accepting=false;closing=false;}
    bool admit(){if(closing)return false;accepting=true;return true;}
    bool accepts(unsigned copy,unsigned current) const {return accepting&&!closing&&copy==current;}
};
struct NowPlayingCadence {
    int64_t completed=0;
    bool due(int64_t now,bool dirty,bool animated) const {
        return !completed || now-completed>(dirty||animated?33000000LL:1000000000LL);
    }
    void finish(int64_t end){completed=end;}
};
