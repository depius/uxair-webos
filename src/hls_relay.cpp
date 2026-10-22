#include "hls_relay.h"
#include "direct_video.h"
#include <curl/curl.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cerrno>
static const size_t manifest_limit=262144,segment_limit=8*1024*1024;
void HlsRelay::trace(const char *stage,int code,long http){
    if(diagnostic_records_.load()<64&&diagnostic_records_.fetch_add(1)<64)
        fprintf(stderr,"[hls-relay] stage=%s code=%d http=%ld\n",stage,code,http);
}
std::string HlsRelay::resource_path(size_t index,const std::string& uri,bool manifest) const {
    std::string suffix=".bin";
    const auto path=uri.substr(0,uri.find('?'));
    for(const char *known:{".ts",".m4s",".mp4",".aac"}){
        const size_t n=strlen(known);if(path.size()>=n&&path.compare(path.size()-n,n,known)==0){suffix=known;break;}}
    if(manifest)suffix=".m3u8";
    return "/"+token_+"/"+std::to_string(index)+suffix;
}
struct WriteBuffer {std::string data;size_t limit;};
static size_t write_data(char *data,size_t size,size_t n,void *ptr){auto& b=*static_cast<WriteBuffer*>(ptr);if(n&&size>SIZE_MAX/n)return 0;const size_t count=size*n;if(count>b.limit-b.data.size())return 0;b.data.append(data,count);return count;}
bool HlsRelay::address_allowed(uint32_t address) const {
    uint32_t ip=ntohl(address);unsigned a=ip>>24,b=(ip>>16)&255;
    if(!a||a==127||a>=224||(a==169&&b==254))return false;
    const bool private_ip=a==10||(a==172&&b>=16&&b<=31)||(a==192&&b==168)||(a==100&&b>=64&&b<=127);
    if(!private_ip)return true;
    in_addr allowed={};return !allowed_lan_.empty()&&inet_pton(AF_INET,allowed_lan_.c_str(),&allowed)==1&&allowed.s_addr==address;
}
static curl_socket_t open_socket(void *ctx,curlsocktype purpose,struct curl_sockaddr *address){
    if(purpose!=CURLSOCKTYPE_IPCXN||address->family!=AF_INET)return CURL_SOCKET_BAD;
    auto *relay=static_cast<HlsRelay*>(ctx);auto *a=reinterpret_cast<sockaddr_in*>(&address->addr);
    if(!relay->address_allowed(a->sin_addr.s_addr)){relay->trace("policy-denied",0);return CURL_SOCKET_BAD;}
    return socket(address->family,address->socktype,address->protocol);
}
static int progress(void *ctx,curl_off_t,curl_off_t,curl_off_t,curl_off_t){return !static_cast<std::atomic<bool>*>(ctx)->load();}
std::string HlsRelay::resolve(const std::string& base,const std::string& relative){
    CURLU *u=curl_url();if(!u)return {};std::string out;
    if(curl_url_set(u,CURLUPART_URL,base.c_str(),0)==CURLUE_OK&&curl_url_set(u,CURLUPART_URL,relative.c_str(),0)==CURLUE_OK){char *p=nullptr;if(curl_url_get(u,CURLUPART_URL,&p,0)==CURLUE_OK){out=p;curl_free(p);}}
    curl_url_cleanup(u);return direct_uri_valid(out)?out:std::string();
}
HlsFetch HlsRelay::fetch(const std::string& uri,size_t limit,bool manifest){
    HlsFetch result;std::string current=uri;const auto began=std::chrono::steady_clock::now();
    for(int redirect=0;redirect<=3&&running_;++redirect){
        const long remaining=12000-long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count());
        if(remaining<=0){result.status=504;break;}
        if(!direct_uri_valid(current)){trace("invalid-url",0);break;}
        CURL *curl=curl_easy_init();if(!curl)break;WriteBuffer buffer;buffer.limit=std::min(limit,segment_limit);
        curl_easy_setopt(curl,CURLOPT_URL,current.c_str());curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(curl,CURLOPT_PROXY,"");curl_easy_setopt(curl,CURLOPT_NETRC,CURL_NETRC_IGNORED);
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,3000L);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,remaining);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_V4);
        char executable[4096];auto executable_length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
        std::string ca;
        if(executable_length>0){executable[executable_length]=0;ca=executable;ca=ca.substr(0,ca.find_last_of('/'))+"/hls-ca.pem";if(access(ca.c_str(),R_OK)==0)curl_easy_setopt(curl,CURLOPT_CAINFO,ca.c_str());}
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
        curl_easy_setopt(curl,CURLOPT_OPENSOCKETFUNCTION,open_socket);curl_easy_setopt(curl,CURLOPT_OPENSOCKETDATA,this);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_data);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);
        curl_easy_setopt(curl,CURLOPT_MAXFILESIZE_LARGE,curl_off_t(buffer.limit));
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&running_);
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"UxAir/1.2");
        auto code=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        char *location=nullptr,*type=nullptr;curl_easy_getinfo(curl,CURLINFO_REDIRECT_URL,&location);curl_easy_getinfo(curl,CURLINFO_CONTENT_TYPE,&type);
        std::string next=location?location:"";result.type=type?type:"application/octet-stream";
        curl_easy_cleanup(curl);++requests;trace(manifest?"manifest-fetch":"segment-fetch",int(code),status);
        if(code!=CURLE_OK){result.status=code==CURLE_OPERATION_TIMEDOUT?504:502;break;}
        if(status>=300&&status<400&&!next.empty()){current=resolve(current,next);continue;}
        if(status!=200){result.status=status==404?404:502;break;}
        result.status=200;result.data.swap(buffer.data);result.url=current;bytes+=result.data.size();return result;
    }
    if(sender_manifest&&manifest&&running_){auto playlist=sender_manifest(uri);if(!playlist.empty()&&playlist.size()<=manifest_limit){result.status=200;result.data=playlist;result.url=uri;result.type="application/vnd.apple.mpegurl";return result;}}
    ++failures;return result;
}
std::string HlsRelay::register_url(const std::string& uri,bool manifest){
    if(!direct_uri_valid(uri))return {};
    std::lock_guard<std::mutex> lock(mutex_);auto i=std::find(resources_.begin(),resources_.end(),uri);
    if(i==resources_.end()){size_t total=uri.size();for(const auto& item:resources_)total+=item.size();if(resources_.size()>=4096||total>2097152)return {};resources_.push_back(uri);manifests_.push_back(manifest);i=resources_.end()-1;}
    if(manifest)manifests_[size_t(i-resources_.begin())]=true;
    const size_t index=size_t(i-resources_.begin());
    return "http://127.0.0.1:"+std::to_string(port_)+resource_path(index,uri,manifests_[index]);
}
std::string HlsRelay::rewrite(const std::string& input,const std::string& base){
    if(input.size()>manifest_limit||input.compare(0,7,"#EXTM3U")!=0)return {};
    std::istringstream in(input);std::string line,out;bool variant=false;
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.size()>8192||line.find("BYTERANGE")!=std::string::npos||line.find("CONTENT-STEERING")!=std::string::npos)return {};
        if(line.compare(0,10,"#EXT-X-KEY")==0&&line.find("METHOD=NONE")==std::string::npos)return {}; // no DRM/key relay
        if(line.compare(0,18,"#EXT-X-SESSION-KEY")==0)return {};
        if(!line.empty()&&line[0]!='#'){line=register_url(resolve(base,line),variant);variant=false;if(line.empty())return {};}
        else {size_t pos=0;while((pos=line.find("URI=\"",pos))!=std::string::npos){const size_t first=pos+5,last=line.find('"',first);if(last==std::string::npos)return {};
            const auto local=register_url(resolve(base,line.substr(first,last-first)),line.find("#EXT-X-MEDIA:")==0||line.find("#EXT-X-I-FRAME-STREAM-INF:")==0);if(local.empty())return {};line.replace(first,last-first,local);pos=first+local.size()+1;}}
        if(line.find("#EXT-X-STREAM-INF:")==0)variant=true;
        out+=line+"\n";if(out.size()>manifest_limit)return {};
    }
    return out;
}
static bool send_all(int fd,const std::string& data){size_t sent=0;while(sent<data.size()){auto n=send(fd,data.data()+sent,data.size()-sent,MSG_NOSIGNAL);if(n<=0)return false;sent+=size_t(n);}return true;}
void HlsRelay::serve(){
    while(running_){
        int fd=accept(listener_,nullptr,nullptr);if(fd<0)break;
        {std::lock_guard<std::mutex> lock(clients_mutex_);clients_.push_back(fd);}
        timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        std::string request;char buffer[1024];
        while(request.find("\r\n\r\n")==std::string::npos&&request.size()<4096){auto n=recv(fd,buffer,std::min(sizeof(buffer),4096-request.size()),0);if(n<=0)break;request.append(buffer,size_t(n));}
        ++incoming_requests;
        HlsFetch response;response.status=404;std::string target,method;std::istringstream header(request);header>>method>>target;
        const std::string prefix="/"+token_+"/";bool manifest=false;long resource=-1;
        if(request.find("\r\n\r\n")==std::string::npos)response.status=request.size()>=4096?431:400;
        else if((method=="GET"||method=="HEAD")&&target.compare(0,prefix.size(),prefix)==0){
            const auto tail=target.substr(prefix.size());const auto dot=tail.find('.');const auto id=tail.substr(0,dot);
            if(!id.empty()&&id.size()<=4&&id.find_first_not_of("0123456789")==std::string::npos){
                const size_t index=size_t(strtoul(id.c_str(),nullptr,10));std::string url;
                {std::lock_guard<std::mutex> lock(mutex_);
                    if(index<resources_.size()&&target==resource_path(index,resources_[index],manifests_[index])){
                        url=resources_[index];manifest=manifests_[index];resource=long(index);}}
                if(!url.empty()){
                    response=fetch(url,manifest?manifest_limit:segment_limit,manifest);
                    if(response.status==200&&(manifest||response.data.compare(0,7,"#EXTM3U")==0||response.type.find("mpegurl")!=std::string::npos)){
                        response.data=rewrite(response.data,response.url);response.type="application/vnd.apple.mpegurl";
                        if(response.data.empty()){response.status=422;trace("manifest-invalid",0);++failures;}
                    }else if(response.status==200){
                        // Windows may label .ts files as text; native demuxers need a media MIME type.
                        if(tail.size()>=3&&tail.compare(tail.size()-3,3,".ts")==0)response.type="video/mp2t";
                        else if((tail.size()>=4&&tail.compare(tail.size()-4,4,".mp4")==0)||(tail.size()>=4&&tail.compare(tail.size()-4,4,".m4s")==0))response.type="video/mp4";
                        else if(tail.size()>=4&&tail.compare(tail.size()-4,4,".aac")==0)response.type="audio/aac";
                        else response.type="application/octet-stream";
                    }
                }
            }
        }
        if(response.status!=200)response.data.clear();
        const char *label=method=="GET"?"GET":method=="HEAD"?"HEAD":"other";
        if(diagnostic_records_.load()<64&&diagnostic_records_.fetch_add(1)<64)
            fprintf(stderr,"[hls-relay] request=%s resource=%ld manifest=%d http=%d bytes=%zu\n",label,resource,int(manifest),response.status,response.data.size());
        std::string wire="HTTP/1.1 "+std::to_string(response.status)+" Result\r\nConnection: close\r\nContent-Type: "+(response.type.empty()?"application/octet-stream":response.type)+"\r\nContent-Length: "+std::to_string(response.data.size())+"\r\n\r\n";
        send_all(fd,wire);if(method!="HEAD")send_all(fd,response.data);
        {std::lock_guard<std::mutex> lock(clients_mutex_);clients_.erase(std::remove(clients_.begin(),clients_.end(),fd),clients_.end());close(fd);}
    }
}
std::string HlsRelay::start(const std::string& uri){
    stop();diagnostic_records_=0;bytes=requests=failures=incoming_requests=0;
    if(!direct_uri_valid(uri)){trace("start-invalid-url",0);return {};}
    static std::once_flag init;static CURLcode initialized=CURLE_FAILED_INIT;
    std::call_once(init,[]{initialized=curl_global_init(CURL_GLOBAL_DEFAULT);});
    if(initialized!=CURLE_OK){trace("curl-initialize",int(initialized));return {};}
    const char *lan=getenv("UXAIR_DIRECT_LAN");allowed_lan_=lan?lan:"";
    trace("start-lan-exception",allowed_lan_.empty()?0:1);
    unsigned char secret[16];const int random=RAND_bytes(secret,sizeof(secret));
    if(random!=1){trace("random",random);return {};}
    token_.clear();const char hex[]="0123456789abcdef";for(auto c:secret){token_+=hex[c>>4];token_+=hex[c&15];}
    listener_=socket(AF_INET,SOCK_STREAM,0);if(listener_<0){trace("socket",errno);return {};}
    sockaddr_in a={};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::bind(listener_.load(),reinterpret_cast<sockaddr*>(&a),sizeof(a))){trace("bind",errno);stop();return {};}
    if(::listen(listener_.load(),4)){trace("listen",errno);stop();return {};}
    socklen_t length=sizeof(a);
    if(getsockname(listener_,reinterpret_cast<sockaddr*>(&a),&length)){trace("socket-name",errno);stop();return {};}
    port_=ntohs(a.sin_port);running_=true;
    const auto local=register_url(uri,true);if(local.empty()){trace("catalog",0);stop();return {};}
    try{for(int i=0;i<2;++i)workers_.emplace_back(&HlsRelay::serve,this);}catch(...){trace("workers",0);stop();return {};}
    trace("listening-port",int(port_));return local;
}
void HlsRelay::stop(){
    const bool active=listener_.load()>=0||!workers_.empty();
    running_=false;
    // Keep the listening descriptor alive until accept workers have exited.
    // Closing it first could let an unrelated RAOP socket reuse the descriptor.
    int fd=listener_.load();if(fd>=0)shutdown(fd,SHUT_RDWR);
    {std::lock_guard<std::mutex> lock(clients_mutex_);for(int client:clients_)shutdown(client,SHUT_RDWR);}
    for(auto& worker:workers_)if(worker.joinable())worker.join();workers_.clear();
    fd=listener_.exchange(-1);if(fd>=0)close(fd);
    if(active)fprintf(stderr,"[hls-relay] stage=stopped native_requests=%llu upstream_fetches=%llu bytes=%llu failures=%llu\n",
        (unsigned long long)incoming_requests.load(),(unsigned long long)requests.load(),(unsigned long long)bytes.load(),(unsigned long long)failures.load());
    std::lock_guard<std::mutex> lock(mutex_);resources_.clear();manifests_.clear();
}
