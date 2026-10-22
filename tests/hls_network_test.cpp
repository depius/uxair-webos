#include "hls_relay.h"
#include <cassert>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <curl/curl.h>
static size_t receive(char *p,size_t s,size_t n,void *out){static_cast<std::string*>(out)->append(p,s*n);return s*n;}
struct Response {long code=0;std::string data,type;curl_off_t length=0;};
static Response get(const std::string& uri,bool head=false){
 Response out;CURL *c=curl_easy_init();assert(c);curl_easy_setopt(c,CURLOPT_URL,uri.c_str());curl_easy_setopt(c,CURLOPT_PROXY,"");
 curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,&out.data);curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,3000L);curl_easy_setopt(c,CURLOPT_NOBODY,head?1L:0L);
 assert(curl_easy_perform(c)==CURLE_OK);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&out.code);char *type=nullptr;curl_easy_getinfo(c,CURLINFO_CONTENT_TYPE,&type);if(type)out.type=type;
 curl_easy_getinfo(c,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&out.length);curl_easy_cleanup(c);return out;
}
static std::string first_uri(const std::string& manifest){const auto first=manifest.find("http://127.0.0.1:");assert(first!=std::string::npos);return manifest.substr(first,manifest.find_first_of("\n\r\"",first)-first);}
int main(int argc,char **argv){assert(argc==3);std::string base=argv[1];HlsRelay relay;auto local=relay.start(base+"/ok");assert(!local.empty());
 CURL *curl=curl_easy_init();assert(curl);std::string output;curl_easy_setopt(curl,CURLOPT_URL,local.c_str());curl_easy_setopt(curl,CURLOPT_PROXY,"");curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&output);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,3000L);assert(curl_easy_perform(curl)==CURLE_OK);long code=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);assert(code==200&&output.find("#EXTM3U")==0);curl_easy_cleanup(curl);
 // Actual native-facing master -> variant -> TS requests, including HEAD probing.
 relay.stop();local=relay.start(base+"/master.m3u8?token=PRIVATE");assert(local.find(".m3u8")!=std::string::npos);
 auto head=get(local,true);assert(head.code==200&&head.data.empty()&&head.length>0&&head.type=="application/vnd.apple.mpegurl");
 auto master=get(local);assert(master.code==200&&master.data.find("PRIVATE")==std::string::npos);
 auto variant_uri=first_uri(master.data);assert(variant_uri.find(".m3u8")!=std::string::npos);auto media=get(variant_uri);assert(media.code==200);
 auto segment_uri=first_uri(media.data);assert(segment_uri.find(".ts")!=std::string::npos);auto segment=get(segment_uri);
 assert(segment.code==200&&segment.data==std::string(188,char(0x47))&&segment.type=="video/mp2t");
 assert(get(segment_uri,true).length==188);assert(relay.incoming_requests==5&&relay.requests==5&&relay.bytes>188);
 assert(get(local+"?bad=1").code==404);relay.stop();
 local=relay.start(base+"/invalid.m3u8");assert(get(local).code==422&&relay.failures==1);relay.stop();local=relay.start(base+"/ok");assert(get(local).code==200);
 assert(relay.fetch(base+"/ok",1024).status==200);
 assert(relay.fetch(base+"/redirect",1024).status==200);
 assert(relay.fetch(base+"/loop",1024).status!=200);
 assert(relay.fetch(base+"/denied",1024).status!=200);
 assert(relay.fetch(base+"/missing.ts",1024).status==404);
 assert(relay.fetch(base+"/large",128).status!=200);
 assert(relay.fetch(argv[2],1024).status!=200); // untrusted local TLS certificate
 assert(relay.fetch(base+"/slow",1024).status==504);
 assert(relay.fetch(base+"/ok",1024).status==200);relay.stop();
 puts("PASS: actual HTTP redirects, SSRF policy, byte limits, 404, TLS failure, timeout and recovery");}
