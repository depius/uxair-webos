#include "receiver.h"
#include "hevc_startup.h"
#include "hevc_schedule.h"
#include <cassert>
#include <cstdio>
#include <algorithm>

Receiver::~Receiver() {}
static int64_t clock_ns=1;
int64_t now_ns(){return clock_ns;}
static void configure(Receiver& r) {
    Settings s;s.width=3840;s.height=2160;s.codec=MirrorCodec::ExperimentalHevc;
    const MirrorLimits limits=mirror_limits(s);
    r.max_video_packet_bytes=limits.video_packet_bytes;r.max_queue_bytes=limits.queue_bytes;
    r.audio_delay_ms=0;
}
struct Run {size_t peak=0;unsigned reopens=0;bool capacity=false;};
static Run rotations(bool corrected) {
    Receiver r;configure(r);StarfishHevcSchedule schedule;Run run;
    const int64_t second=1000000000LL,step=1000000LL,load=50000000LL;
    uint64_t origin=1;int64_t start=200000000LL,busy_until=0;unsigned vi=0,ai=0;
    Packet pending;bool have=false,reopened=false;uint8_t byte=0;
    for(clock_ns=step;clock_ns<30*second;clock_ns+=step) {
        while(vi<unsigned(clock_ns*60/second)) {++vi;byte=vi%60==0?1:0;r.enqueue(true,2,1+uint64_t(vi)*second/60,&byte,1);}
        while(ai<unsigned(clock_ns*100/second)) {++ai;r.enqueue(false,8,1+uint64_t(ai)*second/100,&byte,1);}
        run.peak=std::max(run.peak,r.packets.size());
        if(r.resets.pending()){assert(r.resets.sources==8);run.capacity=true;break;}
        if(clock_ns<busy_until)continue;
        for(unsigned service=0;service<50;++service) {
            if(!have) {
                have=r.pop(pending,origin+uint64_t(std::max<int64_t>(0,clock_ns-start)));
                reopened=false;if(!have)break;
            }
            int64_t pts=int64_t(pending.timestamp-origin);
            const int64_t due=corrected&&pending.video?schedule.feed_due(start+pts):start+pts;
            if(clock_ns<due)break;
            if(pending.video && pending.data[0]==1 && !reopened) {
                const int64_t completed=clock_ns+load;
                start=corrected?schedule.reinitialize(start,pts,completed):completed+StarfishHevcSchedule::kPrerollDelayNs;
                origin=pending.timestamp;busy_until=completed;reopened=true;++run.reopens;break;
            }
            have=false;
        }
    }
    return run;
}
int main() {
    const Run legacy=rotations(false),fixed=rotations(true);
    printf("PASS: repeated reopens: legacy capacity=%d peak=%zu, preserved timeline capacity=%d peak=%zu reopens=%u\n",int(legacy.capacity),legacy.peak,int(fixed.capacity),fixed.peak,fixed.reopens);
    assert(legacy.capacity && legacy.peak==512);
    assert(!fixed.capacity && fixed.peak<80 && fixed.reopens>legacy.reopens);
    StarfishHevcSchedule schedule;
    const int64_t due=schedule.reinitialize(100,50,1000000000LL);
    assert(due==150 && schedule.feed_due(due)==1200000000LL);
    assert(schedule.feed_due(1300000000LL)==1300000000LL); // Never feed future media early.
    schedule.reset();assert(schedule.feed_due(150)==150);

    Receiver r;configure(r);HevcStartupGate gate;gate.accept_load(1);
    for(unsigned i=0;i<60;++i)gate.accepted_video(4096,HevcFeedStage::Preroll);
    assert(gate.video_stage(false,false,4096)==HevcFeedStage::WaitingForLoad);
    Packet video;video.video=true;video.codec=3;video.timestamp=100;video.transport_epoch=0;video.data.assign(4096,0x26);
    PrerollVideoHold<Packet> hold;hold.defer(std::move(video));Packet pending;
    assert(!hold.restore_if_ready(pending,false,0));
    uint8_t data=1;r.enqueue(true,2,101,&data,1);r.enqueue(false,8,102,&data,1);r.enqueue(false,8,104,&data,1);
    assert(!r.pop(pending,101,true)); // Future audio remains queued; held video cannot block selection.
    unsigned audio_fed=0;
    while(r.pop(pending,104,true)){assert(!pending.video);++audio_fed;}
    assert(audio_fed==2 && r.packets.size()==1 && r.video_packets==1 && r.bytes==1);
    // A delayed callback dependent on audio can now arrive without feeding frame 61.
    const bool ready=audio_fed==2;
    assert(hold.restore_if_ready(pending,ready,0) && pending.video && pending.codec==3 && pending.data.size()==4096);
    assert(gate.preroll_frames()==60 && gate.preroll_bytes()==60*4096);
    assert(gate.video_stage(ready,false,pending.data.size())==HevcFeedStage::Loaded);
    assert(r.pop(video,104) && video.video); // Future video survives the geometry boundary.
    assert(!r.pop(video,104));

    hold.defer(std::move(pending));++r.audio_epoch;
    assert(hold.restore_if_ready(pending,true,0)); // Audio FLUSH does not discard a valid video AU.
    hold.defer(std::move(pending));r.begin_close();
    assert(!hold.restore_if_ready(pending,true,r.transport_epoch.load()) && !hold.pending());
    hold.clear();assert(!hold.pending());
    Receiver ordering;configure(ordering);
    ordering.enqueue(true,2,20,&data,1);ordering.enqueue(false,8,10,&data,1);
    assert(ordering.pop(pending,20) && !pending.video && pending.timestamp==10);
    assert(ordering.pop(pending,20) && pending.video && pending.timestamp==20);
    assert(!ordering.pop(pending,20)); // Default A/V dequeue retains timestamp order.
    gate.accept_load(100);
    assert(!gate.startup_timed_out(100+HevcStartupGate::kStartupTimeoutNs,false,false,false));
    assert(gate.startup_timed_out(101+HevcStartupGate::kStartupTimeoutNs,false,false,false));
    assert(!gate.startup_timed_out(101+HevcStartupGate::kStartupTimeoutNs,true,true,false));
    puts("PASS: bounded preroll, due audio progress, retained video, FLUSH, transport invalidation and unchanged startup deadline");
}
