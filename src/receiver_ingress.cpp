#include "receiver.h"
#include <algorithm>
#include <cstdio>
// Shared monotonic clock implemented by shell.cpp; tests supply a deterministic clock.
int64_t now_ns();

// All callers hold the queue mutex. Count each reset notification/rejection,
// including repeated events coalesced into the same pending reset notice.
bool Receiver::record_reset_locked(ResetOrigin origin,int reason,unsigned epoch) {
    ++session_resets;
    return resets.add(origin,reason,epoch);
}
void Receiver::request_reset(ResetOrigin origin,int reason) {
    std::lock_guard<std::mutex> l(mutex);
    record_reset_locked(origin,reason,transport_epoch.load());
    fprintf(stderr,"[session] reset origin=%s reason=%d epoch=%u closing=%d\n",
        reset_origin_name(origin),reason,transport_epoch.load(),int(ingress.closing));
}
ResetNotice Receiver::take_reset() {
    std::lock_guard<std::mutex> l(mutex);ResetNotice result=resets;resets=ResetNotice();return result;
}
void Receiver::begin_close() {
    std::lock_guard<std::mutex> l(mutex);
    if(!ingress.closing){ingress.close();++transport_epoch;}
}
void Receiver::enqueue(bool video,unsigned codec,uint64_t time,const uint8_t *data,int size) {
    if (!data || size<=0 || !time) return;
    unsigned transport,epoch;size_t packet_limit;
    {
        std::lock_guard<std::mutex> l(mutex);
        if(!ingress.accepting)return;
        transport=transport_epoch.load();epoch=audio_epoch.load();
        packet_limit=video?max_video_packet_bytes:size_t(1024*1024);
        if(size_t(size)>packet_limit) {
            ++overflow;++dropped_packets;if(video)++dropped_video_frames;
            record_reset_locked(ResetOrigin::Oversize,0,transport);
            // One fixed-size record per rejection, with no payload, names or IDs.
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
    if (bytes+size>max_queue_bytes || packets.size()>=512) {
        ++overflow;++dropped_packets;if(video)++dropped_video_frames;
        if(record_reset_locked(ResetOrigin::Capacity,0,transport))fprintf(stderr,"[queue] reset-cause=capacity epoch=%u video=%d packets=%zu bytes=%zu incoming=%d limit=%zu\n",
            transport_epoch.load(),int(video),packets.size(),bytes,size,max_queue_bytes);
        return;
    }
    p.enqueued_ns=now_ns();
    ingress_to_queue.record(uint64_t(std::max<int64_t>(0,p.enqueued_ns-arrival)));
    bytes+=size; packets.push_back(std::move(p));
    peak_packets=std::max(peak_packets,packets.size());peak_bytes=std::max(peak_bytes,bytes);
    if(video) {
        ++accepted_video_packets;
        ++video_packets;video_bytes+=size;
        peak_video_packets=std::max(peak_video_packets,video_packets);
        peak_video_bytes=std::max(peak_video_bytes,video_bytes);
    }
}
