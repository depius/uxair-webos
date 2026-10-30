#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <utility>

// Network metadata is bounded before copying or decoding it.
struct NowPlaying {
    std::string title, artist, album, source;
    std::shared_ptr<const std::vector<uint8_t>> cover;
    uint32_t start=0, current=0, end=0;
    int64_t progress_ns=0, cover_ns=0, track_ns=0;
    uint64_t generation=0, cover_generation=0;
};
static const size_t kMetadataBytes=65536, kMetadataTextBytes=1024;
static const size_t kArtworkBytes=2*1024*1024, kArtworkPixels=1024*1024;
inline uint32_t metadata_be32(const uint8_t *p) {
    return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
}
inline bool artwork_dimensions(unsigned w,unsigned h) {
    return w && h && w<=2048 && h<=2048 && uint64_t(w)*h<=kArtworkPixels;
}
inline bool artwork_supported(const uint8_t *p,size_t n) {
    if(!p || n>kArtworkBytes)return false;
    const uint8_t png[]={137,80,78,71,13,10,26,10};
    if(n>=33 && !memcmp(p,png,8))
        return metadata_be32(p+8)==13 && !memcmp(p+12,"IHDR",4) &&
            artwork_dimensions(metadata_be32(p+16),metadata_be32(p+20));
    if(n<4 || p[0]!=0xff || p[1]!=0xd8)return false;
    for(size_t at=2;at<n;) {
        if(p[at++]!=0xff)return false;
        while(at<n && p[at]==0xff)++at;
        if(at==n)return false;
        unsigned marker=p[at++];
        if(marker==0xda || marker==0xd9)return false;
        if(marker==1 || (marker>=0xd0 && marker<=0xd7))continue;
        if(n-at<2)return false;
        size_t length=size_t(p[at])*256+p[at+1];
        if(length<2 || length>n-at)return false;
        // Baseline/extended/progressive JPEG only; no unrelated IMG codecs.
        if(marker>=0xc0 && marker<=0xc2) {
            if(length<8)return false;
            return artwork_dimensions(unsigned(p[at+5])*256+p[at+6],
                                      unsigned(p[at+3])*256+p[at+4]);
        }
        at+=length;
    }
    return false;
}
inline bool dmap_bounded(const uint8_t *p,size_t n,NowPlaying& out,unsigned depth,unsigned& items) {
    if(depth>4)return false;
    for(size_t at=0;at<n;) {
        if(n-at<8 || ++items>256)return false;
        const uint8_t *tag=p+at;size_t length=metadata_be32(p+at+4);at+=8;
        if(length>n-at)return false;
        if(!memcmp(tag,"mlit",4)) {
            if(!dmap_bounded(p+at,length,out,depth+1,items))return false;
        } else {
            std::string *value=!memcmp(tag,"minm",4)?&out.title:
                !memcmp(tag,"asar",4)?&out.artist:!memcmp(tag,"asal",4)?&out.album:nullptr;
            if(value) {
                if(length>kMetadataTextBytes)return false;
                value->assign(reinterpret_cast<const char*>(p+at),length);
                for(char& c:*value)if(static_cast<unsigned char>(c)<32 || c==127)c=' ';
            }
        }
        at+=length;
    }
    return true;
}
inline bool parse_now_playing(const void *data,size_t length,NowPlaying& out) {
    if(length>kMetadataBytes || (!data && length))return false;
    NowPlaying parsed;unsigned items=0;
    if(!dmap_bounded(static_cast<const uint8_t*>(data),length,parsed,0,items))return false;
    out=std::move(parsed);return true;
}
