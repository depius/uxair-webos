#include "media.h"
#include "media_rate.h"
#include "ndl_backend.h"
#include "settings.h"
#include <SDL.h>
#include <SDL_webOS.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <time.h>

// NDL reports Starfish's state events (StarfishMediaAPIs.h, PF_EVENT_TYPE_*).
enum {
    EV_FRAMEREADY = 0x0, EV_VIDEO_INFO = 0x4, EV_INT_ERROR = 0x12, EV_STR_ERROR = 0x13,
    EV_LOADCOMPLETED = 0x16, EV_UNLOADCOMPLETED = 0x17, EV_PLAYING = 0x1a, EV_ENDOFSTREAM = 0x1c,
    EV_NEED_DATA = 0x26, EV_ENOUGH_DATA = 0x27, EV_BUFFERLOW = 0x2c
};
// The C bridge pins this Media context while dispatching an active Load callback.

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
Media::Media() {}
Media::~Media() {
    close();
    const bool released=ndlb_quit()==0;
    if(!released)fprintf(stderr,"[ndl] cleanup-unconfirmed: exported window retained until process exit\n");
    if (released && !window_id_.empty()) { SDL_webOSDestroyExportedWindow(window_id_.c_str()); window_id_.clear(); }
}
void Media::event(void *context, int type, long long value, const char *message) {
    Media *m = static_cast<Media *>(context);
    if (!m) return;
    if (type == EV_FRAMEREADY) { m->last_frame_event_ns=monotonic_ns(); return; }
    if (type == EV_NEED_DATA || type == EV_ENOUGH_DATA || type == EV_BUFFERLOW || type == 0x2a) return;
    fprintf(stderr, "[ndl] event=0x%x value=%lld %.200s\n", type, value, message ? message : "");
    switch (type) {
    case EV_LOADCOMPLETED: m->ready = true; break;
    case EV_PLAYING:
        m->playing = true;
        fprintf(stderr,"[media-state] playing event=0x%x\n",type);
        break;
    case EV_ENDOFSTREAM: m->ended = true; break;
    case EV_INT_ERROR:
    case EV_STR_ERROR: m->failed = true; break;
    case EV_VIDEO_INFO:
        m->video_info_width=video_info_number(message,"\"width\"");
        m->video_info_height=video_info_number(message,"\"height\"");
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
    video_failures_=0;
    int64_t phase=monotonic_ns();
    if(!audio_only && window_id_.empty()) {
        // The video plane is bound to an exported window of our SDL window. It
        // is made once and kept: destroyed and recreated within one reinit
        // (the sender switching to a full-screen photo or video), the
        // compositor does not know the new one yet and NDL fails ("No matching
        // WebOSExported", "fail to create AVProxy").
        const char *id=SDL_webOSCreateExportedWindow(0);
        if(!id || !*id) {
            open_failure_=MediaOpenFailure::AcbCreate;
            fprintf(stderr,"[media-init] failure=exported-window %s\n",SDL_GetError());
            return false;
        }
        window_id_=id;
        SDL_PumpEvents();    // let the compositor learn the new window before NDL refers to it
    }
    const int video_codec=audio_only?NDLB_VIDEO_NONE:codec==MirrorCodec::ExperimentalHevc?NDLB_VIDEO_H265:NDLB_VIDEO_H264;
    const char *video_window=audio_only||window_id_.empty()?nullptr:window_id_.c_str();
    fprintf(stderr,"[media] Load mode=%s max_fps=%d app=%s %dx%d codec=%s window=%s\n",audio_only?"audio-only":"mirroring",max_fps,app_id,
            width,height,video_codec==NDLB_VIDEO_H265?"H265":video_codec==NDLB_VIDEO_H264?"H264":"none",video_window?video_window:"none");
    if (ndlb_open(app_id,video_window,video_codec,width,height,audio?rate:0,&Media::event,this)) {
        open_failure_=MediaOpenFailure::StarfishLoad;
        failed=true;close();
        return false;
    }
    loaded_=true;
    // NDL reports LOADCOMPLETED only once data flows; the caller waits for ready
    // before it feeds, so a successful Load counts as loaded.
    if(failed || ended) {open_failure_=MediaOpenFailure::StarfishLoad;close();return false;}
    ready=true;
    if(!audio_only) {
        SDL_Rect source={0,0,width,height}, target={window.x,window.y,window.width,window.height};
        bool placed=SDL_webOSSetExportedWindow(window_id_.c_str(),&source,&target);
        fprintf(stderr,"[media-init] video window=%d,%d %dx%d source=%dx%d placed=%d\n",
                window.x,window.y,window.width,window.height,width,height,int(placed));
        if(!placed) {
            open_failure_=MediaOpenFailure::InvalidConfiguration;failed=true;close();return false;
        }
    }
    fprintf(stderr,"[media-init] NDL ready ms=%.2f\n",(monotonic_ns()-phase)/1e6);
    return true;
}
// NDL plays as soon as data arrives: nothing to drive between feeds.
void Media::tick() {}
FeedResult Media::feed(const void *data, size_t size, int64_t pts_ns, bool video) {
    if (!loaded_ || failed || !data || !size) {
        if(video)++feed_video_rejected;
        return FeedResult::Error;
    }
    if(!video) {
        // a full audio buffer is not an error: the caller retries
        int available=ndlb_audio_free();
        if(available>=0 && size_t(available)<size) { ++feed_audio_retries; return FeedResult::Retry; }
    }
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    int r=video?ndlb_video(data,unsigned(size),pts_ns/1000):ndlb_audio(data,unsigned(size),pts_ns/1000);
    clock_gettime(CLOCK_MONOTONIC, &after);
    int64_t elapsed_signed=int64_t(after.tv_sec-before.tv_sec)*1000000000LL+int64_t(after.tv_nsec-before.tv_nsec);
    uint64_t elapsed=elapsed_signed>0?uint64_t(elapsed_signed):0;
    ++feed_calls;
    feed_time_ns.fetch_add(elapsed);
    uint64_t previous=feed_max_time_ns.load();
    while(previous<elapsed&&!feed_max_time_ns.compare_exchange_weak(previous,elapsed)) {}
    if (r==0) {
        (video?feed_video_accepted:feed_audio_accepted).fetch_add(1);
        if(video) { ++frames; video_failures_=0; }
        return FeedResult::Accepted;
    }
    if(!video) { ++feed_audio_retries; return FeedResult::Retry; }
    // a refused frame is usually a full decoder queue; a long run of them is an error
    if(++video_failures_<=120) { ++feed_video_retries; return FeedResult::Retry; }
    fprintf(stderr, "[media] video refused %u times in a row: %s\n", video_failures_, ndlb_error());
    ++feed_video_rejected;
    return FeedResult::Error;
}
// NDL has no end-of-stream or pause; the stream simply stops being fed.
void Media::eos() {}
void Media::pause(bool) {}
// The exported video window outlives a session (see open); only the destructor drops it.
void Media::close() {
    // The bridge retires/waits for callbacks even after a partially failed Load.
    const int result=ndlb_close();
    if(result) {failed=true;fprintf(stderr,"[ndl] close-failed: restart required\n");}
    loaded_=false;ready=false;playing=false;
}
