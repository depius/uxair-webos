#include "receiver.h"
#include <algorithm>
#include <cstdio>
// Shared monotonic clock implemented by shell.cpp; tests supply a deterministic clock.
int64_t now_ns();

// The first notice is consumed once. Additional origins are retained for
// classification until take_reset closes ingress and invalidates the epoch.
bool Receiver::record_reset_locked(ResetOrigin origin,int reason,unsigned epoch) {
    ++session_resets;
    if(!ingress.accepts(epoch,transport_epoch.load())) {
        ++suppressed_resets;
        return false;
    }
    if(!resets.add(origin,reason,epoch)) {
        ++suppressed_resets;
        return false;
    }
    return true;
}
void Receiver::request_reset(ResetOrigin origin,int reason) {
    request_reset(origin,reason,transport_epoch.load());
}
void Receiver::request_reset(ResetOrigin origin,int reason,unsigned epoch) {
    std::lock_guard<std::mutex> lock(mutex);
    if(record_reset_locked(origin,reason,epoch))
        fprintf(stderr,"[session] reset origin=%s reason=%d epoch=%u closing=0\n",
            reset_origin_name(origin),reason,epoch);
}
ResetNotice Receiver::take_reset() {
    std::lock_guard<std::mutex> lock(mutex);
    ResetNotice result=resets;
    resets=ResetNotice();
    if(!result.pending())return result;
    if(!ingress.accepts(result.epoch,transport_epoch.load())) {
        ++suppressed_resets;
        return ResetNotice();
    }
    ingress.close();
    ++transport_epoch;
    ++effective_resets;
    return result;
}
void Receiver::begin_close() {
    std::lock_guard<std::mutex> lock(mutex);
    if(!ingress.closing){ingress.close();++transport_epoch;}
    if(resets.pending())++suppressed_resets;
    resets=ResetNotice();
}
void Receiver::set_mirror_audio_schedule(uint64_t origin,int64_t start) {
    std::lock_guard<std::mutex> lock(mutex);
    mirror_audio_origin=origin;
    mirror_audio_start=start;
    if(!origin) {
        last_mirror_video_feed=0;
        return;
    }
    // These packets would be rejected by the main loop's negative-PTS check.
    // Remove them before the next decoder's first-feed wait.
    for(auto i=packets.begin();i!=packets.end();) {
        if(i->video || i->timestamp>=origin) { ++i; continue; }
        bytes-=i->data.size();
        i=packets.erase(i);
        ++audio_before_origin;
        ++dropped_packets;
    }
}
Receiver::AudioPressure Receiver::relieve_audio_pressure(const Packet& incoming,int64_t now) {
    constexpr int64_t recent_video_feed_ns=1000000000LL;
    const int64_t last_video=last_mirror_video_feed.load();
    if(!mirror_audio_origin || paused || !last_video || now-last_video>recent_video_feed_ns)
        return AudioPressure::None;
    const uint64_t elapsed=uint64_t(std::max<int64_t>(0,now-mirror_audio_start));
    const auto future=[&](uint64_t pts) {
        return pts>=mirror_audio_origin && pts-mirror_audio_origin>elapsed;
    };
    // Keep due audio in timestamp order. When the packet count is full,
    // shed the farthest-future audio instead of rejecting a video access unit.
    auto victim=packets.end();
    for(auto i=packets.begin();i!=packets.end();++i) {
        if(i->video || !future(i->timestamp))continue;
        if(victim==packets.end() || i->timestamp>=victim->timestamp)victim=i;
    }
    if(!incoming.video && future(incoming.timestamp) &&
       (victim==packets.end() || incoming.timestamp>=victim->timestamp)) {
        ++audio_pressure_drops;
        ++dropped_packets;
        return AudioPressure::DropIncoming;
    }
    if(victim==packets.end())return AudioPressure::None;
    bytes-=victim->data.size();
    packets.erase(victim);
    ++audio_pressure_drops;
    ++dropped_packets;
    return AudioPressure::FreedSlot;
}
void Receiver::enqueue(bool video,unsigned codec,uint64_t time,const uint8_t *data,int size) {
    enqueue(video,codec,time,data,size,transport_epoch.load());
}
void Receiver::enqueue(bool video,unsigned codec,uint64_t time,const uint8_t *data,int size,unsigned transport) {
    if (!data || size<=0 || !time) return;
    unsigned epoch;size_t packet_limit;
    {
        std::lock_guard<std::mutex> l(mutex);
        if(!ingress.accepts(transport,transport_epoch.load()))return;
        epoch=audio_epoch.load();
        packet_limit=video?max_video_packet_bytes:size_t(1024*1024);
        if(size_t(size)>packet_limit) {
            ++overflow;++dropped_packets;if(video)++dropped_video_frames;
            if(!record_reset_locked(ResetOrigin::Oversize,0,transport))return;
            // Log the first notice with fixed-size fields and no packet contents.
            // Generation is the last started session; active=0 disambiguates pre-Load.
            fprintf(stderr,"[ingress] rejected=oversize kind=%s incoming_bytes=%d limit_bytes=%zu video_codec=%s packet_codec=%u epoch=%u generation=%u generation_active=%d queue_packets=%zu queue_bytes=%zu closing=%d accepting=%d\n",
                video?"video":"audio",size,packet_limit,ingress_video_codec_name(incoming_video_codec.load()),codec,
                transport,diagnostic_generation.load(),int(diagnostic_session_open.load()),packets.size(),bytes,
                int(ingress.closing),int(ingress.accepting));
            return;
        }
    }
    const int64_t arrival=now_ns();
    if(!video) {
        int64_t offset=int64_t(audio_delay_ms.load())*1000000;
        if(offset<0 && time<uint64_t(-offset))return;
        time=offset>=0?time+uint64_t(offset):time-uint64_t(-offset);
    }
    // Copy callback-owned memory before taking the A/V queue lock.
    Packet p; p.transport_epoch=transport;p.audio_epoch=epoch;p.video=video;p.codec=codec;p.timestamp=time;p.arrival_ns=arrival;
    p.data.assign(data,data+size);
    const int64_t copy_done=now_ns();
    std::lock_guard<std::mutex> l(mutex);
    if(!ingress.accepts(transport,transport_epoch.load()))return;
    if(!video && !current_audio_epoch(epoch,audio_epoch.load()))return;
    activity=arrival;
    if(video){last_video_arrival=arrival;++total_video_arrivals;}
    else {last_audio_arrival=arrival;last_audio_codec=codec;}
    const int64_t lock_acquired=now_ns();
    ingress_copy.record(uint64_t(std::max<int64_t>(0,copy_done-arrival)));
    queue_lock_wait.record(uint64_t(std::max<int64_t>(0,lock_acquired-copy_done)));
    if(video) video_timing.record(time,arrival);
    if(!video && mirror_audio_origin && time<mirror_audio_origin) {
        ++audio_before_origin;
        ++dropped_packets;
        return;
    }
    const bool byte_limit=bytes+size>max_queue_bytes;
    if(!byte_limit && packets.size()>=kPacketLimit &&
       relieve_audio_pressure(p,lock_acquired)==AudioPressure::DropIncoming)return;
    if (byte_limit || packets.size()>=kPacketLimit) {
        ++overflow;++dropped_packets;if(video)++dropped_video_frames;
        if(record_reset_locked(ResetOrigin::Capacity,0,transport))fprintf(stderr,"[queue] reset-cause=capacity epoch=%u video=%d packets=%zu bytes=%zu incoming=%d limit=%zu bound=%s audio_packets=%zu video_packets=%zu audio_bytes=%zu video_bytes=%zu\n",
            transport_epoch.load(),int(video),packets.size(),bytes,size,max_queue_bytes,
            byte_limit?(packets.size()>=kPacketLimit?"bytes+packets":"bytes"):"packets",
            packets.size()-video_packets,video_packets,bytes-video_bytes,video_bytes);
        return;
    }
    p.enqueued_ns=now_ns();
    ingress_to_queue.record(uint64_t(std::max<int64_t>(0,p.enqueued_ns-arrival)));
    bytes+=size; packets.push_back(std::move(p));
    peak_packets=std::max(peak_packets,packets.size());peak_bytes=std::max(peak_bytes,bytes);
    if(video) {
        ++accepted_video_packets;
        ++video_enqueued;
        ++video_packets;video_bytes+=size;
        peak_video_packets=std::max(peak_video_packets,video_packets);
        peak_video_bytes=std::max(peak_video_bytes,video_bytes);
    } else ++audio_enqueued;
    peak_audio_packets=std::max(peak_audio_packets,packets.size()-video_packets);
    peak_audio_bytes=std::max(peak_audio_bytes,bytes-video_bytes);
}

bool Receiver::pop(Packet& p,uint64_t latest,bool audio_only) {
    const int64_t pop_begin=now_ns();
    std::lock_guard<std::mutex> l(mutex);
    if(!ingress.accepting || packets.empty()) return false;
    // A/V callbacks arrive on different threads. Select the oldest presentation time.
    auto i=packets.end();
    for(auto candidate=packets.begin();candidate!=packets.end();++candidate) {
        if(audio_only && candidate->video)continue;
        if(i==packets.end() || candidate->timestamp<i->timestamp)i=candidate;
    }
    if(i==packets.end() || i->timestamp>latest)return false;
    p=std::move(*i);bytes-=p.data.size();packets.erase(i);
    p.dequeued_ns=now_ns();
    uint64_t residence=uint64_t(std::max<int64_t>(0,p.dequeued_ns-p.enqueued_ns));
    if(p.video) {
        ++video_dequeued;
        --video_packets;video_bytes-=p.data.size();
        video_queue_residence.record(residence);
    } else {
        ++audio_dequeued;
        audio_queue_residence.record(residence);
    }
    queue_pop.record(uint64_t(std::max<int64_t>(0,now_ns()-pop_begin)));
    return true;
}

void Receiver::flush_audio_packets(unsigned transport) {
    std::lock_guard<std::mutex> l(mutex);
    if(!ingress.accepts(transport,transport_epoch.load()))return;
    ++audio_epoch;
    for(auto i=packets.begin();i!=packets.end();) {
        if(!i->video) { bytes-=i->data.size(); i=packets.erase(i); }
        else ++i;
    }
    audio_flush=true;
    fprintf(stderr,"[session] audio-flush epoch=%u\n",audio_epoch.load());
}
