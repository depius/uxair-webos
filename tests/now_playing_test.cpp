#include "now_playing.h"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>

static std::vector<uint8_t> item(const char *tag,const std::vector<uint8_t>& value) {
    std::vector<uint8_t> out(tag,tag+4);
    for(int i=24;i>=0;i-=8)out.push_back(uint8_t(value.size()>>i));
    out.insert(out.end(),value.begin(),value.end());return out;
}
static void be32(std::vector<uint8_t>& b,size_t at,unsigned n) {
    for(int i=3;i>=0;--i){b[at+i]=uint8_t(n);n>>=8;}
}
int main() {
    NowPlaying value;value.title="unchanged";
    const auto title=item("minm",{'T','i','t','l','e'});
    const auto wrapped=item("mlit",title);
    assert(parse_now_playing(wrapped.data(),wrapped.size(),value)&&value.title=="Title");
    for(size_t n=1;n<wrapped.size();++n) {
        value.title="unchanged";
        assert(!parse_now_playing(wrapped.data(),n,value)&&value.title=="unchanged");
    }
    auto deep=title;for(int i=0;i<4;++i)deep=item("mlit",deep);
    assert(parse_now_playing(deep.data(),deep.size(),value));
    deep=item("mlit",deep);assert(!parse_now_playing(deep.data(),deep.size(),value));
    auto huge=item("minm",std::vector<uint8_t>(1025,'x'));
    assert(!parse_now_playing(huge.data(),huge.size(),value));
    assert(!parse_now_playing(title.data(),65537,value));
    auto corrupt=title;be32(corrupt,4,0xffffffff);
    assert(!parse_now_playing(corrupt.data(),corrupt.size(),value));
    std::vector<uint8_t> many;
    for(int i=0;i<257;++i){auto x=item("noop",{});many.insert(many.end(),x.begin(),x.end());}
    assert(!parse_now_playing(many.data(),many.size(),value));
    auto controls=item("minm",{'a',0,'\n','b'});
    assert(parse_now_playing(controls.data(),controls.size(),value)&&value.title=="a  b");
    assert(parse_now_playing(nullptr,0,value)&&value.title.empty());
    assert(!parse_now_playing(nullptr,1,value));

    std::vector<uint8_t> png(33,0);
    const uint8_t signature[]={137,80,78,71,13,10,26,10};
    memcpy(png.data(),signature,8);be32(png,8,13);memcpy(png.data()+12,"IHDR",4);
    be32(png,16,1024);be32(png,20,1024);assert(artwork_supported(png.data(),png.size()));
    be32(png,16,1025);assert(!artwork_supported(png.data(),png.size()));
    be32(png,16,0xffffffff);assert(!artwork_supported(png.data(),png.size()));
    assert(!artwork_supported(nullptr,0));
    assert(!artwork_supported(png.data(),kArtworkBytes+1));
    std::vector<uint8_t> jpeg={255,216,255,192,0,11,8,0,32,0,32,1,1,17,0};
    assert(artwork_supported(jpeg.data(),jpeg.size()));
    for(size_t n=0;n<jpeg.size();++n)assert(!artwork_supported(jpeg.data(),n));
    jpeg[7]=255;assert(!artwork_supported(jpeg.data(),jpeg.size()));

    // Readers own the immutable bytes even after replacement or disconnect.
    std::mutex mutex;NowPlaying shared;
    shared.cover=std::make_shared<const std::vector<uint8_t>>(1024,uint8_t(42));
    NowPlaying snapshot=shared;assert(snapshot.cover->data()==shared.cover->data());
    std::weak_ptr<const std::vector<uint8_t>> retired=snapshot.cover;
    std::thread writer([&] {
        for(int i=0;i<10000;++i) {
            auto art=std::make_shared<const std::vector<uint8_t>>(1024,uint8_t(i));
            std::lock_guard<std::mutex> lock(mutex);shared.cover=art;
            if(i%2)shared=NowPlaying();
        }
    });
    for(int i=0;i<10000;++i) {
        NowPlaying copy;{std::lock_guard<std::mutex> lock(mutex);copy=shared;}
        if(copy.cover)assert(copy.cover->size()==1024 && copy.cover->front()==copy.cover->back());
    }
    writer.join();assert(snapshot.cover->front()==42);snapshot=NowPlaying();assert(retired.expired());
    // Deterministic malformed-input corpus, also useful under ASan/UBSan.
    unsigned rng=1;
    for(unsigned i=0;i<10000;++i) {
        std::vector<uint8_t> data(i%512);
        for(auto& byte:data){rng=rng*1664525+1013904223;byte=uint8_t(rng>>24);}
        parse_now_playing(data.data(),data.size(),value);artwork_supported(data.data(),data.size());
    }
    puts("PASS: bounded DMAP/artwork, malformed corpus, immutable snapshots, replacement/removal and lifetime");
}
