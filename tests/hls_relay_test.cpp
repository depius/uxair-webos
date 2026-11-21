#include "hls_relay.h"
#include <cassert>
#include <arpa/inet.h>
#include <cstdio>
int main(){
 HlsRelay relay;assert(HlsRelay::resolve("https://example.com/a/master.m3u8","../segments/s.ts")=="https://example.com/segments/s.ts");
 assert(HlsRelay::resolve("https://example.com/a","file:///etc/passwd").empty());
 assert(!relay.address_allowed(inet_addr("127.0.0.1"))&&!relay.address_allowed(inet_addr("10.0.0.1"))&&!relay.address_allowed(inet_addr("169.254.169.254")));
 assert(relay.address_allowed(inet_addr("8.8.8.8")));
 auto uri=relay.start("https://example.com/master.m3u8");assert(!uri.empty()&&uri.find(".m3u8")!=std::string::npos);
 auto manifest=relay.rewrite("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:2,\nsegment.m4s\n#EXT-X-ENDLIST\n","https://example.com/a/master.m3u8");
 assert(manifest.find("http://127.0.0.1:")!=std::string::npos&&manifest.find("segment.m4s")==std::string::npos);
 assert(manifest.find(".mp4")!=std::string::npos&&manifest.find(".m4s")!=std::string::npos);
 auto master=relay.rewrite("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,URI=\"audio.m3u8?token=PRIVATE\"\n#EXT-X-STREAM-INF:BANDWIDTH=900000\nvideo.m3u8?token=PRIVATE\n","https://example.com/master.m3u8");
 assert(master.find(".m3u8")!=std::string::npos&&master.find("PRIVATE")==std::string::npos);
 assert(relay.rewrite("not a playlist","https://example.com/a").empty());
 assert(relay.rewrite("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"key\"\n","https://example.com/a").empty());
 assert(relay.rewrite("#EXTM3U\nfile:///etc/passwd\n","https://example.com/a").empty());
 assert(relay.rewrite(std::string(262145,'x'),"https://example.com/a").empty());
 auto blocked=relay.fetch("http://127.0.0.1:1/",1024);assert(blocked.status!=200);relay.stop();
 puts("PASS: relative HLS/fMP4 rewriting, DRM rejection, address policy and manifest limits");
}
