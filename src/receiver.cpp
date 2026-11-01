#include "receiver.h"
#include "receiver_lifecycle.h"
#include "shell.h"
#include "identity.h"
#include "capabilities.h"
#include "pairing_registration.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>

static Receiver& self(void *p) { return *static_cast<Receiver *>(p); }
extern "C" void uxair_sender_video_reset(void *p,int reason) {
    self(p).request_reset(ResetOrigin::SenderTeardown,reason);
}
static void audio_snapshot(const std::deque<Packet>& packets,ReceiverDiagnostics& d) {
    for(const auto& p:packets)if(!p.video) {
        ++d.audio_queue_packets;d.audio_queue_bytes+=p.data.size();
        if(!d.audio_pts_min || p.timestamp<d.audio_pts_min)d.audio_pts_min=p.timestamp;
        d.audio_pts_max=std::max(d.audio_pts_max,p.timestamp);
    }
}
static void log_message(void *,int level,const char *text) {
    if (level<=LOGGER_INFO) fprintf(stderr,"[airplay] %s\n",text);
}
NowPlaying Receiver::now_playing() {
    NowPlaying copy;
    {std::lock_guard<std::mutex> l(meta_mutex);copy=meta;}
    std::lock_guard<std::mutex> l(mutex);copy.source=client_name;
    return copy;
}
void Receiver::show(const std::string& s) { std::lock_guard<std::mutex> l(mutex);message=s; }
std::string Receiver::status() { std::lock_guard<std::mutex> l(mutex);return message; }
std::string Receiver::pin() { std::lock_guard<std::mutex> l(mutex);return pairing.pin(); }
bool Receiver::idle_for_profile_change() {
    std::lock_guard<std::mutex> l(mutex);
    return client.empty() && !pairing.visible() && !mirror.load();
}
ReceiverDiagnostics Receiver::diagnostics() {
    std::lock_guard<std::mutex> l(mutex);
    ReceiverDiagnostics d;
    audio_snapshot(packets,d);
    d.accepted_video_packets=accepted_video_packets;
    d.queue_packets=packets.size();d.queue_bytes=bytes;
    d.peak_queue_packets=peak_packets;d.peak_queue_bytes=peak_bytes;
    d.video_queue_packets=video_packets;d.video_queue_bytes=video_bytes;
    d.peak_video_queue_packets=peak_video_packets;d.peak_video_queue_bytes=peak_video_bytes;
    d.dropped_packets=dropped_packets;d.dropped_video_frames=dropped_video_frames;
    d.ingress_to_queue=ingress_to_queue.take_window();
    d.ingress_copy=ingress_copy.take_window();
    d.queue_lock_wait=queue_lock_wait.take_window();
    d.queue_pop=queue_pop.take_window();
    d.video_queue_residence=video_queue_residence.take_window();
    d.audio_queue_residence=audio_queue_residence.take_window();
    return d;
}
ReceiverDiagnostics Receiver::queue_snapshot() {
    std::lock_guard<std::mutex> l(mutex);
    ReceiverDiagnostics d;
    audio_snapshot(packets,d);
    d.accepted_video_packets=accepted_video_packets;
    d.queue_packets=packets.size(); d.queue_bytes=bytes;
    d.video_queue_packets=video_packets; d.video_queue_bytes=video_bytes;
    d.dropped_packets=dropped_packets; d.dropped_video_frames=dropped_video_frames;
    return d;
}
VideoTimingSample Receiver::take_video_timing() {
    std::lock_guard<std::mutex> l(mutex);
    return video_timing.take_window();
}
bool Receiver::pop(Packet& p,uint64_t latest) {
    const int64_t pop_begin=now_ns();
    std::lock_guard<std::mutex> l(mutex);
    if(packets.empty()) return false;
    // A/V callbacks arrive on different threads. Select the oldest presentation time.
    auto i=std::min_element(packets.begin(),packets.end(),[](const Packet& a,const Packet& b){return a.timestamp<b.timestamp;});
    if(i->timestamp>latest)return false;
    p=std::move(*i);bytes-=p.data.size();packets.erase(i);
    p.dequeued_ns=now_ns();
    uint64_t residence=uint64_t(std::max<int64_t>(0,p.dequeued_ns-p.enqueued_ns));
    if(p.video) {
        --video_packets;video_bytes-=p.data.size();
        video_queue_residence.record(residence);
    } else audio_queue_residence.record(residence);
    queue_pop.record(uint64_t(std::max<int64_t>(0,now_ns()-pop_begin)));
    return true;
}
void Receiver::clear() { std::lock_guard<std::mutex> l(mutex);++transport_epoch;++audio_epoch;packets.clear();bytes=video_packets=video_bytes=0;accepted_video_packets=0;pairing.reset();client.clear();client_name.clear();video_timing.reset();ingress_to_queue.reset();ingress_copy.reset();queue_lock_wait.reset();queue_pop.reset();video_queue_residence.reset();audio_queue_residence.reset();last_video_arrival=0; }
void Receiver::clear_audio_packets() {
    std::lock_guard<std::mutex> l(mutex);
    ++audio_epoch;
    for(auto i=packets.begin();i!=packets.end();) {
        if(!i->video) { bytes-=i->data.size(); i=packets.erase(i); }
        else ++i;
    }
}
static bool valid_key(const char *p) {
    if(!p || strlen(p)!=44) return false;
    return strspn(p,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")==44;
}
bool Receiver::start(const std::string& dir, const Settings& settings) {
    const AdvertisedCapabilities capabilities=advertised_capabilities(settings);
    active_profile=settings;validate_settings(active_profile);
    const MirrorLimits limits=mirror_limits(active_profile);
    max_video_packet_bytes=limits.video_packet_bytes;
    max_queue_bytes=limits.queue_bytes;
    width=capabilities.width;height=capabilities.height;
    directory=dir; mkdir(directory.c_str(),0700);
    std::ifstream audio_delay_file(directory+"/audio-delay-ms");int delay;
    if(audio_delay_file>>delay)audio_delay_ms=std::max(-1000,std::min(1000,delay));
    std::ifstream f(directory+"/paired.keys");std::string line;
    while(std::getline(f,line)) if(valid_key(line.c_str())) keys.insert(line);
    uint8_t mac[6];
    FILE *m=fopen((directory+"/device.id").c_str(),"rb");
    bool existing=m && fread(mac,1,6,m)==6;if(m) fclose(m);
    if(!existing) {
        if(RAND_bytes(mac,6)!=1) return false;
        mac[0]=(mac[0]&0xfe)|2;
        m=fopen((directory+"/device.id").c_str(),"wb");
        if(!m) return false;
        bool saved=fwrite(mac,1,6,m)==6;fclose(m);if(!saved) return false;
    }
    char device_id[18];snprintf(device_id,sizeof(device_id),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    int err=0;
    dnssd=dnssd_init(UXAIR_DISPLAY_NAME,strlen(UXAIR_DISPLAY_NAME),reinterpret_cast<char *>(mac),6,1,&err);
    if(!dnssd || err) { show("AirPlay discovery failed");return false; }
    dnssd_set_airplay_features(dnssd,0,int(capabilities.direct_video));dnssd_set_airplay_features(dnssd,4,int(capabilities.hls));
    dnssd_set_airplay_features(dnssd,42,int(capabilities.hevc));dnssd_set_airplay_features(dnssd,27,int(capabilities.legacy_pairing));
    raop_callbacks_t cb={};cb.cls=this;
    cb.audio_process=[](void *p,raop_ntp_t *,audio_decode_struct *d){self(p).enqueue(false,d->ct,d->ntp_time_remote,d->data,d->data_len);};
    cb.video_process=[](void *p,raop_ntp_t *,video_decode_struct *d){
        Receiver& r=self(p);
        if(!d->is_h265 || r.active_profile.codec==MirrorCodec::ExperimentalHevc)
            r.enqueue(true,d->is_h265?2:0,d->ntp_time_remote,d->data,d->data_len);
    };
    cb.conn_feedback=[](void *p){self(p).activity=now_ns();};
    cb.conn_reset=[](void *p,int reason){self(p).request_reset(ResetOrigin::TransportCallback,reason);};
    cb.video_reset=[](void *p,reset_type_t reason){self(p).request_reset(ResetOrigin::VideoCallback,int(reason));};
    cb.video_pause=[](void *p){self(p).paused=true;};
    cb.video_resume=[](void *p){self(p).paused=false;};
    cb.audio_flush=[](void *p){Receiver& r=self(p);r.clear_audio_packets();r.audio_flush=true;
        fprintf(stderr,"[session] audio-flush epoch=%u\n",r.audio_epoch.load());};
    // UxPlay also calls video_flush for short-lived discovery HTTP connections.
    // The RTP lifecycle uses video_reset, so that is our session teardown boundary.
    cb.video_flush=[](void *){};
    cb.audio_set_client_volume=[](void *)->double{return 0.0;};
    cb.audio_set_volume=[](void *p,float db){self(p).volume=db<=-144?0:std::min(1.0f,std::pow(10.0f,db/20.0f));};
    cb.audio_get_format=[](void *p,unsigned char *ct,unsigned short *spf,bool *using_screen,bool *is_media,uint64_t *format){
        Receiver& r=self(p);
        r.has_audio=true;
        r.last_audio_codec=ct?*ct:0;
        r.audio_samples_per_frame=spf?*spf:0;
        r.audio_using_screen=using_screen&&*using_screen;
        r.audio_is_media=is_media&&*is_media;
        r.audio_format=format?*format:0;
        fprintf(stderr,"[audio-negotiation] codec=%u samples_per_frame=%u using_screen=%d is_media=%d format=0x%llx standalone_candidate=%d\n",
            ct?unsigned(*ct):0,spf?unsigned(*spf):0,int(r.audio_using_screen.load()),int(r.audio_is_media.load()),
            (unsigned long long)r.audio_format.load(),int(standalone_audio_candidate(
                AirPlayAudioIntent{ct?unsigned(*ct):0,spf?unsigned(*spf):0,r.audio_using_screen.load(),r.audio_is_media.load(),true},r.mirror.load())));
    };
    cb.video_report_size=[](void *p,float *,float *,float *w,float *h){
        Receiver& r=self(p);
        const int bound=r.active_profile.width;
        if(std::isfinite(*w)&&std::isfinite(*h)&&*w>0&&*h>0&&*w<=bound&&*h<=bound&&
           input_fits_mirror_profile(r.active_profile,int(*w),int(*h))) {
            r.width=int(*w);r.height=int(*h);
            fprintf(stderr,"[receiver] sender reported geometry=%dx%d\n",int(*w),int(*h));
        }
    };
    cb.video_set_codec=[](void *p,video_codec_t c)->int{
        Receiver& r=self(p);
        const SenderVideoCodec sender_codec=c==VIDEO_CODEC_H264?SenderVideoCodec::H264:
            c==VIDEO_CODEC_H265?SenderVideoCodec::Hevc:SenderVideoCodec::Unsupported;
        const CodecNegotiationResult result=codec_negotiation_result(r.active_profile,sender_codec);
        const bool accepted=result!=CodecNegotiationResult::Reject;
        const char *mode=r.active_profile.codec==MirrorCodec::ExperimentalHevc?"HEVC-capable":"H264-only";
        const char *selected=c==VIDEO_CODEC_H264?"H264":c==VIDEO_CODEC_H265?"HEVC":"unsupported";
        const char *outcome=result==CodecNegotiationResult::AcceptH264Fallback?"accepted-sender-fallback":
            accepted?"accepted":"rejected";
        fprintf(stderr,"[codec] mode=%s advertised-bit42=%d sender-selected=%s negotiation=%s limit=%dx%d\n",
                mode,int(r.active_profile.codec==MirrorCodec::ExperimentalHevc),selected,outcome,
                r.active_profile.width,r.active_profile.height);
        if(accepted) r.incoming_video_codec=c==VIDEO_CODEC_H265?2:1;
        return accepted?0:-1;
    };
    cb.mirror_video_running=[](void *p,bool running){self(p).mirror=running;};
    cb.report_client_request=[](void *p,char *id,char *model,char *name,bool *admit){
        Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);
        *admit=id && !r.ingress.closing && (r.client.empty()||r.client==id);
        fprintf(stderr,"[client] model=%.80s admitted=%d\n",model?model:"unknown",int(*admit));
        if(*admit) {r.ingress.admit();++r.stream_requests;r.client=id;r.client_name=name?std::string(name).substr(0,80):"";r.message=std::string("Connecting: ")+(name?std::string(name).substr(0,80):"device");r.activity=now_ns();}
    };
    cb.audio_set_metadata=[](void *p,const void *buffer,int length){
        Receiver& r=self(p);NowPlaying m;
        if(length<0 || !parse_now_playing(buffer,size_t(length),m))return;
        std::lock_guard<std::mutex> l(r.meta_mutex);
        // the cover is kept as it is: iOS sends a track's cover before its text
        if(m.title!=r.meta.title||m.artist!=r.meta.artist)r.meta.track_ns=now_ns();
        r.meta.title=m.title;r.meta.artist=m.artist;r.meta.album=m.album;++r.meta.generation;
        fprintf(stderr,"[now-playing] %.60s - %.60s (%.60s)\n",m.artist.c_str(),m.title.c_str(),m.album.c_str());
    };
    cb.audio_set_coverart=[](void *p,const void *buffer,int length){
        Receiver& r=self(p);
        // Allocate on the sender callback, outside the metadata lock. Readers
        // keep an immutable snapshot alive across replacement/disconnect.
        std::shared_ptr<const std::vector<uint8_t>> cover;
        const auto *bytes=static_cast<const uint8_t *>(buffer);
        if(length>0 && artwork_supported(bytes,size_t(length)))
            cover=std::make_shared<const std::vector<uint8_t>>(bytes,bytes+length);
        std::lock_guard<std::mutex> l(r.meta_mutex);
        r.meta.cover=std::move(cover);
        r.meta.cover_ns=now_ns();++r.meta.cover_generation;++r.meta.generation;
        fprintf(stderr,"[now-playing] cover %d bytes\n",length);
    };
    // Called when the sender tears its audio stream down - also when an iPhone
    // locks and sets the stream up again without resending the cover. The
    // cover stays; a track that arrives without one hides it (see main.cpp).
    cb.audio_stop_coverart_rendering=[](void *){};
    cb.audio_set_progress=[](void *p,uint32_t *start,uint32_t *current,uint32_t *end){
        Receiver& r=self(p);std::lock_guard<std::mutex> l(r.meta_mutex);
        if(start&&current&&end){r.meta.start=*start;r.meta.current=*current;r.meta.end=*end;r.meta.progress_ns=now_ns();++r.meta.generation;}
    };
    cb.display_pin=[](void *p,char *pin){Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);r.pairing.update_pin(pin?pin:"");r.message="Enter this code on your Apple device";};
    cb.passwd=[](void *,int *n)->const char *{*n=0;return nullptr;};
    cb.check_register=[](void *p,const char *key)->bool{Receiver& r=self(p);std::lock_guard<std::mutex> l(r.mutex);return valid_key(key)&&r.keys.count(key);};
    cb.register_client=[](void *p,const char *,const char *key,const char *){
        if(!valid_key(key)) return;
        Receiver& r=self(p);
        const int64_t began=now_ns();
        fprintf(stderr,"[pairing] register begin\n");
        bool dismissed=false;
        PairingRegistrationResult result=register_pairing_key(r.pairing_store_mutex,r.mutex,
            r.keys,r.pairing,r.message,r.directory,key,dismissed);
        const char *result_name=result==PairingRegistrationResult::Added?"complete":
            result==PairingRegistrationResult::AlreadyRegistered?"already-registered":
            result==PairingRegistrationResult::LimitReached?"key-limit":"persistence-failed";
        fprintf(stderr,"[pairing] register %s overlay_dismissed=%d elapsed_ms=%.2f\n",
                result_name,int(dismissed),(now_ns()-began)/1e6);
    };
    ntp_global_init();
    raop=raop_init(&cb);if(!raop) return false;
    // UxPlay's INFO log includes the pairing PIN verbatim; keep it out of app logs.
    raop_set_log_callback(raop,log_message,this);raop_set_log_level(raop,LOGGER_ERR);
    if(raop_init2(raop,0,device_id,(directory+"/server.key").c_str()))return false;
    if(raop_set_plist(raop,"width",capabilities.width) ||
       raop_set_plist(raop,"height",capabilities.height) ||
       raop_set_plist(raop,"refreshRate",capabilities.max_fps) ||
       raop_set_plist(raop,"maxFPS",capabilities.max_fps) ||
       raop_set_plist(raop,"pin",0)) return false;
    fprintf(stderr,"[receiver] selected-codec-mode=%s advertised-codecs=%s advertised-bit42=%d mirror-limit=%dx%d advertised_max_fps=%d sender-selects-actual-codec-and-rate=1 max-video-packet=%zu queue=%zu\n",
            capabilities.hevc?"HEVC-capable":"H264-only",
            capabilities.hevc?"H264+HEVC":"H264",int(capabilities.hevc),
            capabilities.width,capabilities.height,capabilities.max_fps,
            max_video_packet_bytes,max_queue_bytes);
    raop_set_dnssd(raop,dnssd);
    unsigned short udp[3]={0,0,0},tcp[2]={0,0},port=0;
    raop_set_udp_ports(raop,udp);raop_set_tcp_ports(raop,tcp);
    if(raop_start_httpd(raop,&port)<0)return false;
    raop_set_port(raop,port);
    if(dnssd_register_raop(dnssd,port)||dnssd_register_airplay(dnssd,port))return false;
    fprintf(stderr,"[receiver] UxAir ready port=%u, paired=%zu\n",port,keys.size());
    return true;
}
void Receiver::adjust_audio_delay(int delta) {
    audio_delay_ms=std::max(-1000,std::min(1000,audio_delay_ms.load()+delta));
    const std::string path=directory+"/audio-delay-ms";
    FILE *f=fopen((path+".tmp").c_str(),"w");
    if(f) {
        bool ok=fprintf(f,"%d\n",audio_delay_ms.load())>0;
        if(fflush(f)||fsync(fileno(f)))ok=false;
        if(fclose(f))ok=false;
        if(ok)rename((path+".tmp").c_str(),path.c_str());
    }
}
void Receiver::disconnect() {
    begin_close();
    if(raop) raop_remove_known_connections(raop);
    clear(); has_audio=false;mirror=false;paused=false;audio_flush=false;last_audio_arrival=0;incoming_video_codec=0;
    audio_using_screen=false;audio_is_media=false;audio_samples_per_frame=audio_format=0;
    {std::lock_guard<std::mutex> l(meta_mutex);const uint64_t g=meta.generation+1,c=meta.cover_generation+1;meta=NowPlaying();meta.generation=g;meta.cover_generation=c;}
    {std::lock_guard<std::mutex> l(mutex);ingress.disconnected();}
    show("Waiting for a connection");
}
void Receiver::stop() {
    begin_close();
    if(dnssd) {dnssd_unregister_airplay(dnssd);dnssd_unregister_raop(dnssd);}
    if(raop) {raop_destroy(raop);raop=nullptr;}
    if(dnssd) {dnssd_destroy(dnssd);dnssd=nullptr;}
}
Receiver::~Receiver() {stop();}
