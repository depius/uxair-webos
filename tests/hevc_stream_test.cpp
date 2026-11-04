#include "hevc_stream.h"
#include "hevc_sample.h"
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

static std::vector<uint8_t> read(const char *path) {
    std::ifstream f(path,std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}
static bool set_first_vcl_type(std::vector<uint8_t>& au, unsigned type) {
    for(size_t i=0;i+5<au.size();++i) {
        size_t header=0;
        if(au[i]==0 && au[i+1]==0 && au[i+2]==1) header=i+3;
        else if(i+4<au.size() && au[i]==0 && au[i+1]==0 && au[i+2]==0 && au[i+3]==1) header=i+4;
        if(!header || header+1>=au.size()) continue;
        unsigned current=(au[header]>>1)&63;
        if(current<=31) {
            au[header]=uint8_t((au[header]&0x81)|(type<<1));
            return true;
        }
        i=header;
    }
    return false;
}
int main(int argc,char **argv) {
    assert(argc==5);
    HevcStream stream;
    for(int test=0;test<2;++test) {
        std::vector<uint8_t> bytes=read(argv[test+1]);
        hevc_probe::Sample sample;std::string error;
        const int width=test?3840:1920,height=test?2160:1080;
        assert(hevc_probe::inspect(bytes.data(),bytes.size(),width,height,sample,error));
        assert(sample.units.size()==30);
        for(size_t i=0;i<sample.units.size();++i) {
            const auto& unit=sample.units[i];
            HevcFrameInfo frame=stream.inspect(bytes.data()+unit.offset,unit.size);
            assert(frame.valid && frame.width==width && frame.height==height);
            assert(frame.profile==1 && frame.bit_depth==8);
            if(i==0) assert(frame.has_aud);
            if(i==0) assert(frame.keyframe && frame.configuration_changed);
        }
    }
    stream.reset();
    std::vector<uint8_t> portrait=read(argv[3]);
    assert(!portrait.empty());
    HevcFrameInfo portrait_frame=stream.inspect(portrait.data(),portrait.size());
    assert(portrait_frame.valid && portrait_frame.keyframe && portrait_frame.configuration_changed);
    assert(portrait_frame.width==996 && portrait_frame.height==2160);
    HevcFrameInfo same_portrait=stream.inspect(portrait.data(),portrait.size());
    assert(same_portrait.valid && !same_portrait.configuration_changed);
    std::vector<uint8_t> uhd=read(argv[2]);
    hevc_probe::Sample uhd_sample;std::string uhd_error;
    assert(hevc_probe::inspect(uhd.data(),uhd.size(),3840,2160,uhd_sample,uhd_error));
    const auto& uhd_first=uhd_sample.units[0];
    const auto& uhd_second=uhd_sample.units[1];
    std::vector<uint8_t> changed_nonkey(uhd.data()+uhd_first.offset,uhd.data()+uhd_first.offset+uhd_first.size);
    assert(set_first_vcl_type(changed_nonkey,1)); // Parameter sets change before the random-access frame.
    HevcFrameInfo changed=stream.inspect(changed_nonkey.data(),changed_nonkey.size());
    assert(changed.valid && !changed.keyframe && changed.configuration_changed);
    assert(changed.width==3840 && changed.height==2160);
    std::vector<uint8_t> keyframe_without_sets(uhd.data()+uhd_second.offset,uhd.data()+uhd_second.offset+uhd_second.size);
    assert(set_first_vcl_type(keyframe_without_sets,19)); // Simulate next random-access frame with no repeated headers.
    HevcFrameInfo delta=stream.inspect(keyframe_without_sets.data(),keyframe_without_sets.size());
    assert(delta.valid && delta.keyframe && !delta.configuration_changed);
    bool restored=false;
    assert(stream.prepend_missing_parameter_sets(keyframe_without_sets,delta,restored) && restored);
    HevcFrameInfo restored_info=stream.inspect(keyframe_without_sets.data(),keyframe_without_sets.size());
    assert(restored_info.valid && restored_info.keyframe && restored_info.has_vps && restored_info.has_sps && restored_info.has_pps);
    assert(!restored_info.configuration_changed && supported_hevc_mirror_profile(restored_info));
    bool unchanged=false;
    assert(stream.prepend_missing_parameter_sets(keyframe_without_sets,restored_info,unchanged) && !unchanged);
    std::vector<uint8_t> main10=read(argv[4]);
    assert(!main10.empty());
    HevcStream main10_stream;
    HevcFrameInfo main10_frame=main10_stream.inspect(main10.data(),main10.size());
    assert(main10_frame.valid && main10_frame.profile==2 && main10_frame.bit_depth==10);
    assert(!supported_hevc_mirror_profile(main10_frame));
    stream.reset();
    std::vector<uint8_t> bytes=read(argv[1]);
    hevc_probe::Sample sample;std::string error;
    assert(hevc_probe::inspect(bytes.data(),bytes.size(),1920,1080,sample,error));
    const auto& second=sample.units[1];
    assert(!stream.inspect(bytes.data()+second.offset,second.size).valid); // no VPS/SPS/PPS after reconnect
    const auto& first=sample.units[0];
    // Header-only callbacks arrive separately after a reconnect. Cache all three,
    // then admit a keyframe without repeated headers and prefix it for Starfish.
    std::vector<std::vector<uint8_t>> headers;
    const size_t end_first=first.offset+first.size;
    auto find_start=[&](size_t from,size_t& prefix)->size_t {
        for(size_t i=from;i+3<=end_first;++i) {
            if(bytes[i]||bytes[i+1])continue;
            if(bytes[i+2]==1){prefix=3;return i;}
            if(i+4<=end_first&&!bytes[i+2]&&bytes[i+3]==1){prefix=4;return i;}
        }
        return end_first;
    };
    size_t prefix=0,pos=find_start(first.offset,prefix);
    while(pos<end_first) {
        const size_t begin=pos+prefix;size_t next_prefix=0;
        const size_t end=find_start(begin,next_prefix);
        const unsigned type=(bytes[begin]>>1)&63;
        if(type>=32 && type<=34)headers.emplace_back(bytes.begin()+pos,bytes.begin()+end);
        pos=end;prefix=next_prefix;
    }
    assert(headers.size()==3);
    stream.reset();
    for(const auto& header:headers) {
        const auto info=stream.inspect(header.data(),header.size());
        assert(!info.valid && info.waiting_configuration);
    }
    std::vector<uint8_t> random_access(bytes.begin()+second.offset,bytes.begin()+second.offset+second.size);
    assert(set_first_vcl_type(random_access,19));
    auto cached=stream.inspect(random_access.data(),random_access.size());
    assert(cached.valid&&cached.keyframe&&cached.configuration_changed&&cached.configuration_complete);
    bool prefixed=false;assert(stream.prepend_missing_parameter_sets(random_access,cached,prefixed)&&prefixed);
    HevcStream fresh;assert(fresh.inspect(random_access.data(),random_access.size()).valid);
    stream.reset();
    auto missing=stream.inspect(bytes.data()+second.offset,second.size);
    assert(!missing.valid&&missing.waiting_configuration&&!missing.configuration_complete);
    const uint8_t broken[]={0xff,0,0,1,0x26,1,0x80};
    auto rejected=stream.inspect(broken,sizeof(broken));assert(!rejected.valid&&!rejected.waiting_configuration);
    assert(!hevc_configuration_wait_expired(1,15000000001LL));
    assert(hevc_configuration_wait_expired(1,15000000002LL));

    assert(!stream.inspect(bytes.data()+first.offset,hevc_probe::kMaxAccessUnit+1).valid);
    assert(!stream.inspect(nullptr,first.size).valid);
    std::vector<uint8_t> malformed(bytes.begin()+first.offset,bytes.begin()+first.offset+first.size);
    malformed[0]=0xff;
    assert(!stream.inspect(malformed.data(),malformed.size()).valid);
    malformed.assign(bytes.begin()+first.offset,bytes.begin()+first.offset+first.size);
    malformed[5]=0x80;
    assert(!stream.inspect(malformed.data(),malformed.size()).valid);
    std::vector<uint8_t> no_aud={0,0,0,1,0x26,0x01,0x80};
    assert(ensure_hevc_aud(no_aud,false));
    assert(no_aud.size()==14 && no_aud[4]==0x46);
    assert(ensure_hevc_aud(no_aud,true) && no_aud.size()==14);
    std::vector<uint8_t> too_big(hevc_probe::kMaxAccessUnit,0);
    assert(!ensure_hevc_aud(too_big,false));
}
