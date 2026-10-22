#include "hls_relay.h"
#include <cassert>
#include <cstdlib>
#include <string>
#include <cstdio>
int main(int argc,char **argv){assert(argc==3);std::string base=argv[1];HlsRelay relay;assert(!relay.start(base+"/ok").empty());
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
