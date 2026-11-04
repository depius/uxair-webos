#include "receiver.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <limits>

// No network/backend is linked. Exercise the actual production ingress methods.
Receiver::~Receiver() {}
static unsigned clock_calls=0;
static std::function<void()> after_copy;
int64_t now_ns() {
    ++clock_calls;
    if(clock_calls==2 && after_copy)after_copy();
    return int64_t(clock_calls)*1000;
}
static void configure(Receiver& r) {
    Settings profile;profile.width=3840;profile.height=2160;profile.codec=MirrorCodec::ExperimentalHevc;
    const MirrorLimits limits=mirror_limits(profile);
    r.max_video_packet_bytes=limits.video_packet_bytes;r.max_queue_bytes=limits.queue_bytes;
    r.incoming_video_codec=2;r.diagnostic_generation=7;r.diagnostic_session_open=true;
}
int main() {
    for(bool video:{false,true}) {
        Receiver r;configure(r);
        const size_t limit=video?r.max_video_packet_bytes:1024*1024;
        std::vector<uint8_t> data(limit+1,0);
        clock_calls=0;r.enqueue(video,video?2:8,1,data.data(),int(limit));
        assert(r.packets.size()==1 && r.bytes==limit && !r.resets.pending());
        assert(r.session_resets==0 && r.accepted_video_packets==unsigned(video));
        for(unsigned count=1;count<=2;++count) {
            r.enqueue(video,video?2:8,1,data.data(),int(limit+1));
            assert(r.packets.size()==1 && r.bytes==limit);
            assert(r.resets.sources==16 && r.resets.failure() && r.resets.epoch==0);
            assert(r.session_resets==count && r.overflow==count && r.dropped_packets==count);
            assert(r.dropped_video_frames==(video?count:0));
            assert(r.accepted_video_packets==unsigned(video));
        }
        ResetNotice notice=r.take_reset();assert(notice.sources==16 && !r.resets.pending());
        r.request_reset(ResetOrigin::SenderTeardown,1);
        assert(r.session_resets==3 && !r.take_reset().failure());
        r.begin_close();assert(r.transport_epoch==1 && !r.ingress.accepting && r.ingress.closing);
        // Closed ingress rejects even a would-be oversized packet without a new reset.
        r.enqueue(video,2,1,data.data(),int(limit+1));
        assert(r.session_resets==3 && !r.resets.pending() && r.packets.size()==1);
        assert(!current_audio_epoch(r.packets.front().transport_epoch,r.transport_epoch.load()));
    }
    // The production copy/relock path must reject retired transport epochs, even
    // if a new admission opens the gate before the old copy gets the lock.
    for(bool readmit:{false,true}) {
        Receiver r;configure(r);uint8_t data=0;clock_calls=0;
        after_copy=[&](){r.begin_close();if(readmit){r.ingress.disconnected();assert(r.ingress.admit());}};
        r.enqueue(true,2,1,&data,1);after_copy={};
        assert(r.packets.empty() && r.bytes==0 && r.accepted_video_packets==0 && r.total_video_arrivals==0);
        assert(!r.resets.pending() && r.session_resets==0);
        if(readmit) {
            clock_calls=0;r.enqueue(true,2,2,&data,1);
            assert(r.packets.size()==1 && r.packets.front().transport_epoch==1 && r.accepted_video_packets==1);
        }
    }
    // Audio FLUSH invalidates an in-flight audio copy, without invalidating video.
    for(bool video:{false,true}) {
        Receiver r;configure(r);uint8_t data=0;clock_calls=0;
        after_copy=[&](){++r.audio_epoch;};r.enqueue(video,video?2:8,1,&data,1);after_copy={};
        assert(r.packets.size()==unsigned(video) && !r.resets.pending());
    }
    Receiver capacity;configure(capacity);capacity.max_queue_bytes=0;uint8_t data=0;clock_calls=0;
    capacity.enqueue(true,2,1,&data,1);
    assert(capacity.session_resets==1 && capacity.resets.sources==8 && capacity.accepted_video_packets==0);
    DecoderCallbackDiagnostics callbacks;callbacks.started();assert(callbacks.total(3)==3);
    callbacks.retire(4);callbacks.retire(99);assert(callbacks.total(99)==4);
    callbacks.started();assert(callbacks.total(2)==6);callbacks.reset();assert(callbacks.total(99)==0);
    assert(std::string(reset_notice_origin(ResetNotice()))=="none");
    puts("PASS: production ingress size boundaries, oversize mask/accounting, closing ingress, transport/audio epochs and callback totals");
}
