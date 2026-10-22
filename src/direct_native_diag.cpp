#include "direct_native_diag.h"
#include "direct_video.h"
#include <plist/plist.h>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <arpa/inet.h>
const char *direct_contract_name(DirectLoadContract c){
    switch(c){case DirectLoadContract::UriHls:return "uri-envelope-hls";case DirectLoadContract::UriAuto:return "uri-envelope-auto";default:return "args-hls";}
}
DirectLoadContract direct_configured_contract(){
    const char *mode=getenv("UXAIR_DIRECT_CONTRACT");
    if(mode&&!strcmp(mode,"uri-envelope-hls"))return DirectLoadContract::UriHls;
    if(mode&&!strcmp(mode,"uri-envelope-auto"))return DirectLoadContract::UriAuto;
    return DirectLoadContract::ArgsHls;
}
const char *direct_native_event_name(int type){
    switch(type){case 0:return "frame-ready";case 1:return "streaming-info-periodic";case 2:return "buffer-range-info";
    case 3:return "duration";case 4:return "video-info";case 5:return "video-track-info";case 7:return "audio-info";
    case 8:return "audio-track-info";case 9:return "subtitle-track-info";case 10:return "buffer-event";case 11:return "source-info";
    case 13:return "program-count";case 14:return "video-track-count";case 15:return "audio-track-count";case 17:return "resource-info";
    case 18:return "int-error";case 19:return "str-error";case 21:return "preload-completed";case 22:return "load-completed";
    case 23:return "unload-completed";case 24:return "track-selected";case 25:return "seek-done";case 26:return "playing";
    case 27:return "paused";case 28:return "end-of-stream";case 29:return "custom";case 38:return "need-data";
    case 39:return "enough-data";case 43:return "vdec-ready";case 44:return "buffer-low-int";case 45:return "buffer-full";
    case 46:return "buffer-low";case 48:return "dropped-frame";default:return "unknown";}
}
static const char *node_type(plist_t n){
    if(!n)return "absent";switch(plist_get_node_type(n)){case PLIST_DICT:return "object";case PLIST_ARRAY:return "array";
    case PLIST_STRING:return "string";case PLIST_UINT:return "integer";case PLIST_REAL:return "real";case PLIST_BOOLEAN:return "boolean";default:return "other";}
}
static void describe(plist_t n,const std::string& prefix,std::ostringstream& out,unsigned depth,unsigned& count){
    if(!n||depth>4||count++>=48)return;
    out<<prefix<<":"<<node_type(n);
    if(plist_get_node_type(n)==PLIST_DICT)out<<"("<<plist_dict_get_size(n)<<")";
    if(plist_get_node_type(n)==PLIST_ARRAY)out<<"("<<plist_array_get_size(n)<<")";
    // Only errorCode may carry a scalar value. Unknown field names and all string
    // contents, including errorText, URI, mediaId, tokens and appId, are never copied.
    if(prefix=="root.errorCode"||prefix=="root.error.code"){
        if(plist_get_node_type(n)==PLIST_UINT){uint64_t v=0;plist_get_uint_val(n,&v);out<<"="<<static_cast<int64_t>(v);}
    }
    out<<" ";
    if(plist_get_node_type(n)==PLIST_DICT){
        static const char *keys[]={"args","uri","type","payload","mediaTransportType","option","appId","mediaId","windowId","mediaFormat","queryPosition","adaptiveStreaming","maxWidth","maxHeight","errorCode","errorText","error","code","message","state","buffering","bufferRange","buffered","position","duration","sourceInfo","videoInfo","audioInfo","streamingInfo","totalSize","downloadedSize","bufferSize","start","end"};
        for(const char *key:keys){auto child=plist_dict_get_item(n,key);if(child)describe(child,prefix+"."+key,out,depth+1,count);}
    }else if(plist_get_node_type(n)==PLIST_ARRAY&&plist_array_get_size(n)>0)describe(plist_array_get_item(n,0),prefix+"[0]",out,depth+1,count);
}
std::string direct_native_json_summary(const char *message){
    if(!message)return "absent";const auto size=strnlen(message,8193);if(size>8192)return "oversize";
    // Bound nesting before handing JSON to libplist. Quoted braces do not count.
    unsigned depth=0;bool quoted=false,escaped=false;
    for(size_t i=0;i<size;++i){const char c=message[i];if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;else if(c=='{'||c=='['){if(++depth>16)return "depth-limit";}else if(c=='}'||c==']'){if(!depth)return "invalid-json";--depth;}}
    if(depth||quoted)return "invalid-json";
    plist_t root=nullptr;plist_from_json(message,uint32_t(size),&root);if(!root)return "non-json";
    std::ostringstream out;unsigned count=0;describe(root,"root",out,0,count);plist_free(root);return out.str();
}
bool direct_fixture_uri_valid(const std::string& uri,const std::string& lan){
    in_addr address={};if(inet_pton(AF_INET,lan.c_str(),&address)!=1)return false;
    const uint32_t n=ntohl(address.s_addr);const unsigned a=n>>24,b=(n>>16)&255;
    if(!(a==10||(a==172&&b>=16&&b<=31)||(a==192&&b==168)))return false;
    const auto prefix="http://"+lan+":8080/";
    return direct_uri_valid(uri)&&(uri==prefix+"master.m3u8"||uri==prefix+"probe.mp4");
}
