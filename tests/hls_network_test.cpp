#include "hls_relay.h"
#include <cassert>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <curl/curl.h>
static size_t receive(char *p,size_t s,size_t n,void *out){static_cast<std::string*>(out)->append(p,s*n);return s*n;}
int main(int argc,char **argv){assert(argc==3);std::string base=argv[1];HlsRelay relay;auto local=relay.start(base+"/ok");assert(!local.empty());
 CURL *curl=curl_easy_init();assert(curl);std::string output;curl_easy_setopt(curl,CURLOPT_URL,local.c_str());curl_easy_setopt(curl,CURLOPT_PROXY,"");curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&output);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,3000L);assert(curl_easy_perform(curl)==CURLE_OK);long code=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);assert(code==200&&output.find("#EXTM3U")==0);curl_easy_cleanup(curl);
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
