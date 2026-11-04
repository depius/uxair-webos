#include "media.h"
#include "media_rate.h"
#include "media_payload.h"
#include "settings.h"
#include <StarfishMediaAPIs.h>
#include <AcbAPI.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <time.h>

// This firmware exports a three-argument wrapper; the NDK declaration omits taskId.
extern "C" int acb_video_info(long, const char *, long *) __asm__("AcbAPI_setMediaVideoData");
struct Media::Impl { std::mutex mutex; std::string video_info; bool retrying=false; };
static int video_info_number(const char *message, const char *field) {
    if(!message) return 0;
    std::string text(message);
    size_t pos=text.find(field);
    if(pos==std::string::npos) return 0;
    pos=text.find(':',pos);
    if(pos==std::string::npos) return 0;
    char *end=nullptr;
    long value=strtol(text.c_str()+pos+1,&end,10);
    return end!=text.c_str()+pos+1 && value>0 && value<=4096 ? int(value) : 0;
}
static int64_t monotonic_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return int64_t(t.tv_sec)*1000000000LL+t.tv_nsec; }
Media::Media() : impl_(new Impl) {}
Media::~Media() { close(); delete impl_; }
static void acb_event(long, long, long event, long app, long play, const char *reply) {
    fprintf(stderr, "[acb] event=%ld app=%ld play=%ld %s\n", event, app, play, reply ? reply : "");
}
void Media::event(int type, int64_t value, const char *message, void *user) {
    Media *m = static_cast<Media *>(user);
    if (type == PF_EVENT_TYPE_FRAMEREADY) { ++m->frames; m->last_frame_event_ns=monotonic_ns(); return; }
    if (type == PF_EVENT_TYPE_INT_NEED_DATA || type == PF_EVENT_TYPE_INT_ENOUGH_DATA || type == PF_EVENT_TYPE_INT_BUFFERLOW || type == 0x2a) return;
    fprintf(stderr, "[smp] event=0x%x value=%lld %s\n", type, (long long)value, message ? message : "");
    if (type == PF_EVENT_TYPE_STR_RESOURCE_INFO)
        fprintf(stderr,"[media-state] resource-info event=0x%x load-completed=0\n",type);
    switch (type) {
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
        m->ready = true;
        fprintf(stderr,"[media-state] load-completed event=0x%x\n",type);
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
        m->playing = true;
        fprintf(stderr,"[media-state] playing event=0x%x\n",type);
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM: m->ended = true; break;
    case PF_EVENT_TYPE_INT_ERROR:
    case PF_EVENT_TYPE_STR_ERROR: m->failed = true; break;
    case PF_EVENT_TYPE_STR_VIDEO_INFO:
        if (message) {
            m->video_info_width=video_info_number(message,"\"width\"");
            m->video_info_height=video_info_number(message,"\"height\"");
            std::lock_guard<std::mutex> lock(m->impl_->mutex);
            m->impl_->video_info = message;
            m->video_info_pending_ = true;
        }
        break;
    default: break;
    }
}
bool Media::open(const char *app_id, int width, int height, bool audio, int rate,
                 int video_source_buffer_bytes, int canvas_width, int canvas_height, MirrorCodec codec, int max_fps, bool audio_only) {
    open_failure_=MediaOpenFailure::None;
    MediaRateFields frame_rate;
    if ((!audio_only && (width<=0 || height<=0 || video_source_buffer_bytes<1048576 || video_source_buffer_bytes>2097152)) ||
        !media_rate_fields(max_fps,frame_rate) ||
        (!audio_only && codec==MirrorCodec::H264 && int64_t(width)*height>1920*1080) ||
        (!audio_only && codec==MirrorCodec::ExperimentalHevc && int64_t(width)*height>3840*2160)) {
        open_failure_=MediaOpenFailure::InvalidConfiguration;
        fprintf(stderr,"[media-init] failure=invalid-configuration\n");
        return false;
    }
    VideoWindow window={};
    if(!audio_only) {
        if (!canvas_width) canvas_width=width;
        if (!canvas_height) canvas_height=height;
        window=fit_video_window(width,height,canvas_width,canvas_height);
        if (!window.width || !window.height) {
            open_failure_=MediaOpenFailure::InvalidConfiguration;
            fprintf(stderr,"[media-init] failure=invalid-display-window\n");
            return false;
        }
    }
    close();
    ready = playing = ended = failed = false;
    frames = 0;
    video_info_width=video_info_height=0;
    acb_window_width=window.width;acb_window_height=window.height;
    last_frame_event_ns = 0;
    feed_video_accepted=feed_audio_accepted=0;
    feed_video_retries=feed_audio_retries=0;
    feed_video_rejected=0;
    feed_calls=0;
    feed_time_ns=feed_max_time_ns=0;
    play_attempts_=0;impl_->retrying=false;
    int64_t phase=monotonic_ns();
    fprintf(stderr,"[media-init] ACB create begin\n");
    acb_ = AcbAPI_create();
    fprintf(stderr,"[media-init] ACB create end ms=%.2f\n",(monotonic_ns()-phase)/1e6);
    if(!acb_) {
        open_failure_=MediaOpenFailure::AcbCreate;
        fprintf(stderr,"[media-init] failure=acb-create\n");
        close();return false;
    }
    phase=monotonic_ns();
    fprintf(stderr,"[media-init] ACB initialize begin\n");
    if (!AcbAPI_initialize(acb_, PLAYER_TYPE_MSE, app_id, acb_event)) {
        open_failure_=MediaOpenFailure::AcbInitialize;
        fprintf(stderr, "[media] ACB initialization failed\n"); close(); return false;
    }
    fprintf(stderr,"[media-init] ACB initialize end ms=%.2f\n",(monotonic_ns()-phase)/1e6);
    phase=monotonic_ns();
    fprintf(stderr,"[media-init] Starfish construct begin\n");
    api_ = new StarfishMediaAPIs(nullptr);
    fprintf(stderr,"[media-init] Starfish construct end ms=%.2f\n",(monotonic_ns()-phase)/1e6);
    phase=monotonic_ns();
    fprintf(stderr,"[media-init] ACB media ID begin\n");
    if (!AcbAPI_setMediaId(acb_, api_->getMediaID())) {
        open_failure_=MediaOpenFailure::MediaIdRegistration;
        fprintf(stderr, "[media] ACB media registration failed\n"); close(); return false;
    }
    fprintf(stderr,"[media-init] ACB media ID end ms=%.2f\n",(monotonic_ns()-phase)/1e6);
    const std::string payload=starfish_load_payload(app_id,codec==MirrorCodec::ExperimentalHevc?"H265":"H264",
        width,height,audio,rate,video_source_buffer_bytes,frame_rate,audio_only);
    fprintf(stderr,"[media-identity] acb-app-id=%s starfish-app-id=%s source=shared-runtime-id\n",app_id,app_id);
    fprintf(stderr, "[media] Load mode=%s max_fps=%d %s\n", audio_only?"audio-only":"mirroring",max_fps,payload.c_str());
    phase=monotonic_ns();
    loaded_ = api_->Load(payload.c_str(), event, this);
    fprintf(stderr,"[media-init] Starfish Load end ms=%.2f success=%d\n",(monotonic_ns()-phase)/1e6,int(loaded_));
    if (!loaded_) { open_failure_=MediaOpenFailure::StarfishLoad;close(); return false; }
    phase=monotonic_ns();
    if(!audio_only) {
        AcbAPI_setDisplayWindow(acb_, window.x, window.y, window.width, window.height, true, &task_);
        fprintf(stderr,"[media-init] ACB display window=%d,%d %dx%d source=%dx%d end ms=%.2f\n",
                window.x,window.y,window.width,window.height,width,height,(monotonic_ns()-phase)/1e6);
    } else fprintf(stderr,"[media-init] audio-only session has no ACB video window\n");
    return true;
}
void Media::tick() {
    if (!api_ || !loaded_ || failed) return;
    if (ready && !sink_attached_) {
        fprintf(stderr,"[media-init] ACB sink and loaded state begin\n");
        AcbAPI_setSinkType(acb_, SINK_TYPE_MAIN);
        AcbAPI_setState(acb_, APPSTATE_FOREGROUND, PLAYSTATE_LOADED, &task_);
        fprintf(stderr,"[media-init] ACB sink and loaded state end\n");
        sink_attached_ = true;
    }
    if (video_info_pending_.exchange(false)) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        fprintf(stderr,"[media-init] ACB video info begin\n");
        int result=acb_video_info(acb_, impl_->video_info.c_str(), &task_);
        fprintf(stderr,"[media-init] ACB video info end result=%d\n",result);
    }
    if (ready && !play_requested_) {
        ++play_attempts_;
        if(play_attempts_==1 || play_attempts_%100==0)fprintf(stderr,"[media-init] Starfish Play attempt=%u\n",play_attempts_);
        play_requested_ = api_->Play();
        if(play_requested_)fprintf(stderr,"[media-init] Starfish Play accepted after %u attempts\n",play_attempts_);
    }
    if (playing && sink_attached_ && !acb_playing_) {
        fprintf(stderr,"[media-init] ACB playing state begin\n");
        AcbAPI_setState(acb_, APPSTATE_FOREGROUND, PLAYSTATE_PLAYING, &task_);
        fprintf(stderr,"[media-init] ACB playing state end\n");
        // ACB only needs the transition once; playing remains observable by the caller.
        acb_playing_=true;
    }
}
FeedResult Media::feed(const void *data, size_t size, int64_t pts_ns, bool video) {
    if (!api_ || !loaded_ || failed || !data || !size) {
        if(video)++feed_video_rejected;
        return FeedResult::Error;
    }
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    char payload[200];
    snprintf(payload, sizeof(payload), "{\"bufferAddr\":\"%p\",\"bufferSize\":%zu,\"pts\":%lld,\"esData\":%d}",
             data, size, (long long)pts_ns, video ? 1 : 2);
    std::string reply = api_->Feed(payload);
    clock_gettime(CLOCK_MONOTONIC, &after);
    int64_t elapsed_signed=int64_t(after.tv_sec-before.tv_sec)*1000000000LL+int64_t(after.tv_nsec-before.tv_nsec);
    uint64_t elapsed=elapsed_signed>0?uint64_t(elapsed_signed):0;
    ++feed_calls;
    feed_time_ns.fetch_add(elapsed);
    uint64_t previous=feed_max_time_ns.load();
    while(previous<elapsed&&!feed_max_time_ns.compare_exchange_weak(previous,elapsed)) {}
    if (reply.find("Ok") != std::string::npos) {
        impl_->retrying=false;
        (video?feed_video_accepted:feed_audio_accepted).fetch_add(1);
        return FeedResult::Accepted;
    }
    if (reply.find("BufferFull") != std::string::npos || reply.find("Pending") != std::string::npos) {
        if(!impl_->retrying)fprintf(stderr,"[media] feed retry kind=%s reply_class=%s ready=%d playing=%d\n",
            video?"video":"audio",reply.find("BufferFull")!=std::string::npos?"BufferFull":"Pending",
            int(ready.load()),int(playing.load()));
        impl_->retrying=true;
        (video?feed_video_retries:feed_audio_retries).fetch_add(1);
        return FeedResult::Retry;
    }
    fprintf(stderr, "[media] Feed rejected: %s\n", reply.c_str());
    if(video)++feed_video_rejected;
    return FeedResult::Error;
}
void Media::eos() { if (api_ && loaded_) api_->pushEOS(); }
void Media::pause(bool paused) {if(api_&&loaded_) {fprintf(stderr,"[media-state] pause-request=%d ready=%d playing=%d\n",int(paused),int(ready.load()),int(playing.load()));if(paused)api_->Pause();else api_->Play();}}
void Media::close() {
    if(acb_ && loaded_) AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_UNLOADED,&task_);
    if (api_) {
        if (loaded_) { api_->Unload(); fprintf(stderr, "[media] unloaded\n"); }
        delete api_; api_ = nullptr;
    }
    if (acb_) {
        AcbAPI_finalize(acb_); AcbAPI_destroy(acb_); acb_ = 0;
        fprintf(stderr, "[media] ACB released\n");
    }
    loaded_ = sink_attached_ = play_requested_ = acb_playing_ = false;
    play_attempts_=0;
    video_info_pending_ = false;
}
