#include "direct_video.h"
#include <plist/plist.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <algorithm>
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
std::string direct_load_payload(const std::string& uri,const std::string& app) {
    if(!direct_uri_valid(uri)||app!="com.github.depius.uxair.dev")return {};
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
    if(owner.empty()||owner.size()>128||target.size()>256||body.size()>16384)return reply;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!owner_.empty()&&owner_!=owner){reply.status=409;return reply;}
    DirectCommand command;command.kind=DirectCommandKind::Stop;
    if(method=="POST"&&target=="/play") {
        if(busy||snapshot_.state==DirectState::Loading||snapshot_.state==DirectState::Playing||snapshot_.state==DirectState::Paused){reply.status=409;return reply;}
        if(!play_body(type,body,command.uri,command.position))return reply;
        if(!commands_.empty()){reply.status=503;return reply;}
        command.kind=DirectCommandKind::Play;owner_=owner;snapshot_=DirectSnapshot();snapshot_.state=DirectState::Loading;
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
        else {reply.type="text/x-apple-plist+xml";out<<"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>duration</key><real>"<<snapshot_.duration<<"</real><key>position</key><real>"<<snapshot_.position<<"</real><key>rate</key><real>"<<(snapshot_.state==DirectState::Playing?1:0)<<"</real><key>readyToPlay</key><"<<(snapshot_.state==DirectState::Playing||snapshot_.state==DirectState::Paused?"true":"false")<<"/></dict></plist>";}
        reply.body=out.str();reply.status=200;return reply;
    } else {reply.status=501;return reply;}
    if(command.kind!=DirectCommandKind::Play&&command.kind!=DirectCommandKind::Stop&&(snapshot_.state==DirectState::Idle||snapshot_.state==DirectState::Error||snapshot_.state==DirectState::Ended)){reply.status=409;return reply;}
    if(commands_.size()>=8){reply.status=503;return reply;}commands_.push_back(command);reply.status=200;return reply;
}
bool DirectSession::take(DirectCommand& command){std::lock_guard<std::mutex> l(mutex_);if(commands_.empty())return false;command=commands_.front();commands_.pop_front();return true;}
void DirectSession::publish(DirectSnapshot s){std::lock_guard<std::mutex> l(mutex_);snapshot_=s;if(s.state==DirectState::Idle||s.state==DirectState::Ended||s.state==DirectState::Error){owner_.clear();commands_.clear();}}
DirectSnapshot DirectSession::snapshot(){std::lock_guard<std::mutex> l(mutex_);return snapshot_;}
void DirectSession::reset(){std::lock_guard<std::mutex> l(mutex_);commands_.clear();owner_.clear();snapshot_=DirectSnapshot();}
