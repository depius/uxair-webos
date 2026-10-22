#include "direct_native_diag.h"
#include "direct_video.h"
#include <plist/plist.h>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
int main(){
 assert(!strcmp(direct_native_event_name(0xd),"program-count"));assert(!strcmp(direct_native_event_name(2),"buffer-range-info"));assert(!strcmp(direct_native_event_name(1),"streaming-info-periodic"));assert(!strcmp(direct_native_event_name(0x13),"str-error"));
 assert(direct_native_json_summary(nullptr)=="absent");assert(direct_native_json_summary("not JSON PRIVATE")=="non-json");
 assert(direct_native_json_summary(std::string(8193,'x').c_str())=="oversize");
 assert(direct_native_json_summary((std::string(17,'[')+std::string(17,']')).c_str())=="depth-limit");
 auto error=direct_native_json_summary("{\"errorCode\":-12,\"errorText\":\"https://SECRET.invalid/?token=PRIVATE\",\"PRIVATE\":\"SECRET\",\"uri\":\"https://SECRET\"}");
 assert(error.find("root.errorCode:integer=-12")!=std::string::npos&&error.find("SECRET")==std::string::npos&&error.find("PRIVATE")==std::string::npos&&error.find("https://")==std::string::npos);
 for(auto c:{DirectLoadContract::ArgsHls,DirectLoadContract::UriHls,DirectLoadContract::UriAuto}){
     auto text=direct_load_payload("http://127.0.0.1:1234/SECRET/0.m3u8","com.github.depius.uxair.dev",true,c);
     plist_t root=nullptr;plist_from_json(text.data(),uint32_t(text.size()),&root);assert(root);
     auto arg=c==DirectLoadContract::ArgsHls?plist_array_get_item(plist_dict_get_item(root,"args"),0):plist_dict_get_item(root,"payload");
     assert(arg&&plist_dict_get_item(arg,"option"));assert(plist_get_node_type(plist_dict_get_item(c==DirectLoadContract::ArgsHls?arg:root,"uri"))==PLIST_STRING);
     assert(plist_get_node_type(plist_dict_get_item(arg,"mediaTransportType"))==PLIST_STRING);
     auto shape=direct_native_json_summary(text.c_str());assert(shape.find("SECRET")==std::string::npos&&shape.find("127.0.0.1")==std::string::npos&&shape.find("uri:string")!=std::string::npos);plist_free(root);
 }
 for(const char *url:{"http://192.168.1.89:8080/master.m3u8","http://192.168.1.89:8080/probe.mp4"})assert(direct_fixture_uri_valid(url,"192.168.1.89"));
 for(const char *url:{"http://192.168.1.90:8080/master.m3u8","http://user:pass@192.168.1.89:8080/master.m3u8","http://192.168.1.89:8080/master.m3u8?secret=1","https://192.168.1.89:8080/master.m3u8","http://192.168.1.89:80/master.m3u8","http://192.168.1.89:8080/other"})assert(!direct_fixture_uri_valid(url,"192.168.1.89"));
 assert(!direct_fixture_uri_valid("http://127.0.0.1:8080/master.m3u8","127.0.0.1"));
 setenv("UXAIR_DIRECT_CONTRACT","uri-envelope-hls",1);assert(direct_configured_contract()==DirectLoadContract::UriHls);
 setenv("UXAIR_DIRECT_CONTRACT","uri-envelope-auto",1);assert(direct_configured_contract()==DirectLoadContract::UriAuto);
 setenv("UXAIR_DIRECT_CONTRACT","PRIVATE",1);assert(direct_configured_contract()==DirectLoadContract::ArgsHls);unsetenv("UXAIR_DIRECT_CONTRACT");
 puts("PASS: named B7 events, bounded credential-free native JSON shapes, URI contract alternatives and strict fixture-only bypass");
}
