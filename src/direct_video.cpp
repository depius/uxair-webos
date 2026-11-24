#include "direct_video.h"
#include "identity.h"
#include "direct_controls.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <utility>
#include <arpa/inet.h>
const char *direct_stage_name(DirectStage s) {
    static const char *names[]={"idle","queued","dequeued","relay","acb-create","acb-initialize","construct","media-id","load","await-load","sink","loaded-state","window","seek","play","await-playing","playing","paused","video-info","stopped","fetch","bufferstream-load","packet-feed"};
    const unsigned n=unsigned(s);return n<sizeof(names)/sizeof(*names)?names[n]:"unknown";
}
const char *direct_error_name(DirectError e) {
    static const char *names[]={"none","busy","relay-start","payload","acb-create","acb-initialize","construct","media-id","load-rejected","native-error","sink","seek","play-rejected","load-timeout","duration-timeout","playing-timeout","progress-timeout","control-rejected","no-native-progress","cleanup-failed","fetch-failed","unsupported-stream",
        "invalid-stream","stream-limit","discontinuity","bufferstream-start","bufferstream-feed","native-api-unavailable"};
    const unsigned n=unsigned(e);return n<sizeof(names)/sizeof(*names)?names[n]:"unknown";
}
const char *direct_backend_name(DirectBackend backend) {
    switch (backend) {
    case DirectBackend::NativeUri: return "Native URI";
    case DirectBackend::Bufferstream: return "BUFFERSTREAM";
    case DirectBackend::None: return "Idle";
    }
    return "Unknown";
}
static std::string diagnostic_plist(const DirectSnapshot& s) {
    std::ostringstream out;
    out<<"<key>uxairStage</key><string>"<<direct_stage_name(s.stage)<<"</string><key>uxairError</key><string>"<<direct_error_name(s.error)<<"</string>";
    out<<"<key>uxairNativeResult</key><integer>"<<s.native_result<<"</integer><key>uxairCallbackCount</key><integer>"<<s.callbacks<<"</integer>";
    out<<"<key>uxairLastCallback</key><integer>"<<s.last_callback<<"</integer><key>uxairLastCallbackValue</key><integer>"<<s.last_callback_value<<"</integer>";
    out<<"<key>uxairRelayRequests</key><integer>"<<s.relay_requests<<"</integer><key>uxairUpstreamFetches</key><integer>"<<s.upstream_fetches<<"</integer>";
    out<<"<key>uxairDownloadedBytes</key><integer>"<<s.downloaded_bytes<<"</integer><key>uxairRelayFailures</key><integer>"<<s.relay_failures<<"</integer>";
    out<<"<key>uxairBackend</key><string>"<<direct_backend_name(s.backend)<<"</string>"
       <<"<key>uxairFallbackReason</key><string>"<<direct_error_name(s.fallback_reason)<<"</string>"
       <<"<key>uxairMediaGets</key><integer>"<<s.media_gets<<"</integer>"
       <<"<key>uxairBufferBytes</key><integer>"<<s.buffer_bytes<<"</integer>";
    return out.str();
}
static bool number(const std::string& text,double& value) {
    if(text.empty()||text.size()>32)return false;char *end=nullptr;value=strtod(text.c_str(),&end);
    return end==text.c_str()+text.size()&&std::isfinite(value)&&value>=0&&value<=31536000;
}
bool direct_uri_valid(const std::string& uri) {
    if(uri.size()>4096)return false;
    size_t begin=uri.compare(0,7,"http://")==0?7:uri.compare(0,8,"https://")==0?8:0;
    if(!begin)return false;
    for(unsigned char c:uri)if(c<=32||c>=127||c=='\\'||c=='"'||c=='#')return false;
    const size_t end=uri.find_first_of("/?",begin);const std::string host=uri.substr(begin,end-begin);
    if(host.empty()||host.find('@')!=std::string::npos||host[0]=='['||host.find('%')!=std::string::npos)return false;
    std::string name=host.substr(0,host.find(':'));
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(name.empty()||name=="localhost"||name.back()=='.'||name.find("localhost.")==0||name=="0.0.0.0"||name.compare(0,4,"127.")==0||name.compare(0,8,"169.254.")==0)return false;
    bool numeric=true;for(char c:name)if(!std::isdigit(static_cast<unsigned char>(c))&&c!='.')numeric=false;
    if(numeric){unsigned a,b,c,d;char extra;if(sscanf(name.c_str(),"%u.%u.%u.%u%c",&a,&b,&c,&d,&extra)!=4||a>223||a==0||a==127||b>255||c>255||d>255)return false;}
    else for(char c:name)if(!std::isalnum(static_cast<unsigned char>(c))&&c!='-'&&c!='.')return false;
    const size_t colon=host.find(':');if(colon!=std::string::npos){double port;if(!number(host.substr(colon+1),port)||port<1||port>65535||std::floor(port)!=port)return false;}
    return true;
}

bool direct_public_ipv4(uint32_t ip) {
    const unsigned a=ip>>24,b=(ip>>16)&255;
    const auto prefix=ip>>8;
    return a && a!=127 && a<224 && !(a==169&&b==254) && !(a==198&&(b==18||b==19)) &&
        prefix!=0xc00000 && prefix!=0xc00002 && prefix!=0xc05863 && prefix!=0xc63364 && prefix!=0xcb0071 &&
        a!=10 && !(a==172&&b>=16&&b<=31) && !(a==192&&b==168) && !(a==100&&b>=64&&b<=127);
}

static bool public_media_uri(const std::string& uri) {
    if (!direct_uri_valid(uri)) return false;
    const size_t start=uri.find("://")+3;
    const auto host=uri.substr(start,uri.find_first_of(":/?",start)-start);
    in_addr literal={};
    return inet_pton(AF_INET,host.c_str(),&literal)!=1 || direct_public_ipv4(ntohl(literal.s_addr));
}
static std::string escape(const std::string& s){std::string out;for(char c:s){if(c=='"'||c=='\\')out+='\\';out+=c;}return out;}
std::string direct_load_payload(const std::string& uri,const std::string& app,bool internal,DirectLoadContract contract) {
    if((!direct_uri_valid(uri)&&!(internal&&uri.compare(0,17,"http://127.0.0.1:")==0&&uri.find_first_of("\r\n\"\\")==std::string::npos))||app!=UXAIR_DEV_APP_ID)return {};
    const std::string option="\"option\":{\"appId\":\""+app+"\",\"mediaFormat\":{\"type\":\"video\"},\"adaptiveStreaming\":{\"maxWidth\":1920,\"maxHeight\":1080},\"queryPosition\":true}";
    if(contract==DirectLoadContract::ArgsHls)return "{\"args\":[{\"uri\":\""+escape(uri)+"\",\"mediaTransportType\":\"HLS\","+option+"}]}";
    // The URI envelope is from uMediaClient::loadAsync, not the SMP BUFFERSTREAM
    // schema. Expose it only as an explicit experimental choice until B7 evidence.
    return "{\"uri\":\""+escape(uri)+"\",\"type\":\"media\",\"payload\":{\"mediaTransportType\":\""+
        (contract==DirectLoadContract::UriHls?"HLS":"URI")+"\","+option+"}}";
}
static bool play_body(const std::string& mime,const std::string& body,DirectCommand& cmd,
                      const std::string& owner,std::string& uuid) {
    auto& uri=cmd.uri;
    auto& start=cmd.position;
    auto& seconds=cmd.position_seconds;
    std::string type=mime.substr(0,mime.find(';'));
    while(!type.empty() && (type.back()==' ' || type.back()=='\t'))type.pop_back();
    std::transform(type.begin(),type.end(),type.begin(),[](unsigned char c){return char(std::tolower(c));});
    const bool binary_signature=body.compare(0,8,"bplist00")==0;
    if(binary_signature && (type.empty() || type=="application/octet-stream"))type="application/x-apple-binary-plist";
    start=0;seconds=false;if(body.empty()||body.size()>16384)return false;
    if(type=="application/x-apple-binary-plist"||type=="text/x-apple-plist+xml"||type=="application/x-apple-plist+xml") {
        plist_t root=nullptr;
        if(type=="application/x-apple-binary-plist")plist_from_bin(body.data(),uint32_t(body.size()),&root);
        else {if(body.find("<!ENTITY")!=std::string::npos)return false;plist_from_xml(body.data(),uint32_t(body.size()),&root);}
        if(!root||plist_get_node_type(root)!=PLIST_DICT){if(root)plist_free(root);return false;}
        plist_t location=plist_dict_get_item(root,"Content-Location"),position=plist_dict_get_item(root,"Start-Position");
        plist_t seconds_node=plist_dict_get_item(root,"Start-Position-Seconds");
        bool valid=location&&plist_get_node_type(location)==PLIST_STRING;
        if(valid){
            uint64_t length=0;
            const char *value=plist_get_string_ptr(location,&length);
            if(value && length<=4096)uri.assign(value,size_t(length));
            else valid=false;
        }
        if(position){if(plist_get_node_type(position)==PLIST_REAL)plist_get_real_val(position,&start);
            else if(plist_get_node_type(position)==PLIST_UINT){uint64_t v=0;plist_get_uint_val(position,&v);start=double(v);}else valid=false;}
        valid=valid&&std::isfinite(start)&&start>=0&&start<=1;
        if(seconds_node){seconds=true;
            if(plist_get_node_type(seconds_node)==PLIST_REAL)plist_get_real_val(seconds_node,&start);
            else if(plist_get_node_type(seconds_node)==PLIST_UINT){uint64_t n=0;plist_get_uint_val(seconds_node,&n);start=double(n);}else valid=false;}
        if (cmd.encrypted) {
            plist_t session=plist_dict_get_item(root,"X-Apple-Session-ID");
            plist_t item=plist_dict_get_item(root,"uuid");
            plist_t media=plist_dict_get_item(root,"mediaType");
            plist_t stream=plist_dict_get_item(root,"streamType");
            plist_t rate=plist_dict_get_item(root,"rate");
            uint64_t len=0;
            if (session) {
                const char *s=plist_get_string_ptr(session,&len);
                valid=valid&&s&&len==owner.size()&&!memcmp(s,owner.data(),owner.size());
            }
            if (item) {
                const char *s=plist_get_string_ptr(item,&len);
                valid=valid&&s&&len&&len<=128&&!memchr(s,0,len);
                if(valid) uuid.assign(s,size_t(len));
            }
            if (media) {
                const char *s=plist_get_string_ptr(media,&len);
                valid=valid&&s&&len&&len<=64;
            }
            if (stream) valid=valid&&PLIST_IS_UINT(stream);
            if (rate) {
                double value=-1;
                if (PLIST_IS_REAL(rate)) plist_get_real_val(rate,&value);
                else if (PLIST_IS_UINT(rate)) {
                    uint64_t integer=2;
                    plist_get_uint_val(rate,&integer);
                    value=double(integer);
                }
                valid=valid&&(value==0||value==1);
                cmd.start_paused=value==0;
            }
        }
        plist_free(root);return valid&&std::isfinite(start)&&start>=0&&start<=(seconds?31536000:1);
    }
    if(type!="text/parameters")return false;
    std::istringstream in(body);std::string line;bool found=false,position=false;double absolute=0;
    while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();const size_t sep=line.find(": ");
        if(line.empty())continue;if(sep==std::string::npos)return false;
        const std::string key=line.substr(0,sep),value=line.substr(sep+2);
        if(key=="Content-Location"){if(found)return false;uri=value;found=true;}
        else if(key=="Start-Position"){if(position||!number(value,start)||start>1)return false;position=true;}
        else if(key=="Start-Position-Seconds"){if(seconds||!number(value,absolute))return false;seconds=true;}
        else return false;
    }
    if(seconds)start=absolute;return found;
}
static DirectReply set_property(const char *path, const std::string& body) {
    DirectReply reply;
    const char *query = strchr(path, '?');
    if (!query || !query[1]) {
        reply.reason = "property-name-required";
        return reply;
    }
    const char *name = uxair_known_property(query + 1);
    if (!name) {
        reply.status = 501;
        reply.reason = "unsupported-property";
        return reply;
    }
    plist_t root = uxair_control_plist(body.data(), body.size());
    plist_t value = PLIST_IS_DICT(root) ? plist_dict_get_item(root, "value") : nullptr;
    if (!value) {
        plist_free(root);
        reply.reason = "invalid-property-plist";
        return reply;
    }

    // These assignments select behaviors the single-item player actually has:
    // no time limits, no subsequent item, and no date-range notifications.
    bool supported = false;
    if (!strcmp(name, "forwardEndTime") || !strcmp(name, "reverseEndTime")) {
        const char *const fields[] = {"value", "timescale", "epoch", "flags"};
        supported = PLIST_IS_DICT(value) && plist_dict_get_size(value) == 4;
        for (const char *field : fields) {
            if (!supported) break;
            plist_t node = plist_dict_get_item(value, field);
            uint64_t integer = 1;
            if (PLIST_IS_UINT(node)) plist_get_uint_val(node, &integer);
            supported = integer == 0;
        }
    } else if (!strcmp(name, "actionAtItemEnd") && PLIST_IS_UINT(value)) {
        uint64_t action = 1;
        plist_get_uint_val(value, &action);
        supported = action == 0;
    } else if (!strcmp(name, "isInterestedInDateRange") && PLIST_IS_BOOLEAN(value)) {
        uint8_t interested = 1;
        plist_get_bool_val(value, &interested);
        supported = !interested;
    }
    plist_free(root);
    if (!supported) {
        reply.status = 501;
        reply.reason = "unsupported-property-value";
        return reply;
    }

    plist_t result = plist_new_dict();
    if (result) plist_dict_set_item(result, "errorCode", plist_new_uint(0));
    char *response = nullptr;
    uint32_t size = 0;
    if (result) plist_to_bin(result, &response, &size);
    plist_free(result);
    if (!response || !size) {
        free(response);
        reply.status = 503;
        reply.reason = "property-response-unavailable";
        return reply;
    }
    reply.body.assign(response, size);
    free(response);
    reply.type = "application/x-apple-binary-plist";
    reply.status = 200;
    reply.reason = "property-default-applied";
    return reply;
}

DirectReply DirectSession::request(const std::string& owner,const std::string& method,const std::string& target,
        const std::string& type,const std::string& body,bool busy,const std::string& controller,uintptr_t token) {
    DirectReply reply;
    if(owner.empty()||owner.size()>128||owner.find_first_of("\r\n")!=std::string::npos||target.size()>256||body.size()>524288)return reply;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!owner_.empty()&&(owner_!=owner||controller_!=controller)){reply.status=409;reply.reason="session-conflict";return reply;}
    last_request_ms_=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    DirectCommand command;command.kind=DirectCommandKind::Stop;command.encrypted=!controller.empty();
    if (command.encrypted && (!token || controller.size()!=32)) return reply;
    if(method=="POST"&&target=="/action") {
        plist_t root=nullptr;
        if(type.find("binary-plist")!=std::string::npos)plist_from_bin(body.data(),uint32_t(body.size()),&root);
        else if(body.find("<!ENTITY")==std::string::npos)plist_from_xml(body.data(),uint32_t(body.size()),&root);
        if(!root||plist_get_node_type(root)!=PLIST_DICT){if(root)plist_free(root);return reply;}
        auto text=[](plist_t n){char *p=nullptr;if(n&&plist_get_node_type(n)==PLIST_STRING)plist_get_string_val(n,&p);std::string v=p?p:"";free(p);return v;};
        const auto kind=text(plist_dict_get_item(root,"type"));plist_t params=plist_dict_get_item(root,"params");
        if(!params||plist_get_node_type(params)!=PLIST_DICT){plist_free(root);return reply;}
        if(kind=="unhandledURLResponse") {
            uint64_t id=0,status=0;auto id_node=plist_dict_get_item(params,"FCUP_Response_RequestID"),status_node=plist_dict_get_item(params,"FCUP_Response_StatusCode"),data_node=plist_dict_get_item(params,"FCUP_Response_Data");
            if(id_node&&plist_get_node_type(id_node)==PLIST_UINT)plist_get_uint_val(id_node,&id);
            if(status_node&&plist_get_node_type(status_node)==PLIST_UINT)plist_get_uint_val(status_node,&status);
            const auto url=text(plist_dict_get_item(params,"FCUP_Response_URL"));
            if(!pending_id_||pending_done_||id!=pending_id_||url!=pending_url_){reply.status=409;plist_free(root);return reply;}
            char *data=nullptr;uint64_t len=0;if(data_node&&plist_get_node_type(data_node)==PLIST_DATA)plist_get_data_val(data_node,&data,&len);
            if(status==200&&data&&len<=262144&&len>=7&&std::string(data,7)=="#EXTM3U")pending_data_.assign(data,size_t(len));
            pending_done_=true;free(data);event_cv_.notify_all();reply.status=status==200&&!pending_data_.empty()?200:422;
            plist_free(root);return reply;
        }
        if(kind=="playlistRemove") {commands_.clear();command.kind=DirectCommandKind::Stop;}
        else {reply.status=501;plist_free(root);return reply;}
        plist_free(root);
    } else if(method=="PUT" && uxair_control_path(target.c_str(),"/setProperty")) {
        return set_property(target.c_str(),body);
    } else if((method=="GET"||method=="POST"||method=="PUT")&&(target.compare(0,13,"/getProperty?")==0||target.compare(0,13,"/setProperty?")==0)) {
        const auto key=target.substr(13);
        if((method=="GET"||method=="POST")&&target.compare(0,13,"/getProperty?")==0&&(key=="playbackPosition"||key=="duration"||key=="rate")) {
            std::ostringstream out;out<<"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>value</key><real>"<<(key=="duration"?snapshot_.duration:key=="rate"?(snapshot_.state==DirectState::Playing?1:0):snapshot_.position)<<"</real></dict></plist>";
            reply.body=out.str();reply.type="text/x-apple-plist+xml";reply.status=200;return reply;
        }
        // Do not acknowledge subtitle, rate-boundary or playlist mutation features we do not implement.
        reply.status=501;return reply;
    } else if(method=="POST"&&target=="/play") {
        if(busy||snapshot_.state==DirectState::Loading||snapshot_.state==DirectState::Playing||snapshot_.state==DirectState::Paused||snapshot_.state==DirectState::Buffering){reply.status=409;return reply;}
        std::string item;
        if(!play_body(type,body,command,owner,item)) {
            reply.descriptor="invalid-play";
            reply.reason="invalid-play-payload";
            return reply;
        }
        if (command.encrypted && command.uri.compare(0,8,"mlhls://")==0) {
            reply.status=415;reply.descriptor="mlhls";reply.reason="resource-bridge-required";return reply;
        }
        if (command.encrypted ? !public_media_uri(command.uri) : !direct_uri_valid(command.uri)) {
            reply.descriptor="invalid-url";reply.reason="media-url-policy";return reply;
        }
        const auto path=command.uri.substr(0,command.uri.find('?'));
        const bool manifest=path.size()>=5 && path.compare(path.size()-5,5,".m3u8")==0;
        if (command.encrypted && path.size()>=4 && path.compare(path.size()-4,4,".mp4")==0) {
            reply.status=415;reply.descriptor="progressive";reply.reason="unsupported-progressive-media";return reply;
        }
        reply.descriptor=command.uri.compare(0,8,"https://")==0 ?
            (manifest?"https-hls":"https-url") : (manifest?"http-hls":"http-url");
        if(!commands_.empty()){reply.status=503;return reply;}
        command.kind=DirectCommandKind::Play;owner_=owner;controller_=controller;media_uuid_=std::move(item);
        play_connection_=command.encrypted?token:0;
        backend_reported_=frame_reported_=audio_reported_=false;
        snapshot_=DirectSnapshot();snapshot_.state=DirectState::Loading;snapshot_.stage=DirectStage::Queued;
        event_locked(DirectState::Loading);
    } else if(method=="POST"&&target=="/stop") {
        if(command.encrypted&&!play_connection_){reply.status=409;reply.reason="no-video-session";return reply;}
        commands_.clear();
    }
    else if(method=="POST"&&target.compare(0,6,"/rate?")==0){
        double rate=0;
        if(target.compare(0,12,"/rate?value=")!=0||!number(target.substr(12),rate)||(rate!=0&&rate!=1))return reply;
        command.kind=rate==0?DirectCommandKind::Pause:DirectCommandKind::Resume;
    } else if(method=="POST"&&uxair_control_path(target.c_str(),"/scrub")) {
        if(command.encrypted&&!body.empty()) {
            plist_t root=uxair_control_plist(body.data(),body.size());
            plist_t item=PLIST_IS_DICT(root)?plist_dict_get_item(root,"uuid"):nullptr;
            uint64_t len=0;
            const char *s=item?plist_get_string_ptr(item,&len):nullptr;
            const bool valid=PLIST_IS_DICT(root)&&(!item||(s&&len&&len<=128&&
                (media_uuid_.empty()||(len==media_uuid_.size()&&!memcmp(s,media_uuid_.data(),size_t(len))))));
            plist_free(root);
            if(!valid){reply.status=409;reply.reason="media-item-conflict";return reply;}
        }
        if(target.compare(0,16,"/scrub?position=")!=0 || !number(target.substr(16),command.position)) {
            reply.reason="invalid-seek-position";
            return reply;
        }
        command.kind=DirectCommandKind::Seek;
        if(command.encrypted&&play_connection_&&!snapshot_.seekable){reply.status=409;reply.reason="seek-unavailable";return reply;}
    } else if(method=="GET"&&(target=="/scrub"||target=="/playback-info")) {
        if(command.encrypted&&target=="/scrub"&&!play_connection_){reply.status=409;reply.reason="no-video-session";return reply;}
        if(snapshot_.state==DirectState::Error){reply.status=503;reply.type="text/x-apple-plist+xml";
            reply.body="<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"+diagnostic_plist(snapshot_)+"</dict></plist>";return reply;}
        std::ostringstream out;
        if(target=="/scrub")out<<"duration: "<<snapshot_.duration<<"\nposition: "<<snapshot_.position<<"\n";
        else {reply.type="text/x-apple-plist+xml";out<<"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>duration</key><real>"<<snapshot_.duration<<"</real><key>position</key><real>"<<snapshot_.position<<"</real><key>rate</key><real>"<<(snapshot_.state==DirectState::Playing?1:0)<<"</real><key>readyToPlay</key><"<<(snapshot_.state==DirectState::Playing||snapshot_.state==DirectState::Paused?"true":"false")<<"/><key>playbackBufferEmpty</key><"<<((snapshot_.state==DirectState::Loading||snapshot_.state==DirectState::Buffering)?"true":"false")<<"/><key>playbackBufferFull</key><false/><key>playbackLikelyToKeepUp</key><"<<(snapshot_.state==DirectState::Playing?"true":"false")<<"/><key>loadedTimeRanges</key><array/><key>seekableTimeRanges</key><array>"<<((command.encrypted?snapshot_.seekable:snapshot_.duration>0)?"<dict><key>start</key><real>0</real><key>duration</key><real>"+std::to_string(snapshot_.duration)+"</real></dict>":"")<<"</array></dict></plist>";}
        reply.body=out.str();
        if(target=="/playback-info"){const auto end=reply.body.rfind("</dict></plist>");if(end!=std::string::npos)reply.body.insert(end,diagnostic_plist(snapshot_));}
        reply.status=200;return reply;
    } else {reply.status=501;return reply;}
    if(command.kind!=DirectCommandKind::Play&&command.kind!=DirectCommandKind::Stop&&(snapshot_.state==DirectState::Idle||snapshot_.state==DirectState::Error||snapshot_.state==DirectState::Ended)){
        reply.status=409;reply.reason="no-video-session";return reply;
    }
    if(commands_.size()>=8){reply.status=503;return reply;}
    commands_.push_back(command);reply.status=200;reply.queued=command.kind==DirectCommandKind::Play;return reply;
}
bool DirectSession::take(DirectCommand& command){std::lock_guard<std::mutex> l(mutex_);if(commands_.empty())return false;command=commands_.front();commands_.pop_front();return true;}
const char *direct_state_name(DirectState s){switch(s){case DirectState::Loading:case DirectState::Buffering:return "loading";case DirectState::Playing:return "playing";case DirectState::Paused:return "paused";case DirectState::Ended:case DirectState::Idle:return "stopped";default:return "stopped";}}
void DirectSession::event_locked(DirectState state) {
    if (!reverse_running_ && !encrypted_reverse_) return;
    std::string event="<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>category</key><string>video</string>";
    if(encrypted_reverse_) event+="<key>sessionID</key><integer>1</integer>";
    event+="<key>state</key><string>";
    event+=direct_state_name(state);
    event+="</string>";
    if(state==DirectState::Error) event+="<key>reason</key><string>error</string>"+diagnostic_plist(snapshot_);
    else if(state==DirectState::Ended&&snapshot_.natural_end) event+="<key>reason</key><string>ended</string>";
    event+="</dict></plist>";
    if(events_.size()>=16) events_.pop_front();
    events_.push_back(std::move(event));
    event_cv_.notify_all();
}
void DirectSession::publish(DirectSnapshot s){
    std::lock_guard<std::mutex> l(mutex_);
    if(!std::isfinite(s.position)||s.position<0)s.position=0;if(!std::isfinite(s.duration)||s.duration<0)s.duration=0;
    const auto prior=snapshot_.state;
    if(!controller_.empty()) {
        if(s.backend!=snapshot_.backend && s.backend!=DirectBackend::None) {
            if(snapshot_.backend!=DirectBackend::None&&!backend_reported_)++backend_failures_;
            if(s.fallback_reason!=DirectError::None&&snapshot_.backend==DirectBackend::None) {
                ++backend_attempts_;++backend_failures_;
            }
            ++backend_attempts_;backend_reported_=false;
        }
        if(!backend_reported_&&s.backend!=DirectBackend::None&&(s.backend_loaded||s.state==DirectState::Error)){
            if(s.backend_loaded)++backend_loads_;else ++backend_failures_;
            backend_reported_=true;
            fprintf(stderr,"[direct-session] backend=%s loaded=%d\n",direct_backend_name(s.backend),int(s.backend_loaded));
        }
        if(s.frame_observed&&!frame_reported_){++video_starts_;frame_reported_=true;fprintf(stderr,"[direct-session] first-frame=observed\n");}
        if(s.audio_fed&&!audio_reported_){++audio_starts_;audio_reported_=true;fprintf(stderr,"[direct-audio] first-feed=accepted\n");}
    }
    snapshot_=s;
    if(s.state!=prior&&(s.state!=DirectState::Buffering||prior!=DirectState::Loading)) event_locked(s.state);
    if(play_connection_&&s.state!=prior)
        fprintf(stderr,"[direct-session] state=%s stage=%s error=%s frame_observed=%d audio_feed_observed=%d\n",
                direct_state_name(s.state),direct_stage_name(s.stage),direct_error_name(s.error),
                int(s.frame_observed),int(s.audio_fed));
    if(s.state==DirectState::Idle||s.state==DirectState::Ended||s.state==DirectState::Error){
        play_connection_=0;media_uuid_.clear();
        if(!reverse_running_&&!encrypted_reverse_){owner_.clear();controller_.clear();}
        commands_.clear();
    }
}
DirectSnapshot DirectSession::snapshot(){std::lock_guard<std::mutex> l(mutex_);return snapshot_;}
void DirectSession::summarize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!backend_attempts_)return;
    fprintf(stderr,"[direct-session-summary] backend_load_attempts=%u backend_loaded=%u backend_load_failures=%u "
                "video_first_frames=%u audio_first_feeds=%u\n",
            backend_attempts_,backend_loads_,backend_failures_,video_starts_,audio_starts_);
    backend_attempts_=backend_loads_=backend_failures_=video_starts_=audio_starts_=0;
}
void DirectSession::close_reverse(){reverse_running_=false;event_cv_.notify_all();if(reverse_thread_.joinable())reverse_thread_.join();if(reverse_fd_>=0){close(reverse_fd_);reverse_fd_=-1;}}
DirectSession::~DirectSession(){close_reverse();}
bool DirectSession::bind_reverse(const std::string& owner,int fd,uintptr_t token){
    if(owner.empty()||owner.size()>128||owner.find_first_of("\r\n")!=std::string::npos)return false;
    {std::lock_guard<std::mutex> l(mutex_);if(reverse_running_||!controller_.empty()||(!owner_.empty()&&owner_!=owner))return false;}
    close_reverse();int copy=dup(fd);if(copy<0)return false;
    {std::lock_guard<std::mutex> l(mutex_);reverse_fd_=copy;reverse_token_=token;owner_=owner;reverse_running_=true;}
    try {
        reverse_thread_=std::thread(&DirectSession::reverse_loop,this);
    } catch (...) {
        connection_lost(token);
        close_reverse();
        return false;
    }
    return true;
}
bool DirectSession::bind_encrypted_reverse(const std::string& owner,const std::string& controller,uintptr_t token) {
    if(owner.empty()||owner.size()>128||owner.find_first_of("\r\n")!=std::string::npos||controller.size()!=32||!token) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if(reverse_running_||encrypted_reverse_||token==play_connection_||
       (!owner_.empty()&&(owner_!=owner||controller_!=controller))) return false;
    owner_=owner;
    controller_=controller;
    encrypted_reverse_=token;
    last_request_ms_=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if(play_connection_) event_locked(snapshot_.state);
    return true;
}

int DirectSession::take_encrypted_event(uintptr_t token,std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!token||token!=encrypted_reverse_) return -1;
    if(events_.empty()) {
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return !play_connection_&&(snapshot_.state==DirectState::Ended||snapshot_.state==DirectState::Error||
            now-last_request_ms_>=30000) ? -1 : 0;
    }
    body=std::move(events_.front());
    events_.pop_front();
    return 1;
}

void DirectSession::reverse_loop(){
    while(reverse_running_){std::string body,owner;
        {std::unique_lock<std::mutex> l(mutex_);event_cv_.wait_for(l,std::chrono::milliseconds(200),[&]{return !reverse_running_||!events_.empty();});if(!reverse_running_)break;if(events_.empty())continue;body=events_.front();events_.pop_front();owner=owner_;}
        std::string wire="POST /event HTTP/1.1\r\nContent-Type: text/x-apple-plist+xml\r\nX-Apple-Session-ID: "+owner+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
        size_t offset=0;while(offset<wire.size()&&reverse_running_){pollfd p={reverse_fd_,POLLOUT,0};if(poll(&p,1,250)<=0){reverse_running_=false;break;}auto n=send(reverse_fd_,wire.data()+offset,wire.size()-offset,MSG_DONTWAIT|MSG_NOSIGNAL);if(n<=0){reverse_running_=false;break;}offset+=size_t(n);}
        if(!reverse_running_)connection_lost(reverse_token_);
    }
}
void DirectSession::connection_lost(uintptr_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!token) return;
    if(token==encrypted_reverse_) {
        encrypted_reverse_=0;
        events_.clear();
        if(!play_connection_ && snapshot_.state!=DirectState::Loading &&
           snapshot_.state!=DirectState::Playing && snapshot_.state!=DirectState::Paused &&
           snapshot_.state!=DirectState::Buffering) {owner_.clear();controller_.clear();}
        return;
    }
    const bool legacy=token==reverse_token_;
    if(!legacy && token!=play_connection_) return;
    if(legacy) {
        reverse_running_=false;reverse_token_=0;events_.clear();
        pending_data_.clear();pending_done_=true;event_cv_.notify_all();
    }
    play_connection_=0;
    commands_.clear();
    DirectCommand stop;
    stop.kind=DirectCommandKind::Stop;
    commands_.push_back(stop);
}
std::string DirectSession::sender_manifest(const std::string& url){
    std::unique_lock<std::mutex> l(mutex_);if(!reverse_running_||pending_id_||!direct_uri_valid(url)||events_.size()>=16)return {};
    if(++request_id_==0)++request_id_;
    pending_id_=request_id_;pending_url_=url;pending_done_=false;pending_data_.clear();
    plist_t root=plist_new_dict(),request=plist_new_dict(),headers=plist_new_dict();
    plist_dict_set_item(root,"sessionID",plist_new_uint(1));plist_dict_set_item(root,"type",plist_new_string("unhandledURLRequest"));
    plist_dict_set_item(request,"FCUP_Response_RequestID",plist_new_uint(pending_id_));plist_dict_set_item(request,"FCUP_Response_URL",plist_new_string(url.c_str()));
    plist_dict_set_item(request,"FCUP_Response_ClientInfo",plist_new_uint(1));plist_dict_set_item(request,"FCUP_Response_ClientRef",plist_new_uint(40030004));
    plist_dict_set_item(request,"sessionID",plist_new_uint(1));plist_dict_set_item(headers,"X-Playback-Session-Id",plist_new_string(owner_.c_str()));plist_dict_set_item(request,"FCUP_Response_Headers",headers);plist_dict_set_item(root,"request",request);
    char *xml=nullptr;uint32_t len=0;plist_to_xml(root,&xml,&len);plist_free(root);if(xml){events_.push_back(std::string(xml,len));free(xml);}event_cv_.notify_all();
    event_cv_.wait_for(l,std::chrono::seconds(3),[&]{return pending_done_||!reverse_running_;});const auto result=pending_data_;pending_data_.clear();pending_id_=0;pending_url_.clear();return result;
}
void DirectSession::reset(bool keep_reverse){if(!keep_reverse)close_reverse();std::lock_guard<std::mutex> l(mutex_);commands_.clear();if(!keep_reverse){owner_.clear();controller_.clear();events_.clear();reverse_token_=encrypted_reverse_=0;}play_connection_=0;media_uuid_.clear();snapshot_=DirectSnapshot();pending_done_=true;event_cv_.notify_all();}

bool DirectSession::check_timeout(int64_t now_ms){std::lock_guard<std::mutex> lock(mutex_);if(owner_.empty()||reverse_running_||(!controller_.empty()&&(!play_connection_||encrypted_reverse_))||last_request_ms_==0||now_ms-last_request_ms_<30000)return false;commands_.clear();DirectCommand stop;stop.kind=DirectCommandKind::Stop;commands_.push_back(stop);last_request_ms_=0;return true;}

void DirectSession::cancel_fetch(){std::lock_guard<std::mutex> l(mutex_);pending_data_.clear();pending_done_=true;event_cv_.notify_all();}
