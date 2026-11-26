#include "hls_relay.h"
#include "direct_video.h"
#include <curl/curl.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <strings.h>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <iterator>
#include <cerrno>
#include <cctype>
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
bool hls_decimal_integer(const std::string& value, uint64_t& number) {
    if (value.empty() || value.size()>20) return false;
    number=0;
    for (const char c : value) {
        if(c<'0'||c>'9'||number>(UINT64_MAX-unsigned(c-'0'))/10) return false;
        number=number*10+unsigned(c-'0');
    }
    return true;
}
struct WriteBuffer {
    std::string data;
    size_t limit;
    bool exceeded=false;
    HlsRange range;
    bool range_seen=false, range_match=false, other_encoding=false;
    long http=0;
};
static size_t range_header(char *data,size_t size,size_t n,void *ptr){
    auto& b=*static_cast<WriteBuffer*>(ptr);
    if (n && size>SIZE_MAX/n) return 0;
    const size_t count=size*n;
    if (count>8192) return 0;
    const std::string line(data,count);
    if (line.compare(0,5,"HTTP/")==0) {
        const auto space=line.find(' ');
        b.http=space==std::string::npos?0:strtol(line.c_str()+space+1,nullptr,10);
        b.range_seen=b.range_match=b.other_encoding=false;
        b.data.clear();
    } else if (!strncasecmp(line.c_str(),"Content-Range:",14)) {
        if (b.range_seen) { b.range_match=false; return count; }
        b.range_seen=true;
        const auto start=line.find_first_not_of(" \t",14);
        if (start==std::string::npos || line.compare(start,6,"bytes ")) return count;
        const auto dash=line.find('-',start+6), slash=line.find('/',start+6);
        if (dash==std::string::npos||slash==std::string::npos||dash>=slash) return count;
        const auto end=line.find_last_not_of(" \t\r\n");
        if (end<=slash) return count;
        uint64_t first,last,total;
        const auto extent=line.substr(slash+1,end-slash);
        b.range_match=hls_decimal_integer(line.substr(start+6,dash-start-6),first)&&
            hls_decimal_integer(line.substr(dash+1,slash-dash-1),last)&&
            (extent=="*"||(hls_decimal_integer(extent,total)&&total>last))&&
            first==b.range.offset&&last==b.range.offset+b.range.length-1;
    } else if (!strncasecmp(line.c_str(),"Content-Encoding:",17)) {
        const auto first=line.find_first_not_of(" \t",17);
        const auto last=line.find_last_not_of(" \t\r\n");
        if (first==std::string::npos||last<first||strcasecmp(line.substr(first,last-first+1).c_str(),"identity"))
            b.other_encoding=true;
    } else if ((line=="\r\n" || line=="\n") && b.http>=200 && b.http<300 &&
               (b.http!=206 || !b.range_match || b.other_encoding)) {
        return 0;
    }
    return count;
}
static size_t write_data(char *data,size_t size,size_t n,void *ptr){
    auto& b=*static_cast<WriteBuffer*>(ptr);
    if(n&&size>SIZE_MAX/n){b.exceeded=true;return 0;}
    const size_t count=size*n;
    if (b.range.length && b.http>=300 && b.http<400) return count;
    if (b.range.length && (b.http!=206||!b.range_match||b.other_encoding)) return 0;
    if(count>b.limit-b.data.size()){b.exceeded=true;return 0;}
    b.data.append(data,count);return count;
}
bool HlsRelay::address_allowed(uint32_t address) const {
    uint32_t ip=ntohl(address);unsigned a=ip>>24,b=(ip>>16)&255;
    if(direct_public_ipv4(ip))return true;
    if(!(a==10||(a==172&&b>=16&&b<=31)||(a==192&&b==168)))return false;
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
    const unsigned flags=direct_uri_valid(relative)?CURLU_PATH_AS_IS:0;
    if(curl_url_set(u,CURLUPART_URL,base.c_str(),CURLU_PATH_AS_IS)==CURLUE_OK&&curl_url_set(u,CURLUPART_URL,relative.c_str(),flags)==CURLUE_OK){char *p=nullptr;if(curl_url_get(u,CURLUPART_URL,&p,0)==CURLUE_OK){out=p;curl_free(p);}}
    curl_url_cleanup(u);return direct_uri_valid(out)?out:std::string();
}

static std::string http_origin(const std::string& uri) {
    auto* url=curl_url();
    if(!url) return {};
    char *scheme=nullptr,*host=nullptr,*port=nullptr;
    std::string origin;
    if(curl_url_set(url,CURLUPART_URL,uri.c_str(),0)==CURLUE_OK &&
       curl_url_get(url,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK &&
       curl_url_get(url,CURLUPART_HOST,&host,0)==CURLUE_OK &&
       curl_url_get(url,CURLUPART_PORT,&port,CURLU_DEFAULT_PORT)==CURLUE_OK) {
        origin=std::string(scheme)+"://"+host+":"+port;
        std::transform(origin.begin(),origin.end(),origin.begin(),[](unsigned char c){return char(std::tolower(c));});
    }
    curl_free(scheme);curl_free(host);curl_free(port);curl_url_cleanup(url);
    return origin;
}

void HlsRelay::set_request_origin(const std::string& uri,uint64_t generation) {
    // Called before starting this run's workers. Share only cookies issued by
    // the original media origin. A redirect cannot expand their HTTP origin.
    request_origin_=http_origin(uri);
    generation_=generation;
    cookie_share_=curl_share_init();
    if(!cookie_share_) return;
    curl_share_setopt(cookie_share_,CURLSHOPT_LOCKFUNC,+[](CURL*,curl_lock_data data,curl_lock_access,void* mutex) {
        if(data==CURL_LOCK_DATA_COOKIE) static_cast<std::mutex*>(mutex)->lock();
    });
    curl_share_setopt(cookie_share_,CURLSHOPT_UNLOCKFUNC,+[](CURL*,curl_lock_data data,void* mutex) {
        if(data==CURL_LOCK_DATA_COOKIE) static_cast<std::mutex*>(mutex)->unlock();
    });
    curl_share_setopt(cookie_share_,CURLSHOPT_USERDATA,&cookie_mutex_);
    curl_share_setopt(cookie_share_,CURLSHOPT_SHARE,CURL_LOCK_DATA_COOKIE);
}

HlsFetch HlsRelay::fetch(const std::string& uri,size_t limit,bool manifest,HlsRange range,const char* role){
    HlsFetch result;std::string current=uri;const auto began=std::chrono::steady_clock::now();
    if (range.length && (range.length>std::min(limit,segment_limit)||range.offset>uint64_t(INT64_MAX)-range.length)) {
        result.range_rejected=true; return result;
    }
    for(int redirect=0;redirect<=3&&running_;++redirect){
        const long remaining=12000-long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count());
        if(remaining<=0){result.status=504;break;}
        if(!direct_uri_valid(current)){trace("invalid-url",0);break;}
        CURL *curl=curl_easy_init();if(!curl)break;WriteBuffer buffer;buffer.limit=std::min(limit,segment_limit);
        const bool same_origin=!request_origin_.empty() && http_origin(current)==request_origin_;
        if(same_origin && cookie_share_) {
            curl_easy_setopt(curl,CURLOPT_SHARE,cookie_share_);
            curl_easy_setopt(curl,CURLOPT_COOKIEFILE,"");
        }
        std::string request_range;
        if (range.length) {
            buffer.limit=size_t(range.length);
            buffer.range=range;
            request_range=std::to_string(range.offset)+"-"+std::to_string(range.offset+range.length-1);
            curl_easy_setopt(curl,CURLOPT_RANGE,request_range.c_str());
            curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,range_header);
            curl_easy_setopt(curl,CURLOPT_HEADERDATA,&buffer);
        }
        curl_easy_setopt(curl,CURLOPT_URL,current.c_str());curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(curl,CURLOPT_PATH_AS_IS,1L);
        curl_easy_setopt(curl,CURLOPT_PROXY,"");curl_easy_setopt(curl,CURLOPT_NETRC,CURL_NETRC_IGNORED);
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,3000L);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,remaining);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_V4);
        char executable[4096];auto executable_length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
        std::string ca;
        if(executable_length>0){executable[executable_length]=0;ca=executable;ca=ca.substr(0,ca.find_last_of('/'))+"/hls-ca.pem";if(access(ca.c_str(),R_OK)==0)curl_easy_setopt(curl,CURLOPT_CAINFO,ca.c_str());}
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
        curl_easy_setopt(curl,CURLOPT_OPENSOCKETFUNCTION,open_socket);curl_easy_setopt(curl,CURLOPT_OPENSOCKETDATA,this);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_data);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);
        curl_easy_setopt(curl,CURLOPT_MAXFILESIZE_LARGE,curl_off_t(std::min(limit,segment_limit)));
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&running_);
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"UxAir/1.4");
        auto code=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        result.http_status=int(status);
        char *location=nullptr,*type=nullptr;curl_easy_getinfo(curl,CURLINFO_REDIRECT_URL,&location);curl_easy_getinfo(curl,CURLINFO_CONTENT_TYPE,&type);
        std::string next=location?location:"";result.type=type?type:"application/octet-stream";
        struct curl_slist* cookies=nullptr;
        if(same_origin&&cookie_share_) curl_easy_getinfo(curl,CURLINFO_COOKIELIST,&cookies);
        unsigned stored=0;
        for(auto* item=cookies;item&&stored<=64;item=item->next) ++stored;
        if(stored>64) curl_easy_setopt(curl,CURLOPT_COOKIELIST,"ALL");
        const bool auth=status==401||status==403;
        if((requests.load()<24||auth||code!=CURLE_OK) && diagnostic_records_.load()<64)
            fprintf(stderr,"[hls-fetch] generation=%llu request=%s status=%ld code=%d redirects=%d origin=%s query=%d "
                    "cookie-store=%d supplied-headers=0 cancelled=%d retry=%s\n",
                    (unsigned long long)generation_,role?role:manifest?"manifest":"segment",status,int(code),redirect,
                    request_origin_.empty()?"unset":same_origin?"same":"cross",
                    int(current.find('?')!=std::string::npos),cookies!=nullptr,int(!running_.load()),
                    auth?"denied":"none");
        curl_slist_free_all(cookies);
        curl_easy_cleanup(curl);++requests;trace(manifest?"manifest-fetch":"segment-fetch",int(code),status);
        if(auth){result.status=int(status);break;}
        if(code!=CURLE_OK){
            result.limit_exceeded=buffer.exceeded||code==CURLE_FILESIZE_EXCEEDED;
            result.range_rejected=range.length&&status!=0&&(status!=206||!buffer.range_match||buffer.other_encoding||buffer.data.size()!=range.length);
            result.status=code==CURLE_OPERATION_TIMEDOUT?504:502;
            break;
        }
        if(!running_)break;
        if(status>=300&&status<400&&!next.empty()){
            auto resolved=resolve(current,next);
            if(current.compare(0,8,"https://")==0&&resolved.compare(0,8,"https://")!=0)break;
            current=std::move(resolved);
            continue;
        }
        if(range.length && (status!=206||!buffer.range_match||buffer.other_encoding||buffer.data.size()!=range.length)){
            result.range_rejected=true;break;
        }
        if(!range.length&&status!=200){result.status=status==404?404:502;break;}
        result.status=200;result.data.swap(buffer.data);result.url=current;bytes+=result.data.size();return result;
    }
    if(sender_manifest&&manifest&&running_&&(result.status==403||result.status==404)){auto playlist=sender_manifest(uri);if(!playlist.empty()&&playlist.size()<=manifest_limit){result.status=200;result.data=playlist;result.url=uri;result.type="application/vnd.apple.mpegurl";return result;}}
    if(!running_){
        result.cancelled=true;
        trace(manifest?"manifest-cancelled":"segment-cancelled",0);
    }else ++failures;
    return result;
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
    if(input.size()>manifest_limit||input.compare(0,7,"#EXTM3U")!=0||input.find('\0')!=std::string::npos)return {};
    std::istringstream in(input);std::string line,out;bool variant=false;
    if(!std::getline(in,line) || (line!="#EXTM3U" && line!="#EXTM3U\r"))return {};
    out="#EXTM3U\n";
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.size()>8192||line.find("BYTERANGE")!=std::string::npos||line.find("CONTENT-STEERING")!=std::string::npos)return {};
        if(line.compare(0,10,"#EXT-X-KEY")==0&&line!="#EXT-X-KEY:METHOD=NONE")return {}; // no DRM/key relay
        if(line.find("{$")!=std::string::npos) return {};
        if(line.compare(0,4,"#EXT")==0) {
            // Forward only tags whose URI handling is understood. Every network
            // reference must be registered here before native firmware sees it.
            const auto name=line.substr(0,line.find(':'));
            const std::string allowed[]={
                "#EXTINF","#EXT-X-VERSION","#EXT-X-TARGETDURATION","#EXT-X-MEDIA-SEQUENCE",
                "#EXT-X-DISCONTINUITY-SEQUENCE","#EXT-X-DISCONTINUITY","#EXT-X-ENDLIST",
                "#EXT-X-PLAYLIST-TYPE","#EXT-X-INDEPENDENT-SEGMENTS","#EXT-X-PROGRAM-DATE-TIME",
                "#EXT-X-START","#EXT-X-STREAM-INF","#EXT-X-MEDIA","#EXT-X-I-FRAME-STREAM-INF",
                "#EXT-X-MAP","#EXT-X-KEY"
            };
            if(std::find(std::begin(allowed),std::end(allowed),name)==std::end(allowed)) return {};
            const bool permits_uri=name=="#EXT-X-MEDIA" || name=="#EXT-X-I-FRAME-STREAM-INF" || name=="#EXT-X-MAP";
            if(permits_uri) {
                // Reject malformed URI attributes rather than relying on native parser leniency.
                const auto uri=line.find("URI");
                if(uri!=std::string::npos && line.compare(uri,5,"URI=\"")!=0)return {};
                if(uri==std::string::npos && name!="#EXT-X-MEDIA")return {};
            }
        }
        if(line.compare(0,18,"#EXT-X-SESSION-KEY")==0)return {};
        if(!line.empty()&&line[0]!='#'){line=register_url(resolve(base,line),variant);variant=false;if(line.empty())return {};}
        else {size_t pos=0;while((pos=line.find("URI=\"",pos))!=std::string::npos){const size_t first=pos+5,last=line.find('"',first);if(last==std::string::npos)return {};
            const auto local=register_url(resolve(base,line.substr(first,last-first)),line.find("#EXT-X-MEDIA:")==0||line.find("#EXT-X-I-FRAME-STREAM-INF:")==0);if(local.empty())return {};line.replace(first,last-first,local);pos=first+local.size()+1;}}
        if(line.find("#EXT-X-STREAM-INF:")==0)variant=true;
        out+=line+"\n";if(out.size()>manifest_limit)return {};
    }
    return out;
}
static bool send_all(int fd,const std::string& data){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    size_t sent=0;
    while(sent<data.size() && std::chrono::steady_clock::now()<deadline){
        const auto n=send(fd,data.data()+sent,data.size()-sent,MSG_NOSIGNAL);
        if(n<=0)return false;
        sent+=size_t(n);
    }
    return sent==data.size();
}
void HlsRelay::serve(){
    while(running_){
        int fd=accept(listener_,nullptr,nullptr);if(fd<0)break;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if(!running_){close(fd);break;}
            clients_.push_back(fd);
        }
        timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        std::string request;char buffer[1024];
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(request.find("\r\n\r\n")==std::string::npos&&request.size()<4096&&
              std::chrono::steady_clock::now()<deadline){auto n=recv(fd,buffer,std::min(sizeof(buffer),4096-request.size()),0);if(n<=0)break;request.append(buffer,size_t(n));}
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
        if(response.status!=200){response.data.clear();response.type="application/octet-stream";}
        const char *label=method=="GET"?"GET":method=="HEAD"?"HEAD":"other";
        if(diagnostic_records_.load()<64&&diagnostic_records_.fetch_add(1)<64)
            fprintf(stderr,"[hls-relay] request=%s resource=%ld manifest=%d http=%d bytes=%zu\n",label,resource,int(manifest),response.status,response.data.size());
        std::string wire="HTTP/1.1 "+std::to_string(response.status)+" Result\r\nConnection: close\r\nContent-Type: "+(response.type.empty()?"application/octet-stream":response.type)+"\r\nContent-Length: "+std::to_string(response.data.size())+"\r\n\r\n";
        const bool headers_sent=send_all(fd,wire);
        if(method=="GET" && headers_sent && send_all(fd,response.data) &&
           response.status==200 && response.type!="application/vnd.apple.mpegurl" && !response.data.empty())++media_gets;
        {std::lock_guard<std::mutex> lock(clients_mutex_);clients_.erase(std::remove(clients_.begin(),clients_.end(),fd),clients_.end());close(fd);}
    }
}
bool HlsRelay::begin(bool allow_configured_lan) {
    stop();
    diagnostic_records_=0;
    bytes=requests=failures=incoming_requests=media_gets=0;
    static std::once_flag init;
    static CURLcode initialized=CURLE_FAILED_INIT;
    std::call_once(init,[]{initialized=curl_global_init(CURL_GLOBAL_DEFAULT);});
    if(initialized!=CURLE_OK){trace("curl-initialize",int(initialized));return false;}
    const char *lan=allow_configured_lan?getenv("UXAIR_DIRECT_LAN"):nullptr;
    allowed_lan_=lan?lan:"";
    trace("start-lan-exception",allowed_lan_.empty()?0:1);
    running_=true;
    return true;
}
std::string HlsRelay::start(const std::string& uri,bool allow_configured_lan){
    if(!direct_uri_valid(uri)||!begin(allow_configured_lan))return {};
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
    if(cookie_share_) { curl_share_cleanup(cookie_share_);cookie_share_=nullptr; }
    request_origin_.clear();generation_=0;
    fd=listener_.exchange(-1);if(fd>=0)close(fd);
    if(active)fprintf(stderr,"[hls-relay] stage=stopped native_requests=%llu upstream_fetches=%llu bytes=%llu failures=%llu\n",
        (unsigned long long)incoming_requests.load(),(unsigned long long)requests.load(),(unsigned long long)bytes.load(),(unsigned long long)failures.load());
    std::lock_guard<std::mutex> lock(mutex_);resources_.clear();manifests_.clear();
}
