// Explicit development-only native diagnostic. No RAOP, settings, pairing or History.
#include "direct_player.h"
#include "shell.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
static bool launcher_authorized(){
    const char *flag=getenv("UXAIR_NATIVE_PROBE_AUTHORIZED");
    return flag&&!strcmp(flag,"fixture-only");
}
int main(int argc,char **argv){
    umask(0077);
    if(argc!=2||!launcher_authorized()){fprintf(stderr,"[native-probe] use the dev-only launcher with all receivers closed\n");return 2;}
    const std::string lan=argv[1],base="http://"+lan+":8080/";
    if(!direct_fixture_uri_valid(base+"master.m3u8",lan)){fprintf(stderr,"[native-probe] invalid fixture IPv4\n");return 2;}
    setenv("UXAIR_DIRECT_LAN",lan.c_str(),1);unsetenv("UXAIR_DIRECT_DISCOVERY");
    // Keep proprietary stdout (which can contain media URIs) out of the diagnostic log.
    if(!freopen("/dev/null","w",stdout))return 2;
    setvbuf(stderr,nullptr,_IOLBF,0);
    Shell shell;if(!shell.open())return 3;
    struct Case {const char *name;DirectLoadContract contract;bool direct;const char *file;};
    const Case cases[]={
        {"A-args-hls-loopback",DirectLoadContract::ArgsHls,false,"master.m3u8"},
        {"B-uri-hls-loopback",DirectLoadContract::UriHls,false,"master.m3u8"},
        {"C-uri-auto-loopback",DirectLoadContract::UriAuto,false,"master.m3u8"},
        {"D-uri-hls-fixture-lan",DirectLoadContract::UriHls,true,"master.m3u8"},
        {"E-uri-auto-fixture-lan",DirectLoadContract::UriAuto,true,"master.m3u8"},
        {"F-uri-auto-mp4-fixture-lan",DirectLoadContract::UriAuto,true,"probe.mp4"}};
    for(const auto& c:cases){
        if(!shell.pump())return 130;
        fprintf(stderr,"[native-probe] case=%s begin contract=%s source=%s\n",c.name,direct_contract_name(c.contract),c.direct?"fixture-lan":"loopback-relay");
        shell.screen(std::string("Native URI probe ")+c.name,"",0);
        DirectPlayer player;const auto began=now_ns();bool running=player.open(base+c.file,0,began,nullptr,false,c.contract,c.direct),shown=false;
        DirectSnapshot status=player.snapshot();int64_t playing_since=0;
        while(running&&now_ns()-began<16000000000LL){
            if(!shell.pump()){player.close();return 130;}
            const auto now=now_ns();status=player.tick(now);
            if(status.state==DirectState::Error||status.state==DirectState::Ended)break;
            if(status.state==DirectState::Playing){
                if(!playing_since)playing_since=now;
                if(!shown){shell.transparent();shown=true;}
                if(now-playing_since>=6000000000LL)break;
            }
            SDL_Delay(10);
        }
        fprintf(stderr,"[native-probe] case=%s end state=%s stage=%s error=%s callbacks=%llu relay_requests=%llu fetches=%llu bytes=%llu failures=%llu\n",
            c.name,direct_state_name(status.state),direct_stage_name(status.stage),direct_error_name(status.error),
            (unsigned long long)status.callbacks,(unsigned long long)status.relay_requests,(unsigned long long)status.upstream_fetches,
            (unsigned long long)status.downloaded_bytes,(unsigned long long)status.relay_failures);
        player.close();
        if(status.state==DirectState::Playing&&(c.direct||(status.relay_requests>0&&status.upstream_fetches>0&&status.downloaded_bytes>0))){
            fprintf(stderr,"[native-probe] candidate=%s contract=%s source=%s; report visible-video and audible-tone separately; callbacks do not prove either\n",
                c.name,direct_contract_name(c.contract),c.direct?"fixture-lan":"loopback-relay");return 0;
        }
    }
    fprintf(stderr,"[native-probe] no-playing-candidate; URI API/HLS support not established; do not infer loopback restriction from zero requests alone\n");return 1;
}
