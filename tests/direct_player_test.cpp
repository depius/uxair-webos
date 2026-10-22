#include "direct_player.h"
#include <StarfishMediaAPIs.h>
#include <cassert>
#include <cstdio>
void (*StarfishMediaAPIs::callback)(int,int64_t,const char*,void*)=nullptr;
void *StarfishMediaAPIs::context=nullptr;
std::string StarfishMediaAPIs::payload,StarfishMediaAPIs::last_seek;
int StarfishMediaAPIs::unloads=0;
bool StarfishMediaAPIs::load_success=true;
extern "C" int AcbAPI_setMediaVideoData(long,const char*,long*){return 1;}
int main(){
 DirectPlayer p;assert(p.open("https://media.example/vod.m3u8",0,1));
 assert(StarfishMediaAPIs::payload.find("HLS")!=std::string::npos);
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);
 assert(p.tick(2).state==DirectState::Playing);
 DirectCommand c;c.kind=DirectCommandKind::Pause;assert(p.control(c));assert(p.tick(3).state==DirectState::Paused);
 c.kind=DirectCommandKind::Resume;assert(p.control(c));c.kind=DirectCommandKind::Seek;c.position=1;assert(p.control(c));
 StarfishMediaAPIs::callback(0x1c,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(4).state==DirectState::Ended);p.close();
 assert(StarfishMediaAPIs::unloads==1);auto stale_context=StarfishMediaAPIs::context;auto stale_callback=StarfishMediaAPIs::callback;
 assert(p.open("https://media.example/vod.m3u8",0,1));stale_callback(0x1c,0,nullptr,stale_context);assert(p.tick(2).state==DirectState::Loading);assert(p.tick(16000000002LL).state==DirectState::Error);p.close();
 StarfishMediaAPIs::load_success=false;assert(!p.open("https://media.example/vod.m3u8",0,1));assert(p.tick(2).state==DirectState::Idle);
 StarfishMediaAPIs::load_success=true;assert(p.open("https://media.example/vod.m3u8",0,1));
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).state==DirectState::Playing);p.close();
 assert(p.open("https://media.example/vod.m3u8",6.5,1,nullptr,true));
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).state==DirectState::Playing);
 assert(StarfishMediaAPIs::last_seek=="6500");p.close();
 assert(p.open("https://media.example/vod.m3u8",.25,1));
 StarfishMediaAPIs::callback(0x16,0,nullptr,StarfishMediaAPIs::context);assert(p.tick(2).state==DirectState::Loading);
 StarfishMediaAPIs::callback(3,12000,nullptr,StarfishMediaAPIs::context);assert(p.tick(3).state==DirectState::Playing);
 assert(StarfishMediaAPIs::last_seek=="3000");p.close();
 assert(!p.open("file:///etc/passwd",0,1));
 puts("PASS: native adapter load/play/pause/resume/seek/EOS/timeout with simulated firmware");
}
