#include "receiver.h"

#include <cassert>
#include <cstdio>
#include <thread>

Receiver::~Receiver() {}
static int64_t clock_ns=1;
int64_t now_ns() { return clock_ns; }
static constexpr int64_t second=1000000000LL;

static void push(Receiver& r,bool video,uint64_t pts,unsigned epoch=0) {
    uint8_t data=video?0x26:0x01;
    r.enqueue(video,video?2:8,pts,&data,1,epoch);
}
static void open_mirror(Receiver& r) {
    r.audio_delay_ms=0;
    r.max_queue_bytes=16*1024*1024;
    r.set_mirror_audio_schedule(1,0);
    r.note_mirror_video_feed(clock_ns);
}
static void soak(bool audio) {
    clock_ns=1;
    Receiver r;
    open_mirror(r);
    unsigned audio_index=0, video_served=0, audio_served=0, rotations=0;
    const unsigned frames=60*60*8;
    for(unsigned i=1;i<=frames;++i) {
        clock_ns=int64_t(i)*second/60;
        push(r,true,1+clock_ns);
        while(audio && uint64_t(audio_index+1)*second/100<=uint64_t(clock_ns)) {
            ++audio_index;
            // Intermittent seven-second PTS lead models audio not eligible for dequeue.
            // The timestamps remain unchanged by pressure handling.
            const uint64_t lead=(i/1800)%2?7*second:0;
            push(r,false,1+uint64_t(audio_index)*second/100+lead);
        }
        if(i%1800==0) {
            r.set_mirror_audio_schedule(1+clock_ns,clock_ns);
            ++rotations;
        }
        Packet p;
        while(r.pop(p,1+clock_ns)) {
            if(p.video) {
                ++video_served;
                r.note_mirror_video_feed(clock_ns);
            } else ++audio_served;
        }
        assert(!r.resets.pending());
        assert(r.packets.size()<=Receiver::kPacketLimit && r.bytes<=r.max_queue_bytes);
    }
    assert(video_served==frames && rotations==16);
    if(audio)assert(audio_served>0 && r.audio_pressure_drops>0);
    else assert(r.audio_enqueued==0 && audio_served==0 && r.audio_pressure_drops==0);
}
static void pressure() {
    Receiver r;
    clock_ns=second;
    open_mirror(r);
    // Audio fills the packet cap with bytes to spare while its PTS is in the future.
    for(unsigned i=0;i<Receiver::kPacketLimit;++i)push(r,false,2*second+i);
    assert(!r.resets.pending() && r.bytes==Receiver::kPacketLimit);
    Packet p;
    assert(!r.pop(p,1+clock_ns));
    push(r,false,3*second);
    assert(r.audio_pressure_drops==1 && r.audio_enqueued==Receiver::kPacketLimit);
    push(r,true,clock_ns);
    assert(r.audio_pressure_drops==2 && r.packets.size()==Receiver::kPacketLimit);
    assert(r.pop(p,1+clock_ns) && p.video && p.data[0]==0x26);
    for(unsigned i=0;i<Receiver::kPacketLimit-1;++i)
        assert(r.pop(p,3*second) && !p.video && p.timestamp==uint64_t(2*second)+i);
    assert(!r.pop(p) && r.bytes==0 && !r.resets.pending());

    push(r,false,3*second);
    push(r,true,3*second);
    push(r,false,4*second);
    r.set_mirror_audio_schedule(4*second,clock_ns);
    assert(r.audio_before_origin==1 && r.packets.size()==2);
    assert(r.pop(p) && p.video);
    r.flush_audio_packets(0);
    assert(r.audio_flush && r.audio_epoch==1 && r.packets.empty() && r.bytes==0);
    r.paused=true;
    push(r,false,4*second);
    assert(r.packets.size()==1); // Resume audio remains admissible.
    r.paused=false;
    assert(r.pop(p) && p.audio_epoch==1);
}
static void pressure_preserves_due_audio() {
    Receiver r;
    clock_ns=second;
    open_mirror(r);
    for(unsigned i=0;i<Receiver::kPacketLimit;++i)push(r,false,i+1);
    push(r,false,2*second);
    assert(!r.resets.pending() && r.audio_pressure_drops==1);
    push(r,true,clock_ns);
    assert(r.resets.sources==8 && r.audio_pressure_drops==1);

    Receiver paused;
    open_mirror(paused);
    paused.paused=true;
    for(unsigned i=0;i<Receiver::kPacketLimit;++i)push(paused,false,2*second+i);
    push(paused,true,clock_ns);
    assert(paused.resets.sources==8 && paused.audio_pressure_drops==0);

    Receiver video_only;
    open_mirror(video_only);
    for(unsigned i=0;i<Receiver::kPacketLimit;++i)push(video_only,true,2*second+i);
    push(video_only,true,clock_ns);
    assert(video_only.resets.sources==8 && video_only.video_packets==Receiver::kPacketLimit);
}
static void reset_coalescing() {
    Receiver r;
    r.audio_delay_ms=0;
    for(unsigned i=0;i<Receiver::kPacketLimit;++i)push(r,false,i+1);
    // Standalone audio and hard byte limits retain the existing failure path.
    push(r,false,700);
    assert(r.resets.sources==8 && r.bytes<r.max_queue_bytes);
    std::thread workers[8];
    for(auto& worker:workers)worker=std::thread([&] {
        for(unsigned i=0;i<32;++i)r.request_reset(ResetOrigin::TransportCallback,1,0);
    });
    for(auto& worker:workers)worker.join();
    ResetNotice reset=r.take_reset();
    assert(reset.failure() && reset.epoch==0 && reset.sources==12);
    assert(r.effective_resets==1 && r.suppressed_resets==256);
    assert(!r.take_reset().pending() && r.transport_epoch==1);
    Packet pending;assert(!r.pop(pending));
    r.request_reset(ResetOrigin::VideoCallback,1,0);
    r.packets.clear();
    r.bytes=0;
    r.ingress.disconnected();
    assert(r.ingress.admit());
    push(r,false,1,0);
    r.flush_audio_packets(0);
    r.request_reset(ResetOrigin::SenderTeardown,1,0);
    assert(r.packets.empty() && r.audio_epoch==0 && !r.take_reset().pending());
    push(r,false,1,1);
    assert(r.packets.size()==1);
    r.request_reset(ResetOrigin::SenderTeardown,1,1);
    reset=r.take_reset();
    assert(reset.pending() && !reset.failure() && r.effective_resets==2);
    assert(r.suppressed_resets==258);
    Receiver byte_limit;
    open_mirror(byte_limit);
    byte_limit.max_queue_bytes=1;
    push(byte_limit,false,3*second);
    push(byte_limit,true,second);
    assert(byte_limit.take_reset().failure() && byte_limit.audio_pressure_drops==0);
}
int main() {
    soak(true);
    soak(false);
    pressure();
    pressure_preserves_due_audio();
    reset_coalescing();
    puts("PASS: accelerated 60 FPS A/V and silent runs, timed audio pressure, reinitialization, FLUSH, duplicate resets and recovery");
}
