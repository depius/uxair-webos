#include "direct_player.h"
#include <StarfishMediaAPIs.h>
#include <cassert>
#include <cstdio>
#include <AcbAPI.h>
#include <thread>
#include <cstdlib>
void (*StarfishMediaAPIs::callback)(int,int64_t,const char*,void*)=nullptr;
void *StarfishMediaAPIs::context=nullptr;
std::string StarfishMediaAPIs::payload,StarfishMediaAPIs::last_seek;
int StarfishMediaAPIs::unloads=0;
int foreground_calls=0,load_calls=0;
static bool foreground_result=true;
extern "C" bool test_foreground(StarfishMediaAPIs*) __asm__("_ZN17StarfishMediaAPIs16notifyForegroundEv");
extern "C" bool test_foreground(StarfishMediaAPIs*){++foreground_calls;return foreground_result;}
bool StarfishMediaAPIs::load_success=true,StarfishMediaAPIs::play_success=true,StarfishMediaAPIs::synchronous_load=false;
bool acb_initialize_success=true,acb_sink_success=true;int acb_player_type=-1,acb_sink_calls=0;
extern "C" int AcbAPI_setMediaVideoData(long,const char*,long*){return 1;}
int main(){
 DirectPlayer p;assert(p.open("https://media.example/vod.m3u8",0,1));
 assert(StarfishMediaAPIs::payload.find("HLS")!=std::string::npos);
 assert(foreground_calls==1&&load_calls==1);
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);
 assert(p.tick(2).state==DirectState::Playing);
 DirectCommand c;c.kind=DirectCommandKind::Pause;assert(p.control(c));assert(p.tick(3).state==DirectState::Paused);
 c.kind=DirectCommandKind::Resume;assert(p.control(c));c.kind=DirectCommandKind::Seek;c.position=1;assert(p.control(c));
 StarfishMediaAPIs::callback(0x1c,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(4).state==DirectState::Ended);p.close();
 assert(StarfishMediaAPIs::unloads==1);auto stale_context=StarfishMediaAPIs::context;auto stale_callback=StarfishMediaAPIs::callback;
 assert(p.open("https://media.example/vod.m3u8",0,1));stale_callback(0x1c,0,nullptr,stale_context);assert(p.tick(2).state==DirectState::Loading);assert(p.tick(16000000002LL).state==DirectState::Error);p.close();
 StarfishMediaAPIs::load_success=false;assert(!p.open("https://media.example/vod.m3u8",0,1));assert(p.tick(2).state==DirectState::Error);assert(p.snapshot().error==DirectError::LoadRejected);
 StarfishMediaAPIs::load_success=true;assert(p.open("https://media.example/vod.m3u8",0,1));
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).state==DirectState::Playing);p.close();
 assert(p.open("https://media.example/vod.m3u8",6.5,1,nullptr,true));
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).state==DirectState::Playing);
 assert(StarfishMediaAPIs::last_seek=="6500");p.close();
 assert(p.open("https://media.example/vod.m3u8",.25,1));
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).state==DirectState::Loading);
 StarfishMediaAPIs::callback(3,12000,nullptr,StarfishMediaAPIs::context);assert(p.tick(3).state==DirectState::Playing);
 assert(StarfishMediaAPIs::last_seek=="3000");p.close();
 assert(acb_player_type==PLAYER_TYPE_VIDEO);
 // Reproduce the exact B7 callbacks. None is an error or evidence of fetched data.
 assert(p.open("https://media.example/vod.m3u8",0,1));
 StarfishMediaAPIs::callback(0xd,1,reinterpret_cast<const char*>(1),StarfishMediaAPIs::context);
 for(int i=0;i<73;++i){StarfishMediaAPIs::callback(2,0,reinterpret_cast<const char*>(1),StarfishMediaAPIs::context);
     StarfishMediaAPIs::callback(1,0,"{\"state\":\"PRIVATE\",\"uri\":\"https://SECRET.invalid\"}",StarfishMediaAPIs::context);}
 auto b7=p.tick(5002000001LL);assert(b7.state==DirectState::Loading&&b7.callbacks==147&&b7.relay_requests==0&&b7.upstream_fetches==0);
 assert(p.tick(15001000001LL).error==DirectError::LoadTimeout);p.close();
 // Foreground's boolean alone is not interpreted as asynchronous readiness.
 foreground_result=false;assert(p.open("https://media.example/vod.m3u8",0,1));assert(p.tick(2).state==DirectState::Loading);p.close();foreground_result=true;
 for(auto contract:{DirectLoadContract::UriHls,DirectLoadContract::UriAuto}){
     assert(p.open("https://media.example/vod.m3u8",0,1,nullptr,false,contract));
     assert(StarfishMediaAPIs::payload.find("\"type\":\"media\"")!=std::string::npos&&StarfishMediaAPIs::payload.find("\"args\"")==std::string::npos);p.close();}
 // The production command path cannot bypass the relay. Explicit probe source
 // overrides reject credentials, non-fixture paths and non-allowlisted origins.
 setenv("UXAIR_DIRECT_LAN","192.168.1.89",1);
 assert(!p.open("https://media.example/vod.m3u8",0,1,nullptr,false,DirectLoadContract::UriAuto,true));assert(p.snapshot().error==DirectError::Payload);
 unsetenv("UXAIR_DIRECT_LAN");
 // No native callback: fail at the unchanged 15-second deadline, preserving counters.
 assert(p.open("https://media.example/vod.m3u8",0,1));
 StarfishMediaAPIs::callback(0xb,77,"https://SECRET.invalid/?token=PRIVATE",StarfishMediaAPIs::context);
 auto waiting=p.tick(6000000001LL);assert(waiting.state==DirectState::Loading&&waiting.callbacks==1&&waiting.last_callback==0xb);
 auto timeout=p.tick(15000000002LL);assert(timeout.error==DirectError::LoadTimeout&&timeout.stage==DirectStage::AwaitLoad);p.close();
 // Real protocol queue -> native adapter -> feedback, followed by a clean retry.
 DirectSession session;const std::string body="Content-Location: https://media.example/vod.m3u8\r\nStart-Position: 0\r\n";
 auto request=[&](const char *method,const char *path){return session.request("owner",method,path,"text/parameters",path==std::string("/play")?body:std::string());};
 assert(request("POST","/play").status==200);assert(session.take(c));assert(p.open(c.uri,c.position,1));
 auto cb=StarfishMediaAPIs::callback;auto ctx=StarfishMediaAPIs::context;
 std::thread callback([&]{cb(0x13,42,"https://SECRET.invalid/?token=PRIVATE",ctx);});
 for(int i=0;i<100;++i){session.publish(p.snapshot());request("GET","/playback-info");}callback.join();
 session.publish(p.tick(2));auto failed=request("GET","/playback-info");assert(failed.status==503);
 assert(failed.body.find("native-error")!=std::string::npos&&failed.body.find("42")!=std::string::npos&&failed.body.find("PRIVATE")==std::string::npos);p.close();
 assert(request("POST","/stop").status==200);assert(session.take(c));
 assert(request("POST","/play").status==200);assert(session.take(c));StarfishMediaAPIs::synchronous_load=true;
 assert(p.open(c.uri,0,1));session.publish(p.tick(2));assert(request("GET","/playback-info").status==200&&session.snapshot().state==DirectState::Playing);p.close();StarfishMediaAPIs::synchronous_load=false;
 acb_initialize_success=false;assert(!p.open("https://media.example/vod.m3u8",0,1));assert(p.snapshot().error==DirectError::AcbInitialize);acb_initialize_success=true;
 assert(p.open("https://media.example/vod.m3u8",.25,1));cb(0x16,0,nullptr,StarfishMediaAPIs::context);const auto before=acb_sink_calls;
 p.tick(2);p.tick(3);assert(acb_sink_calls==before+1);assert(p.tick(15000000002LL).error==DirectError::DurationTimeout);p.close();
 assert(p.open("https://media.example/vod.m3u8",0,1));acb_sink_success=false;cb(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).error==DirectError::Sink);p.close();acb_sink_success=true;
 assert(p.open("https://media.example/vod.m3u8",0,1));StarfishMediaAPIs::play_success=false;cb(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).error==DirectError::PlayRejected);p.close();StarfishMediaAPIs::play_success=true;
 assert(!p.open("file:///etc/passwd",0,1));
 puts("PASS: native adapter load/play/pause/resume/seek/EOS/timeout with simulated firmware");
}
