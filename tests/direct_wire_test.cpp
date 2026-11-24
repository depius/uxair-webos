#include "receiver.h"
#include "capabilities.h"
extern "C" {
#include "http_request.h"
#include <plist/plist.h>
}
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {
const char *const owner="wire-session-private";
const std::string uri="https://media.example/vod/master.m3u8?token=WIRE-PRIVATE";

std::string encode(plist_t value) {
    char *data=nullptr;
    uint32_t size=0;
    plist_to_bin(value,&data,&size);
    assert(data && size);
    std::string result(data,size);
    free(data);
    plist_free(value);
    return result;
}

struct Response {
    std::string headers, body;
    void expect(const char *protocol, int code) const {
        const std::string prefix=std::string(protocol)+" "+std::to_string(code)+" ";
        if(headers.compare(0,prefix.size(),prefix)!=0)
            fprintf(stderr,"Expected %s but got %s\n",prefix.c_str(),headers.substr(0,50).c_str());
        assert(headers.compare(0,prefix.size(),prefix)==0);
    }
};

Response exchange(unsigned short port, const std::string& line, const std::string& headers="",
                  const std::string& body="", size_t split=0) {
    const int fd=socket(AF_INET,SOCK_STREAM,0);
    assert(fd>=0);
    const timeval timeout={3,0};
    assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    sockaddr_in address={};
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons(port);
    assert(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)));
    std::string request=line+"\r\nHost: localhost\r\n"+headers+
        "Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    auto send_part=[&](size_t start,size_t length) {
        while(length) {
            const auto n=send(fd,request.data()+start,length,MSG_NOSIGNAL);
            assert(n>0);
            start+=size_t(n);
            length-=size_t(n);
        }
    };
    if(split) {
        send_part(0,split);
        usleep(25000);
    }
    send_part(split,request.size()-split);
    std::string buffer;
    char chunk[4096];
    size_t end=std::string::npos, length=0;
    do {
        const auto n=recv(fd,chunk,sizeof(chunk),0);
        assert(n>0);
        buffer.append(chunk,size_t(n));
        assert(buffer.size()<65536);
        end=buffer.find("\r\n\r\n");
        if(end==std::string::npos)continue;
        const auto header=buffer.find("Content-Length:");
        if(header<end)length=size_t(std::strtoul(buffer.c_str()+header+15,nullptr,10));
    } while(end==std::string::npos || buffer.size()<end+4+length);
    close(fd);
    return {buffer.substr(0,end),buffer.substr(end+4,length)};
}

plist_t decode(const Response& response) {
    plist_t root=nullptr;
    if(response.body.compare(0,8,"bplist00")==0)
        plist_from_bin(response.body.data(),uint32_t(response.body.size()),&root);
    else plist_from_xml(response.body.data(),uint32_t(response.body.size()),&root);
    assert(PLIST_IS_DICT(root));
    return root;
}

uint64_t feature_mask(const Response& response) {
    auto root=decode(response);
    uint64_t mask=0;
    plist_get_uint_val(plist_dict_get_item(root,"features"),&mask);
    plist_free(root);
    return mask;
}

void split_request_line() {
    for (const char* protocol: {"HTTP/1.1","RTSP/1.0"}) {
        auto *request=http_request_init();
        const std::string line=std::string("GET /playback-info ")+protocol+
            "\r\nx-apple-session-id: wire-session-private\r\n\r\n";
        for(char byte:line) http_request_add_data(request,&byte,1);
        assert(http_request_is_complete(request) && !http_request_has_error(request));
        assert(!strcmp(http_request_get_protocol(request),protocol));
        assert(!strcmp(http_request_get_url(request),"/playback-info"));
        assert(!strcmp(http_request_get_header(request,"X-Apple-Session-ID"),owner));
        http_request_destroy(request);
    }
}
}

int main() {
    split_request_line();
    char path[]="/tmp/uxair-wire-XXXXXX";
    assert(mkdtemp(path));
    Receiver receiver;
    Settings settings;
    unsetenv("UXAIR_DIRECT_VIDEO");
    assert(receiver.start(path,settings));
    const auto port=raop_get_port(receiver.raop);
    assert(port);
    const uint64_t expected=UXPLAY_PINNED_FEATURES | 17;
    assert(dnssd_get_airplay_features(receiver.dnssd)==expected);
    assert(feature_mask(exchange(port,"GET /info HTTP/1.1"))==expected);
    assert(feature_mask(exchange(port,"GET /server-info RTSP/1.0","cseq: 12\r\n"))==expected);

    const auto info=exchange(port,"GET /info?txtAirPlay?txtRAOP RTSP/1.0","cseq: 7\r\n");
    info.expect("RTSP/1.0",200);
    assert(info.headers.find("CSeq: 7")!=std::string::npos);
    auto txt=decode(info);
    auto txt_airplay=plist_dict_get_item(txt,"txtAirPlay");
    assert(PLIST_IS_DATA(txt_airplay) && PLIST_IS_DATA(plist_dict_get_item(txt,"txtRAOP")));
    char *data=nullptr;
    uint64_t bytes=0;
    plist_get_data_val(txt_airplay,&data,&bytes);
    assert(data && std::string(data,size_t(bytes)).find("features=0x5A7FFEF7,0x0")!=std::string::npos);
    free(data);
    plist_free(txt);

    plist_t commands=plist_new_dict();
    plist_dict_set_item(commands,"type",plist_new_string("updateMRSupportedCommands"));
    plist_dict_set_item(commands,"params",plist_new_string("WIRE-PRIVATE"));
    const std::string cseq="cseq: 21\r\nx-apple-session-id: "+std::string(owner)+"\r\n";
    auto control=exchange(port,"POST /command RTSP/1.0",cseq+"Content-Type: application/octet-stream\r\n",encode(commands));
    control.expect("RTSP/1.0",200); // pinned upstream response, no video handler
    DirectCommand pending;
    assert(!receiver.direct.take(pending) && receiver.direct.snapshot().state==DirectState::Idle);
    exchange(port,"POST /unknown-WIRE-PRIVATE HTTP/1.1").expect("HTTP/1.1",501);
    // Wait for the RTSP control connection to be reaped before the pairing request.
    usleep(50000);

    plist_t play=plist_new_dict();
    plist_dict_set_item(play,"Content-Location",plist_new_string(uri.c_str()));
    plist_dict_set_item(play,"Start-Position-Seconds",plist_new_real(0.5));
    const auto descriptor=encode(play);
    const std::string type="content-type: application/octet-stream\r\n";
    const std::string session="x-apple-session-id: "+std::string(owner)+"\r\n";
    exchange(port,"POST /play HTTP/1.1",type,descriptor).expect("HTTP/1.1",400);
    exchange(port,"POST /play HTTP/1.1",session+type,"bplist00bad").expect("HTTP/1.1",400);
    receiver.has_audio=true;
    exchange(port,"POST /play HTTP/1.1",session+type,descriptor).expect("HTTP/1.1",409);
    assert(!receiver.direct.take(pending));
    receiver.has_audio=false;
    receiver.mirror=true;
    exchange(port,"POST /play HTTP/1.1",session+type,descriptor).expect("HTTP/1.1",409);
    receiver.mirror=false;
    unsetenv("UXAIR_DIRECT_VIDEO");
    const auto accepted=exchange(port,"POST /play RTSP/1.0",cseq+type,descriptor,11);
    accepted.expect("RTSP/1.0",200);
    assert(accepted.headers.find("CSeq: 21")!=std::string::npos);
    auto feedback=decode(exchange(port,"GET /playback-info HTTP/1.1",session));
    uint8_t ready=1;
    plist_get_bool_val(plist_dict_get_item(feedback,"readyToPlay"),&ready);
    assert(!ready);
    plist_free(feedback);
    assert(receiver.direct.take(pending) && pending.kind==DirectCommandKind::Play &&
        pending.uri==uri && pending.position_seconds && pending.position==0.5);
    assert(receiver.reserve_direct());
    DirectSnapshot snapshot;
    snapshot.state=DirectState::Error;
    snapshot.error=DirectError::FetchFailed;
    receiver.direct.publish(snapshot);
    receiver.release_direct(true);
    exchange(port,"GET /playback-info RTSP/1.0",cseq).expect("RTSP/1.0",503);

    exchange(port,"POST /play HTTP/1.1",session+"Content-Type: text/parameters\r\n",
             "Content-Location: "+uri+"\r\n").expect("HTTP/1.1",200);
    exchange(port,"POST /stop RTSP/1.0",cseq).expect("RTSP/1.0",200);
    assert(receiver.direct.take(pending) && pending.kind==DirectCommandKind::Stop);
    receiver.direct.reset();
    exchange(port,"POST /pair-pin-start RTSP/1.0","CSeq: 22\r\n").expect("RTSP/1.0",200);
    assert(!receiver.pin().empty() && !receiver.direct.take(pending));

    receiver.stop();
    assert(!unlink((std::string(path)+"/server.key").c_str()));
    assert(!unlink((std::string(path)+"/device.id").c_str()));
    assert(!rmdir(path));
    assert(!setenv("UXAIR_DIRECT_VIDEO","0",1));
    char disabled_path[]="/tmp/uxair-wire-disabled-XXXXXX";
    assert(mkdtemp(disabled_path));
    Receiver disabled;
    assert(disabled.start(disabled_path,settings));
    assert(dnssd_get_airplay_features(disabled.dnssd)==UXPLAY_PINNED_FEATURES);
    assert(feature_mask(exchange(raop_get_port(disabled.raop),"GET /info HTTP/1.1"))==UXPLAY_PINNED_FEATURES);
    disabled.stop();
    assert(!unlink((std::string(disabled_path)+"/server.key").c_str()));
    assert(!unlink((std::string(disabled_path)+"/device.id").c_str()));
    assert(!rmdir(disabled_path));
    puts("PASS: pinned TCP dispatcher, discovery identity, fragmented requests, RAOP admission, Direct Video and safe playback feedback");
}
