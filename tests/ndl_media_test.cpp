// Runs the unchanged NDL Media/C bridge with controlled host API stubs.
// Does not simulate actual decoding, firmware threads, jail mounts or playback.
#include "media.h"
#include "ndl_backend.h"
#include <NDL_directmedia.h>
#include <SDL.h>
#include <SDL_webOS.h>
#include <cassert>
#include <cstdio>
#include <cstring>

static bool fail_dl,fail_init,fail_load,fail_export,missing_binding,fail_placement;
static int binding_result,video_result,audio_result,unload_result,quit_result,audio_free=4096;
static int init_calls,load_calls,unload_calls,quit_calls,binding_calls,export_calls,destroy_calls,video_calls,audio_calls;
static long long video_pts,audio_pts;
static NDL_DIRECTMEDIA_DATA_INFO_T last_info;
static ndl_test_event_fn callback;
static void controls() {
    fail_dl=fail_init=fail_load=fail_export=missing_binding=fail_placement=false;
    binding_result=video_result=audio_result=unload_result=quit_result=0;audio_free=4096;
}
extern "C" {
bool NDL_DirectMedia_DL_IsInitialized(void) {return 0;}
bool NDL_DirectMedia_DL_Initialize(void) {return !fail_dl;}
int NDL_DirectMediaInit(const char *id,void (*)(const char *)) {
    ++init_calls;assert(!strcmp(id,"com.github.depius.uxair"));return fail_init?-1:0;
}
int NDL_DirectMediaLoad(NDL_DIRECTMEDIA_DATA_INFO_T *info,ndl_test_event_fn fn) {
    ++load_calls;last_info=*info;callback=fn;return fail_load?-1:0;
}
int NDL_DirectMediaUnload(void) {++unload_calls;return unload_result;}
int NDL_DirectMediaQuit(void) {++quit_calls;return quit_result;}
const char *NDL_DirectMediaGetError(void) {return "mock error";}
int NDL_DirectMediaSetWindowId(const char *id) {++binding_calls;assert(!strcmp(id,"mock-window"));return binding_result;}
void *ndl_test_dlsym(void *,const char *name) {
    assert(!strcmp(name,"NDL_DirectMediaSetWindowId"));
    return missing_binding?nullptr:reinterpret_cast<void *>(&NDL_DirectMediaSetWindowId);
}
int NDL_DirectVideoPlay(void *,unsigned,long long pts) {++video_calls;video_pts=pts;return video_result;}
int NDL_DirectAudioPlay(void *,unsigned,long long pts) {++audio_calls;audio_pts=pts;return audio_result;}
int NDL_DirectAudioGetAvailableBufferSize(int *out) {*out=audio_free;return 0;}
const char *SDL_webOSCreateExportedWindow(int) {++export_calls;return fail_export?nullptr:"mock-window";}
void SDL_webOSDestroyExportedWindow(const char *id) {++destroy_calls;assert(!strcmp(id,"mock-window"));}
bool SDL_webOSSetExportedWindow(const char *,const SDL_Rect *,const SDL_Rect *) {return !fail_placement;}
void SDL_PumpEvents(void) {}
const char *SDL_GetError(void) {return "mock SDL error";}
}
static bool open_video(Media& m,MirrorCodec codec=MirrorCodec::H264,int w=1920,int h=1080) {
    return m.open("com.github.depius.uxair",w,h,true,44100,2097152,1920,1080,codec,60,false);
}
int main() {
    controls();
    {Media m;assert(!m.open("com.github.depius.uxair",0,1080,true));assert(m.open_failure()==MediaOpenFailure::InvalidConfiguration);}
    for(int failure=0;failure<4;++failure) {
        controls();fail_export=failure==0;fail_dl=failure==1;fail_init=failure==2;fail_load=failure==3;
        Media m;assert(!open_video(m));assert(!m.ready);unsigned char b[4]={};assert(m.feed(b,4,0,true)==FeedResult::Error);
    }
    controls();int exported=export_calls,initialized=init_calls,unloaded=unload_calls,destroyed=destroy_calls;
    {
        Media m;assert(open_video(m));assert(m.ready&&!m.playing);
        assert(last_info.video.type==NDL_VIDEO_TYPE_H264&&last_info.video.width==1920);
        assert(last_info.audio.pcm.sampleRate==2&&!strcmp(last_info.audio.pcm.channelMode,"stereo")&&!strcmp(last_info.audio.pcm.format,"S16LE"));
        callback(0x1a,0,nullptr);assert(m.playing);
        callback(0x4,0,"{\"width\":996,\"height\":2160}");assert(m.video_info_width==996&&m.video_info_height==2160);
        unsigned char bytes[16]={};
        assert(m.feed(bytes,sizeof bytes,123456789,true)==FeedResult::Accepted&&video_pts==123456);
        int fed=audio_calls;audio_free=8;assert(m.feed(bytes,sizeof bytes,123456789,false)==FeedResult::Retry&&audio_calls==fed);
        audio_free=4096;assert(m.feed(bytes,sizeof bytes,123456789,false)==FeedResult::Accepted&&audio_pts==123456);
        audio_result=-1;assert(m.feed(bytes,sizeof bytes,123456789,false)==FeedResult::Retry);audio_result=0;
        video_result=-1;for(int i=0;i<120;++i)assert(m.feed(bytes,sizeof bytes,0,true)==FeedResult::Retry);
        assert(m.feed(bytes,sizeof bytes,0,true)==FeedResult::Error);video_result=0;
        ndl_test_event_fn retired=callback;
        m.close();m.close();assert(unload_calls==unloaded+1);
        m.failed=false;retired(0x12,0,nullptr);assert(!m.failed); // No current session.
        assert(open_video(m,MirrorCodec::ExperimentalHevc,996,2160));
        assert(init_calls==initialized+1&&export_calls==exported+1); // Retained for process lifetime.
        assert(last_info.video.type==NDL_VIDEO_TYPE_H265&&last_info.video.height==2160);
        // Reproduce a known limitation, not a claim of protection: the vendor
        // callback carries no epoch. A retired callback reaches the new session.
        retired(0x12,0,nullptr);assert(m.failed);
        puts("OBSERVED: retired NDL callback can mark the reopened session failed; firmware delivery contract unverified");
    }
    assert(destroy_calls==destroyed+1);
    controls();
    {Media m;int video_windows=export_calls;assert(m.open("com.github.depius.uxair",0,0,true,44100,1048576,0,0,MirrorCodec::H264,30,true));assert(last_info.video.type==0&&export_calls==video_windows);}
    for(int limitation=0;limitation<3;++limitation) {
        controls();missing_binding=limitation==0;binding_result=limitation==1?-1:0;fail_placement=limitation==2;
        Media m;assert(open_video(m)); // Current behavior: these failures do not fail open.
    }
    puts("OBSERVED: missing/failed NDL window binding or placement does not reject open");
    controls();{Media m;assert(open_video(m));unload_result=-1;quit_result=-1;m.close();unsigned char b[4]={};assert(m.feed(b,4,0,true)==FeedResult::Error);}
    puts("OBSERVED: failed NDL Unload/Quit results are not propagated; firmware cleanup outcome unverified");
    assert(init_calls>0&&load_calls>0&&quit_calls>0&&binding_calls>0&&video_calls>0&&audio_calls>0);
    puts("PASS: actual NDL sources, startup failures, PCM/video feed, retries, reopens and cleanup under mocked APIs");
}
