#include "direct_video.h"
#include <plist/plist.h>
#include <cassert>
#include <thread>
#include <cstdio>
#include <cstdlib>
static std::string body="Content-Location: https://media.example/vod/master.m3u8?token=PRIVATE\r\nStart-Position: 0\r\n";
int main(){
    for(const char *uri:{"file:///etc/passwd","http://127.0.0.1/a","http://127.1/a","http://169.254.1.1/a","http://user:pass@example.com/a","http://[::1]/a","http://0/a","http://example.com/\nsecret","http:///a","http://example.com:0/a"})assert(!direct_uri_valid(uri));
    assert(direct_uri_valid("http://192.168.1.50/video.m3u8"));
    assert(direct_uri_valid("https://example.com/video.m3u8?token=x"));
    assert(direct_load_payload("https://example.com/x","com.github.depius.uxair").empty());
    assert(direct_load_payload("https://example.com/x","com.github.depius.uxair.dev").find("BUFFERSTREAM")==std::string::npos);
    body="Content-Location: https://media.example/master.m3u8?token=PRIVATE\r\nStart-Position: 0\r\n";
    DirectSession session;
    auto send=[&](const char *method,const char *url,const std::string& data=""){return session.request("session-A",method,url,"text/parameters",data).status;};
    assert(send("POST","/play",body)==200);DirectCommand c;assert(session.take(c)&&c.kind==DirectCommandKind::Play);
    assert(send("POST","/play",body)==409);
    assert(send("POST","/rate?value=nan")==400);assert(send("POST","/scrub?position=-1")==400);
    assert(send("POST","/scrub?position=inf")==400);assert(send("POST","/scrub?position=7")==200);
    assert(session.take(c)&&c.position==7);
    assert(session.request("session-B","POST","/stop","","",false).status==409);
    DirectSnapshot playing;playing.state=DirectState::Playing;playing.position=12;playing.duration=120;session.publish(playing);
    assert(send("GET","/playback-info")==200);assert(send("POST","/rate?value=0")==200);assert(session.take(c)&&c.kind==DirectCommandKind::Pause);
    assert(send("POST","/rate?value=1")==200);assert(session.take(c)&&c.kind==DirectCommandKind::Resume);
    assert(send("POST","/reverse")==501);assert(send("POST","/action")==501);
    assert(send("POST","/stop")==200);assert(session.take(c)&&c.kind==DirectCommandKind::Stop);session.reset();
    assert(send("POST","/play","Content-Location: file:///bad\r\n")==400);
    assert(send("POST","/play",std::string(16385,'a'))==400);
    assert(session.request("session-A","POST","/play","text/parameters",body,true).status==409);
    // Exercise actual libplist binary/XML readers, not a parallel parser implementation.
    for(bool binary:{false,true}){
        plist_t dict=plist_new_dict();plist_dict_set_item(dict,"Content-Location",plist_new_string("https://example.com/vod.m3u8"));
        plist_dict_set_item(dict,"Start-Position",plist_new_real(.25));char *data=nullptr;uint32_t len=0;
        if(binary)plist_to_bin(dict,&data,&len);else plist_to_xml(dict,&data,&len);
        assert(session.request("session-A","POST","/play",binary?"application/x-apple-binary-plist":"text/x-apple-plist+xml",std::string(data,len)).status==200);
        assert(session.take(c)&&c.position==.25);free(data);plist_free(dict);session.reset();
    }
    assert(session.request("session-A","POST","/play","application/x-apple-binary-plist","bplistBAD").status==400);
    // Callback/main-thread publication and cancellation have finite CI timeouts.
    std::thread callback([&]{for(int i=0;i<1000;++i)send("GET","/playback-info");});
    for(int i=0;i<1000;++i){session.publish(playing);session.snapshot();}callback.join();
    for(int i=0;i<20;++i){session.reset();assert(send("POST","/play",body)==200);assert(session.take(c));assert(send("POST","/stop")==200);session.reset();}
    puts("PASS: direct URL/plist parsing, session ownership, controls, bounded queue and concurrent snapshots");
}
