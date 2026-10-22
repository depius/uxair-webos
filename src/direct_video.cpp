#include "direct_video.h"
#include <plist/plist.h>
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
static std::string escape(const std::string& s){std::string out;for(char c:s){if(c=='"'||c=='\\')out+='\\';out+=c;}return out;}
std::string direct_load_payload(const std::string& uri,const std::string& app,bool internal) {
    if((!direct_uri_valid(uri)&&!(internal&&uri.compare(0,17,"http://127.0.0.1:")==0&&uri.find_first_of("\r\n\"\\")==std::string::npos))||app!="com.github.depius.uxair.dev")return {};
    return "{\"args\":[{\"uri\":\""+escape(uri)+"\",\"mediaTransportType\":\"HLS\",\"option\":{\"appId\":\""+app+"\",\"adaptiveStreaming\":{\"maxWidth\":1920,\"maxHeight\":1080},\"queryPosition\":true}}]}";
}
static bool play_body(const std::string& type,const std::string& body,std::string& uri,double& start) {
    start=0;if(body.empty()||body.size()>16384)return false;
    if(type=="application/x-apple-binary-plist"||type=="text/x-apple-plist+xml"||type=="application/x-apple-plist+xml") {
        plist_t root=nullptr;
        if(type=="application/x-apple-binary-plist")plist_from_bin(body.data(),uint32_t(body.size()),&root);
        else {if(body.find("<!ENTITY")!=std::string::npos)return false;plist_from_xml(body.data(),uint32_t(body.size()),&root);}
        if(!root||plist_get_node_type(root)!=PLIST_DICT){if(root)plist_free(root);return false;}
        plist_t location=plist_dict_get_item(root,"Content-Location"),position=plist_dict_get_item(root,"Start-Position");
        bool valid=location&&plist_get_node_type(location)==PLIST_STRING;
        if(valid){char *v=nullptr;plist_get_string_val(location,&v);if(v){uri=v;free(v);}else valid=false;}
        if(position){if(plist_get_node_type(position)==PLIST_REAL)plist_get_real_val(position,&start);
            else if(plist_get_node_type(position)==PLIST_UINT){uint64_t v=0;plist_get_uint_val(position,&v);start=double(v);}else valid=false;}
        plist_free(root);return valid&&direct_uri_valid(uri)&&std::isfinite(start)&&start>=0&&start<=1;
    }
    if(type!="text/parameters")return false;
    std::istringstream in(body);std::string line;bool found=false,position=false;
    while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();const size_t sep=line.find(": ");
        if(line.empty())continue;if(sep==std::string::npos)return false;
        const std::string key=line.substr(0,sep),value=line.substr(sep+2);
        if(key=="Content-Location"){if(found)return false;uri=value;found=true;}
        else if(key=="Start-Position"){if(position||!number(value,start)||start>1)return false;position=true;}
        else return false;
    }
    return found&&direct_uri_valid(uri);
}
DirectReply DirectSession::request(const std::string& owner,const std::string& method,const std::string& target,const std::string& type,const std::string& body,bool busy) {
    DirectReply reply;
    if(owner.empty()||owner.size()>128||target.size()>256||body.size()>524288)return reply;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!owner_.empty()&&owner_!=owner){reply.status=409;return reply;}
    last_request_ms_=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    DirectCommand command;command.kind=DirectCommandKind::Stop;
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
            if(!pending_id_||id!=pending_id_||url!=pending_url_){reply.status=409;plist_free(root);return reply;}
            char *data=nullptr;uint64_t len=0;if(data_node&&plist_get_node_type(data_node)==PLIST_DATA)plist_get_data_val(data_node,&data,&len);
            if(status==200&&data&&len<=262144&&len>=7&&std::string(data,7)=="#EXTM3U")pending_data_.assign(data,size_t(len));
            pending_done_=true;free(data);event_cv_.notify_all();reply.status=status==200&&!pending_data_.empty()?200:422;
            plist_free(root);return reply;
        }
        if(kind=="playlistRemove") {commands_.clear();command.kind=DirectCommandKind::Stop;}
        else {reply.status=501;plist_free(root);return reply;}
        plist_free(root);
    } else if((method=="GET"||method=="PUT")&&(target.compare(0,13,"/getProperty?")==0||target.compare(0,13,"/setProperty?")==0)) {
        const auto key=target.substr(13);
        if(method=="GET"&&(key=="playbackPosition"||key=="duration"||key=="rate")) {
            std::ostringstream out;out<<"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>value</key><real>"<<(key=="duration"?snapshot_.duration:key=="rate"?(snapshot_.state==DirectState::Playing?1:0):snapshot_.position)<<"</real></dict></plist>";
            reply.body=out.str();reply.type="text/x-apple-plist+xml";reply.status=200;return reply;
        }
        // Do not acknowledge subtitle, rate-boundary or playlist mutation features we do not implement.
        reply.status=501;return reply;
    } else if(method=="POST"&&target=="/play") {
        if(busy||snapshot_.state==DirectState::Loading||snapshot_.state==DirectState::Playing||snapshot_.state==DirectState::Paused){reply.status=409;return reply;}
        if(!play_body(type.substr(0,type.find(';')),body,command.uri,command.position))return reply;
        if(!commands_.empty()){reply.status=503;return reply;}
        command.kind=DirectCommandKind::Play;owner_=owner;snapshot_=DirectSnapshot();snapshot_.state=DirectState::Loading;
        if(reverse_running_){events_.push_back("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>category</key><string>video</string><key>state</key><string>loading</string></dict></plist>");event_cv_.notify_all();}
    } else if(method=="POST"&&target=="/stop") {commands_.clear();}
    else if(method=="POST"&&target.compare(0,6,"/rate?")==0){
        if(target=="/rate?value=0")command.kind=DirectCommandKind::Pause;
        else if(target=="/rate?value=1")command.kind=DirectCommandKind::Resume;else return reply;
    } else if(method=="POST"&&target.compare(0,16,"/scrub?position=")==0) {
        if(!number(target.substr(16),command.position))return reply;command.kind=DirectCommandKind::Seek;
    } else if(method=="GET"&&(target=="/scrub"||target=="/playback-info")) {
        if(snapshot_.state==DirectState::Error){reply.status=503;return reply;}
        std::ostringstream out;
        if(target=="/scrub")out<<"duration: "<<snapshot_.duration<<"\nposition: "<<snapshot_.position<<"\n";
        else {reply.type="text/x-apple-plist+xml";out<<"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>duration</key><real>"<<snapshot_.duration<<"</real><key>position</key><real>"<<snapshot_.position<<"</real><key>rate</key><real>"<<(snapshot_.state==DirectState::Playing?1:0)<<"</real><key>readyToPlay</key><"<<(snapshot_.state==DirectState::Playing||snapshot_.state==DirectState::Paused?"true":"false")<<"/><key>playbackBufferEmpty</key><"<<(snapshot_.state==DirectState::Loading?"true":"false")<<"/><key>playbackBufferFull</key><false/><key>playbackLikelyToKeepUp</key><"<<(snapshot_.state==DirectState::Playing?"true":"false")<<"/><key>loadedTimeRanges</key><array/><key>seekableTimeRanges</key><array>"<<(snapshot_.duration>0?"<dict><key>start</key><real>0</real><key>duration</key><real>"+std::to_string(snapshot_.duration)+"</real></dict>":"")<<"</array></dict></plist>";}
        reply.body=out.str();reply.status=200;return reply;
    } else {reply.status=501;return reply;}
    if(command.kind!=DirectCommandKind::Play&&command.kind!=DirectCommandKind::Stop&&(snapshot_.state==DirectState::Idle||snapshot_.state==DirectState::Error||snapshot_.state==DirectState::Ended)){reply.status=409;return reply;}
    if(commands_.size()>=8){reply.status=503;return reply;}commands_.push_back(command);reply.status=200;return reply;
}
bool DirectSession::take(DirectCommand& command){std::lock_guard<std::mutex> l(mutex_);if(commands_.empty())return false;command=commands_.front();commands_.pop_front();return true;}
const char *direct_state_name(DirectState s){switch(s){case DirectState::Loading:return "loading";case DirectState::Playing:return "playing";case DirectState::Paused:return "paused";case DirectState::Ended:case DirectState::Idle:return "stopped";default:return "stopped";}}
void DirectSession::publish(DirectSnapshot s){
    std::lock_guard<std::mutex> l(mutex_);
    if(!std::isfinite(s.position)||s.position<0)s.position=0;if(!std::isfinite(s.duration)||s.duration<0)s.duration=0;
    if(reverse_running_&&s.state!=snapshot_.state){std::string event="<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>category</key><string>video</string><key>state</key><string>";event+=direct_state_name(s.state);event+="</string>";if(s.state==DirectState::Error)event+="<key>reason</key><string>error</string>";event+="</dict></plist>";if(events_.size()>=16)events_.pop_front();events_.push_back(event);event_cv_.notify_all();}
    snapshot_=s;if(s.state==DirectState::Idle||s.state==DirectState::Ended||s.state==DirectState::Error){if(!reverse_running_)owner_.clear();commands_.clear();}
}
DirectSnapshot DirectSession::snapshot(){std::lock_guard<std::mutex> l(mutex_);return snapshot_;}
void DirectSession::close_reverse(){reverse_running_=false;event_cv_.notify_all();if(reverse_thread_.joinable())reverse_thread_.join();if(reverse_fd_>=0){close(reverse_fd_);reverse_fd_=-1;}}
DirectSession::~DirectSession(){close_reverse();}
bool DirectSession::bind_reverse(const std::string& owner,int fd,uintptr_t token){
    if(owner.empty()||owner.size()>128||owner.find_first_of("\r\n")!=std::string::npos)return false;
    {std::lock_guard<std::mutex> l(mutex_);if(reverse_running_||(!owner_.empty()&&owner_!=owner))return false;}
    close_reverse();int copy=dup(fd);if(copy<0)return false;
    {std::lock_guard<std::mutex> l(mutex_);reverse_fd_=copy;reverse_token_=token;owner_=owner;reverse_running_=true;}
    reverse_thread_=std::thread(&DirectSession::reverse_loop,this);return true;
}
void DirectSession::reverse_loop(){
    while(reverse_running_){std::string body,owner;
        {std::unique_lock<std::mutex> l(mutex_);event_cv_.wait_for(l,std::chrono::milliseconds(200),[&]{return !reverse_running_||!events_.empty();});if(!reverse_running_)break;if(events_.empty())continue;body=events_.front();events_.pop_front();owner=owner_;}
        std::string wire="POST /event HTTP/1.1\r\nContent-Type: text/x-apple-plist+xml\r\nX-Apple-Session-ID: "+owner+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
        size_t offset=0;while(offset<wire.size()&&reverse_running_){pollfd p={reverse_fd_,POLLOUT,0};if(poll(&p,1,250)<=0){reverse_running_=false;break;}auto n=send(reverse_fd_,wire.data()+offset,wire.size()-offset,MSG_DONTWAIT|MSG_NOSIGNAL);if(n<=0){reverse_running_=false;break;}offset+=size_t(n);}
        if(!reverse_running_)connection_lost(reverse_token_);
    }
}
void DirectSession::connection_lost(uintptr_t token){std::lock_guard<std::mutex> l(mutex_);if(token!=reverse_token_)return;reverse_running_=false;owner_.clear();snapshot_=DirectSnapshot();events_.clear();pending_done_=true;commands_.clear();DirectCommand stop;stop.kind=DirectCommandKind::Stop;commands_.push_back(stop);event_cv_.notify_all();}
std::string DirectSession::sender_manifest(const std::string& url){
    std::unique_lock<std::mutex> l(mutex_);if(!reverse_running_||pending_id_||!direct_uri_valid(url)||events_.size()>=16)return {};
    pending_id_=++request_id_;pending_url_=url;pending_done_=false;pending_data_.clear();
    plist_t root=plist_new_dict(),request=plist_new_dict(),headers=plist_new_dict();
    plist_dict_set_item(root,"sessionID",plist_new_uint(1));plist_dict_set_item(root,"type",plist_new_string("unhandledURLRequest"));
    plist_dict_set_item(request,"FCUP_Response_RequestID",plist_new_uint(pending_id_));plist_dict_set_item(request,"FCUP_Response_URL",plist_new_string(url.c_str()));
    plist_dict_set_item(request,"FCUP_Response_ClientInfo",plist_new_uint(1));plist_dict_set_item(request,"FCUP_Response_ClientRef",plist_new_uint(40030004));
    plist_dict_set_item(request,"sessionID",plist_new_uint(1));plist_dict_set_item(headers,"X-Playback-Session-Id",plist_new_string(owner_.c_str()));plist_dict_set_item(request,"FCUP_Response_Headers",headers);plist_dict_set_item(root,"request",request);
    char *xml=nullptr;uint32_t len=0;plist_to_xml(root,&xml,&len);plist_free(root);if(xml){events_.push_back(std::string(xml,len));free(xml);}event_cv_.notify_all();
    event_cv_.wait_for(l,std::chrono::seconds(3),[&]{return pending_done_||!reverse_running_;});const auto result=pending_data_;pending_data_.clear();pending_id_=0;pending_url_.clear();return result;
}
void DirectSession::reset(bool keep_reverse){if(!keep_reverse)close_reverse();std::lock_guard<std::mutex> l(mutex_);commands_.clear();if(!keep_reverse){owner_.clear();events_.clear();reverse_token_=0;}snapshot_=DirectSnapshot();pending_done_=true;event_cv_.notify_all();}

bool DirectSession::check_timeout(int64_t now_ms){std::lock_guard<std::mutex> lock(mutex_);if(owner_.empty()||reverse_running_||last_request_ms_==0||now_ms-last_request_ms_<30000)return false;commands_.clear();DirectCommand stop;stop.kind=DirectCommandKind::Stop;commands_.push_back(stop);last_request_ms_=0;return true;}

void DirectSession::cancel_fetch(){std::lock_guard<std::mutex> l(mutex_);pending_data_.clear();pending_done_=true;event_cv_.notify_all();}
