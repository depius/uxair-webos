// Actual NDL sources under host mocks. No firmware playback/ABI claim.
#include "media.h"
#include "ndl_backend.h"
#include <NDL_directmedia.h>
#include <SDL.h>
#include <SDL_webOS.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

static bool fail_dl,fail_init,fail_load,fail_export,missing_binding,fail_placement;
static bool callback_on_unload,callback_on_quit,callback_inside_load;
static int binding_result,video_result,audio_result,unload_result,quit_result,audio_free=4096;
static int init_calls,load_calls,unload_calls,quit_calls,binding_calls,export_calls,destroy_calls,video_calls,audio_calls;
static long long video_pts,audio_pts;
static NDL_DIRECTMEDIA_DATA_INFO_T last_info;
static ndl_test_event_fn callback;
static std::atomic<bool> *callback_busy;
extern "C" {
bool NDL_DirectMedia_DL_IsInitialized(void) {return false;}
bool NDL_DirectMedia_DL_Initialize(void) {return !fail_dl;}
int NDL_DirectMediaInit(const char *id,void (*)(const char *)) {
    ++init_calls;assert(!strcmp(id,"com.github.depius.uxair"));return fail_init?-1:0;
}
int NDL_DirectMediaLoad(NDL_DIRECTMEDIA_DATA_INFO_T *info,ndl_test_event_fn fn) {
    ++load_calls;last_info=*info;callback=fn;
    if(callback_inside_load) {std::thread t([fn]{fn(0x12,0,nullptr);});t.join();}
    return fail_load?-1:0;
}
int NDL_DirectMediaUnload(void) {
    ++unload_calls;if(callback_busy)assert(!callback_busy->load());
    if(callback_on_unload&&callback)callback(0x12,0,nullptr);
    return unload_result;
}
int NDL_DirectMediaQuit(void) {++quit_calls;if(callback_on_quit&&callback)callback(0x12,0,nullptr);return quit_result;}
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
static void normal_and_retired() {
    ndl_test_event_fn retired;
    {
        Media m;assert(open_video(m));assert(m.ready&&!m.playing);
        assert(last_info.video.type==NDL_VIDEO_TYPE_H264&&last_info.video.width==1920);
        assert(last_info.audio.pcm.sampleRate==2&&!strcmp(last_info.audio.pcm.channelMode,"stereo")&&!strcmp(last_info.audio.pcm.format,"S16LE"));
        callback(0x1a,0,nullptr);assert(m.playing);
        unsigned char bytes[16]={};
        assert(m.feed(bytes,sizeof bytes,123456789,true)==FeedResult::Accepted&&video_pts==123456);
        audio_free=8;assert(m.feed(bytes,sizeof bytes,0,false)==FeedResult::Retry&&audio_calls==0);
        audio_free=4096;assert(m.feed(bytes,sizeof bytes,123456789,false)==FeedResult::Accepted&&audio_pts==123456);
        audio_result=-1;assert(m.feed(bytes,sizeof bytes,0,false)==FeedResult::Retry);audio_result=0;
        video_result=-1;for(int i=0;i<120;++i)assert(m.feed(bytes,sizeof bytes,0,true)==FeedResult::Retry);
        assert(m.feed(bytes,sizeof bytes,0,true)==FeedResult::Error);video_result=0;
        retired=callback;callback_on_unload=callback_on_quit=true;
        m.close();m.close();assert(unload_calls==1&&!m.failed&&!m.ready&&!m.playing);
        retired(0x12,0,nullptr);assert(!m.failed);
        assert(open_video(m,MirrorCodec::ExperimentalHevc,996,2160));
        assert(init_calls==1&&export_calls==1&&callback!=retired);
        assert(last_info.video.type==NDL_VIDEO_TYPE_H265&&last_info.video.height==2160);
        for(int type:{0x12,0x13,0x16,0x1a,0x1c,0x4,0})retired(type,0,reinterpret_cast<const char *>(1));
        assert(!m.failed&&!m.playing&&!m.ended&&m.video_info_width==0&&m.last_frame_event_ns==0);
        callback(0x4,0,"{\"width\":996,\"height\":2160}");assert(m.video_info_width==996);
        callback(0x1a,0,nullptr);assert(m.playing);
    }
    assert(destroy_calls==1&&quit_calls==1);
    retired(0x12,0,nullptr); // After destruction: must not dereference the old Media.
    {Media next;assert(open_video(next));retired(0x12,0,nullptr);assert(!next.failed);}
    puts("PASS: NDL retired callbacks rejected across reopen, Quit and Media destruction");
}
static void startup_failure(int kind) {
    fail_export=kind==0;fail_dl=kind==1;fail_init=kind==2;fail_load=kind==3;
    missing_binding=kind==4;binding_result=kind==5?-1:0;fail_placement=kind==6;callback_inside_load=kind==7;
    Media m;assert(!open_video(m));assert(!m.ready&&!m.playing);
    unsigned char b[4]={};assert(m.feed(b,4,0,true)==FeedResult::Error);
    if(callback) {callback(0x1a,0,nullptr);assert(!m.playing);}
    if(kind==0)assert(init_calls==0);
    if(kind==4||kind==5)assert(load_calls==0);
    if(kind==3||kind==6||kind==7)assert(unload_calls==1);
    if(kind==1||kind==2||kind==5) {int calls=init_calls;assert(!open_video(m));assert(init_calls==calls&&load_calls==0);}
}
static void failed_unload() {
    Media m;assert(open_video(m));ndl_test_event_fn retired=callback;
    unload_result=-1;m.close();assert(m.failed&&!m.ready&&unload_calls==1);
    assert(!open_video(m));assert(load_calls==1&&unload_calls==1);
    assert(ndlb_quit()==0); // Deinit succeeded, but the process remains poisoned.
    assert(!open_video(m));assert(load_calls==1&&init_calls==1);
    retired(0x1a,0,nullptr);assert(!m.playing);
    puts("PASS: NDL failed Unload retires callbacks and refuses reuse even after successful Quit");
}
static void failed_quit() {
    Media *m=new Media;assert(open_video(*m));ndl_test_event_fn retired=callback;
    quit_result=-1;delete m;assert(quit_calls==1&&destroy_calls==0);
    retired(0x12,0,nullptr);
    assert(ndlb_quit()==-1&&quit_calls==1);
    {Media next;assert(!open_video(next));assert(load_calls==1);}
    assert(quit_calls==1);
    puts("PASS: NDL failed Quit never reuses backend or destroys an unconfirmed exported window");
}
struct Target {std::atomic<bool> entered{false},release{false},busy{false};std::atomic<unsigned> calls{0};};
static void blocking_event(void *p,int,long long,const char *) {
    Target& t=*static_cast<Target *>(p);++t.calls;t.busy=true;t.entered=true;
    while(!t.release.load())std::this_thread::yield();
    t.busy=false;
}
static void count_event(void *p,int,long long,const char *) {++*static_cast<std::atomic<unsigned> *>(p);}
static void concurrent_close() {
    Target *target=new Target;callback_busy=&target->busy;
    assert(ndlb_open("com.github.depius.uxair","mock-window",NDLB_VIDEO_H264,1920,1080,44100,blocking_event,target)==0);
    ndl_test_event_fn retired=callback;
    std::thread emitter([retired]{retired(0x1a,0,nullptr);});
    while(!target->entered.load())std::this_thread::yield();
    std::atomic<bool> started{false},closed{false};
    std::thread closer([&]{started=true;assert(ndlb_close()==0);closed=true;});
    while(!started.load())std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(!closed.load());
    target->release=true;emitter.join();closer.join();assert(closed&&target->calls==1);
    callback_busy=nullptr;delete target;retired(0x12,0,nullptr);
    std::atomic<unsigned> fresh{0};assert(ndlb_open("com.github.depius.uxair","mock-window",1,1920,1080,44100,count_event,&fresh)==0);
    retired(0x12,0,nullptr);assert(fresh==0);callback(0x1a,0,nullptr);assert(fresh==1);
    assert(ndlb_quit()==0);
    puts("PASS: NDL retirement waits for in-flight callback before context destruction");
}
static void bounded_identities() {
    std::atomic<unsigned> count{0};std::vector<ndl_test_event_fn> identities;
    for(unsigned i=0;i<NDLB_CALLBACK_LIMIT;++i) {
        assert(ndlb_open("com.github.depius.uxair","mock-window",1,1920,1080,44100,count_event,&count)==0);
        for(auto prior:identities)assert(prior!=callback);
        identities.push_back(callback);callback(0,0,nullptr);
    }
    assert(count==NDLB_CALLBACK_LIMIT);
    assert(ndlb_open("com.github.depius.uxair","mock-window",1,1920,1080,44100,count_event,&count)==-1);
    assert(load_calls==NDLB_CALLBACK_LIMIT&&strstr(ndlb_error(),"exhausted"));
    for(auto prior:identities)prior(0x12,0,nullptr);
    assert(count==NDLB_CALLBACK_LIMIT&&ndlb_quit()==0);
    assert(ndlb_open("com.github.depius.uxair","mock-window",1,1920,1080,44100,count_event,&count)==-1);
    puts("PASS: NDL callback identities never wrap, including after Quit; exhaustion fails closed");
}
static void scenario(int n) {
    if(n==0)normal_and_retired();
    else if(n<=8)startup_failure(n-1);
    else if(n==9)failed_unload();
    else if(n==10)failed_quit();
    else if(n==11)concurrent_close();
    else if(n==12)bounded_identities();
    else {Media m;assert(m.open("com.github.depius.uxair",0,0,true,44100,1048576,0,0,MirrorCodec::H264,30,true));assert(last_info.video.type==0&&export_calls==0);}
}
int main() {
    // Each scenario is an independent process: a poisoned backend/callback pool
    // must never gain a production reset hook merely to make tests convenient.
    for(int n=0;n<14;++n) {
        pid_t child=fork();assert(child>=0);
        if(!child){scenario(n);exit(0);}
        int status=0;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    puts("PASS: actual NDL callback identity, concurrency, startup and teardown regression suite");
}
