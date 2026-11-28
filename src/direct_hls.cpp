#include "direct_hls.h"
#include "audio.h"
#include "h264.h"
#include "hevc_stream.h"
#include "hevc_sample.h"
#include "hls_relay.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/log.h>
#include <libavutil/intreadwrite.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <map>

namespace {
constexpr int64_t second = 1000000000;
constexpr size_t max_segment_bytes = 8 * 1024 * 1024;
constexpr size_t max_packet_bytes = hevc_probe::kMaxAccessUnit;
// Allow rounding to ordinary fragmented MP4 timescales at 30 fps.
constexpr int64_t hevc_uhd_min_frame_ns = int64_t(second / direct_hevc_uhd_max_fps) - 84000;
constexpr size_t max_segment_packets = 1152;
thread_local unsigned* timestamp_warnings = nullptr;

void hls_ffmpeg_log(void* object, int level, const char* format, va_list args) {
    if (timestamp_warnings && level == AV_LOG_WARNING &&
        std::strncmp(format, "Invalid timestamps stream=", sizeof("Invalid timestamps stream=")-1) == 0) {
        ++*timestamp_warnings;
        return;
    }
    av_log_default_callback(object, level, format, args);
}

struct TimestampWarnings {
    unsigned* previous = timestamp_warnings;
    explicit TimestampWarnings(unsigned& count) { timestamp_warnings = &count; }
    ~TimestampWarnings() { timestamp_warnings = previous; }
};

bool starts(const std::string& value, const char* prefix) {
    return value.compare(0, std::strlen(prefix), prefix) == 0;
}

bool decimal(const std::string& value, double& number) {
    if (value.empty() || value.find_first_not_of("0123456789.") != std::string::npos) return false;
    char* end = nullptr;
    number = std::strtod(value.c_str(), &end);
    return end == value.c_str() + value.size() && std::isfinite(number);
}

std::string attribute(const std::string& line, const char* key) {
    const auto colon = line.find(':');
    size_t pos = colon + 1;
    while (pos < line.size()) {
        const auto equal = line.find('=', pos);
        if (equal == std::string::npos) break;
        const bool quoted = equal + 1 < line.size() && line[equal + 1] == '"';
        const size_t first = equal + (quoted ? 2 : 1);
        const auto end = line.find(quoted ? '"' : ',', first);
        if (quoted && end == std::string::npos) break;
        if (line.compare(pos, equal - pos, key) == 0) return line.substr(first, end - first);
        if (end == std::string::npos) break;
        pos = end + (quoted ? 2 : 1);
    }
    return {};
}

double variant_bandwidth(const std::string& tag, unsigned& preference) {
    const auto codecs = attribute(tag, "CODECS");
    bool hevc_possible = codecs.empty();
    preference=2; // Undeclared codec, confirm from the initialization data.
    const auto range=attribute(tag,"VIDEO-RANGE");
    if(!range.empty() && range!="SDR") return 0;
    if (!codecs.empty()) {
        const auto comma = codecs.find(',');
        if (comma != std::string::npos && codecs.find(',', comma + 1) != std::string::npos) return 0;
        const auto trim = [](const std::string& codec) {
            const auto start = codec.find_first_not_of(" \t");
            return start == std::string::npos ? std::string() :
                codec.substr(start, codec.find_last_not_of(" \t") - start + 1);
        };
        const auto first_codec = trim(codecs.substr(0, comma));
        const auto second_codec = comma==std::string::npos?std::string():trim(codecs.substr(comma + 1));
        const auto is_video=[](const std::string& codec) {
            return starts(codec,"avc1.")||starts(codec,"avc3.")||starts(codec,"hvc1.1.")||starts(codec,"hev1.1.");
        };
        const bool first_video=is_video(first_codec);
        const auto& video=first_video?first_codec:second_codec;
        const auto& audio=first_video?second_codec:first_codec;
        if(!is_video(video)||is_video(audio)) return 0;
        const bool supported_audio=audio=="mp4a.40.2"||audio=="mp4a.40.5"||audio=="mp4a.40.29"||
            audio=="mp4a.40.34"||audio=="mp4a.6B"||audio=="mp4a.6b"||audio=="mp4a.69"||
            audio=="mp3"||audio=="ac-3"||audio=="ec-3"||audio=="fLaC"||audio=="alac"||audio=="opus"||audio=="Opus";
        hevc_possible=starts(video,"hvc1.1.")||starts(video,"hev1.1.");
        if(starts(video,"avc1.")||starts(video,"avc3.")) preference=supported_audio?5:3;
        else if(starts(video,"hvc1.1.")||starts(video,"hev1.1.")) preference=supported_audio?4:1;
        else return 0;
    }
    if (!attribute(tag, "VIDEO").empty()) return 0;
    const auto resolution = attribute(tag, "RESOLUTION");
    bool uhd = false;
    if (!resolution.empty()) {
        const auto divider = resolution.find('x');
        double width = 0, height = 0;
        if (divider == std::string::npos || !decimal(resolution.substr(0, divider), width) ||
            !decimal(resolution.substr(divider + 1), height) || width < 1 || height < 1 || width != std::floor(width) ||
            height != std::floor(height) || width > direct_hevc_max_width || height > direct_hevc_max_height) return 0;
        uhd = direct_hevc_uhd(int(width),int(height));
        if (uhd && !hevc_possible) return 0;
    }
    const auto frame_rate = attribute(tag, "FRAME-RATE");
    double fps = 0;
    if (!frame_rate.empty() && (!decimal(frame_rate, fps) || fps <= 0 || fps > (uhd ? direct_hevc_uhd_max_fps : 60))) return 0;
    double bandwidth = 0;
    return decimal(attribute(tag, "BANDWIDTH"), bandwidth) && bandwidth <= 20000000 ? bandwidth : 0;
}

bool parse_range(const std::string& value, HlsRange& range) {
    const auto separator=value.find('@');
    if (!hls_decimal_integer(value.substr(0,separator),range.length) || !range.length) return false;
    range.offset=UINT64_MAX; // resolved against the preceding media subrange once its URI is known
    return separator==std::string::npos ||
        (hls_decimal_integer(value.substr(separator+1),range.offset) && range.offset!=UINT64_MAX);
}

bool resolve_range(HlsRange& range, const std::string& uri, const std::vector<DirectSegment>& segments) {
    if (!range.length) return true;
    if (range.offset==UINT64_MAX) {
        if (segments.empty() || segments.back().url!=uri || !segments.back().range.length) return false;
        const auto& last=segments.back().range;
        range.offset=last.offset+last.length;
    }
    return range.length<=max_segment_bytes && range.offset<=uint64_t(INT64_MAX)-range.length;
}

bool map_attributes(const std::string& line, std::string& uri, HlsRange& range) {
    bool have_range=false;
    for (size_t at=line.find(':')+1; at<line.size();) {
        const auto equal=line.find('=',at);
        if (equal==std::string::npos || equal==at || equal+1==line.size()) return false;
        const auto name=line.substr(at,equal-at);
        const bool quoted=line[equal+1]=='"';
        const auto begin=equal+(quoted?2:1);
        auto end=line.find(quoted?'"':',',begin);
        if (quoted && end==std::string::npos) return false;
        if (end==std::string::npos) end=line.size();
        const auto value=line.substr(begin,end-begin);
        if (name=="URI") {
            if (!quoted||!uri.empty()||value.empty()) return false;
            uri=value;
        } else if (name=="BYTERANGE") {
            if (!quoted||have_range||!parse_range(value,range)) return false;
            have_range=true;
        }
        at=end+unsigned(quoted);
        if (at==line.size()) break;
        if (line[at]!=',' || ++at==line.size()) return false;
    }
    return !uri.empty();
}

struct SegmentReader {
    const std::string& init;
    const std::string& data;
    const std::atomic<bool>& cancelled;
    size_t offset = 0;

    SegmentReader(const std::string& prefix, const std::string& bytes, const std::atomic<bool>& cancelled)
        : init(prefix), data(bytes), cancelled(cancelled) {}

    size_t size() const { return init.size()+data.size(); }

    static int read(void* opaque, uint8_t* dst, int length) {
        auto& reader = *static_cast<SegmentReader*>(opaque);
        if (reader.cancelled) return AVERROR_EXIT;
        const bool in_init=reader.offset<reader.init.size();
        const auto& part=in_init?reader.init:reader.data;
        const size_t start=in_init?reader.offset:reader.offset-reader.init.size();
        const size_t count = std::min(size_t(length), part.size() - start);
        if (!count) return AVERROR_EOF;
        std::memcpy(dst, part.data() + start, count);
        reader.offset += count;
        return int(count);
    }

    static int interrupted(void* opaque) { return static_cast<SegmentReader*>(opaque)->cancelled.load(); }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& reader=*static_cast<SegmentReader*>(opaque);
        if (reader.cancelled) return AVERROR_EXIT;
        const int64_t size=reader.size();
        if (whence==AVSEEK_SIZE) return size;
        whence&=~AVSEEK_FORCE;
        if (whence!=SEEK_SET&&whence!=SEEK_CUR&&whence!=SEEK_END) return AVERROR(EINVAL);
        const int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?int64_t(reader.offset):size;
        if (offset< -base||offset>size-base) return AVERROR(EINVAL);
        return reader.offset=size_t(base+offset);
    }
};

struct Demuxer {
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVPacket* packet = nullptr;
    AVBSFContext* annex_b = nullptr;

    ~Demuxer() {
        av_packet_free(&packet);
        av_bsf_free(&annex_b);
        avformat_close_input(&format);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }

    bool open(SegmentReader& reader, bool mp4) {
        const auto* input = av_find_input_format(mp4?"mov":"mpegts");
        if (!input) return false;
        auto* buffer = static_cast<uint8_t*>(av_malloc(16384));
        if (!buffer) return false;
        io = avio_alloc_context(buffer, 16384, 0, &reader, SegmentReader::read, nullptr, mp4?SegmentReader::seek:nullptr);
        if (!io) { av_free(buffer); return false; }
        format = avformat_alloc_context();
        if (!format) return false;
        format->pb = io;
        // Keep explicit PES timestamps. FFmpeg's generic timestamp fill-in
        // can move DTS backwards after a BipBop B frame, even though the
        // original PES decode times are ordered. AAC uses its own sample clock.
        format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOFILLIN;
        format->max_streams = 8;
        format->probesize = 128 * 1024;
        format->max_probe_packets = 64;
        format->interrupt_callback = {SegmentReader::interrupted, &reader};
        // A MOV reference can never open a path, device or URL on its own.
        if (mp4) format->io_open=[](AVFormatContext*,AVIOContext**,const char*,int,AVDictionary**) {
            return AVERROR(EPERM);
        };
        return avformat_open_input(&format, nullptr, input, nullptr) == 0 && (packet = av_packet_alloc());
    }

    bool init_video(const AVStream& stream) {
        if (!annex_b) {
            const auto* filter=av_bsf_get_by_name(stream.codecpar->codec_id==AV_CODEC_ID_HEVC?
                                                 "hevc_mp4toannexb":"h264_mp4toannexb");
            if (!filter||av_bsf_alloc(filter,&annex_b)<0||
                avcodec_parameters_copy(annex_b->par_in,stream.codecpar)<0) return false;
            annex_b->time_base_in=stream.time_base;
            if (av_bsf_init(annex_b)<0) return false;
        }
        return true;
    }
    bool convert_video(const AVStream& stream) {
        if(!init_video(stream)) return false;
        return av_bsf_send_packet(annex_b,packet)==0&&av_bsf_receive_packet(annex_b,packet)==0;
    }
};

struct AdtsFormat { int sample_rate; int channels; };
bool compatible_audio(AVCodecID id, bool mp4) {
    return id==AV_CODEC_ID_AAC||id==AV_CODEC_ID_MP3||id==AV_CODEC_ID_AC3||id==AV_CODEC_ID_EAC3||
        (mp4 && (id==AV_CODEC_ID_FLAC||id==AV_CODEC_ID_ALAC||id==AV_CODEC_ID_OPUS));
}

bool mp4_audio_config(const AVCodecParameters& codec) {
    switch(codec.codec_id) {
    case AV_CODEC_ID_AAC: {
        if(codec.codec_tag!=MKTAG('m','p','4','a') || !codec.extradata ||
           codec.extradata_size<2 || codec.extradata_size>64) return false;
        const unsigned object_type=codec.extradata[0]>>3;
        // AOT 2 may still signal SBR or PS in the extension or implicitly in-band.
        // FFmpeg interprets the full AudioSpecificConfig and owns AAC decoding.
        return object_type==2 || object_type==5 || object_type==29;
    }
    case AV_CODEC_ID_AC3: return codec.codec_tag==MKTAG('a','c','-','3');
    case AV_CODEC_ID_EAC3: return codec.codec_tag==MKTAG('e','c','-','3');
    case AV_CODEC_ID_FLAC: return codec.codec_tag==MKTAG('f','L','a','C');
    case AV_CODEC_ID_ALAC: return codec.codec_tag==MKTAG('a','l','a','c');
    case AV_CODEC_ID_OPUS: return codec.codec_tag==MKTAG('O','p','u','s');
    default: return true;
    }
}
constexpr int aac_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                             22050, 16000, 12000, 11025, 8000, 7350};

AdtsFormat adts_aac_lc(const AVPacket& packet) {
    if (packet.size < 7) return {};
    const auto* data = packet.data;
    const unsigned rate = (data[2] >> 2) & 15;
    const unsigned channels = ((data[2] & 1) << 2) | (data[3] >> 6);
    const int frame_bytes = ((data[3] & 3) << 11) | (data[4] << 3) | (data[5] >> 5);
    // The AAC decoder reports the actual input format to swresample. BipBop
    // uses index 7 (22.05 kHz), and all standard ADTS rates can be resampled.
    if (data[0] != 0xff || (data[1] & 0xf6) != 0xf0 || (data[2] >> 6) != 1 ||
        rate >= sizeof(aac_rates) / sizeof(*aac_rates) || (channels != 1 && channels != 2) ||
        ((data[1] & 1) == 0 && packet.size < 9) ||
        (data[6] & 3) || frame_bytes != packet.size) return {};
    return {aac_rates[rate], int(channels)};
}

// Validate the two resources independently before exposing their virtual
// concatenation to MOV. Only self-contained, movie-relative fragments fit it.
struct Box { uint32_t type=0; const uint8_t* data=nullptr; size_t size=0; };
constexpr uint32_t fourcc(char a,char b,char c,char d) { return MKBETAG(a,b,c,d); }
bool read_box(const uint8_t*& cursor, size_t& remaining, Box& box) {
    if (remaining<8) return false;
    uint64_t size=AV_RB32(cursor);
    const size_t header=size==1?16:8;
    if (remaining<header) return false;
    if (size==1) size=AV_RB64(cursor+8);
    if (size<header||size>remaining) return false;
    box.type=AV_RB32(cursor+4);box.data=cursor+header;box.size=size_t(size)-header;
    cursor+=size_t(size);remaining-=size_t(size);
    return true;
}
bool encryption_box(uint32_t type) {
    return type==fourcc('p','s','s','h')||type==fourcc('s','e','n','c')||type==fourcc('s','a','i','z')||
        type==fourcc('s','a','i','o')||type==fourcc('t','e','n','c')||type==fourcc('s','i','n','f')||
        type==fourcc('e','n','c','v')||type==fourcc('e','n','c','a');
}

DirectError inspect_boxes(const uint8_t* at, size_t length, unsigned depth, bool& mvex) {
    if (depth>8) return DirectError::Mp4Boxes;
    Box child;
    while (length) {
        if (!read_box(at,length,child)) return DirectError::Mp4Boxes;
        if (encryption_box(child.type)) return DirectError::EncryptedMedia;
        size_t skip=0;
        switch (child.type) {
        case fourcc('m','v','e','x'): mvex=true; break;
        case fourcc('t','r','a','k'): case fourcc('m','d','i','a'): case fourcc('m','i','n','f'):
        case fourcc('s','t','b','l'): break;
        case fourcc('s','t','s','d'): skip=8; break;  // full box and entry count
        case fourcc('d','v','c','C'): case fourcc('d','v','v','C'): case fourcc('d','v','w','C'):
        case fourcc('d','v','h','1'): case fourcc('d','v','h','e'):
        case fourcc('m','d','c','v'): case fourcc('c','l','l','i'): return DirectError::HevcHdr;
        case fourcc('a','v','c','1'): case fourcc('a','v','c','3'):
        case fourcc('h','v','c','1'): case fourcc('h','e','v','1'): skip=78; break;
        case fourcc('m','p','4','a'): case fourcc('f','L','a','C'):
        case fourcc('a','l','a','c'): case fourcc('O','p','u','s'):
            if (child.size<28 || AV_RB16(child.data+8)!=0) return DirectError::Mp4Config;
            skip=28;
            break;
        default: continue;
        }
        if (child.size<skip) return DirectError::Mp4Boxes;
        const auto error=inspect_boxes(child.data+skip,child.size-skip,depth+1,mvex);
        if (error!=DirectError::None) return error;
    }
    return DirectError::None;
}

DirectError inspect_fragment(const Box& moof, uint32_t& sequence) {
    auto* at=moof.data;
    auto remaining=moof.size;
    Box child;
    unsigned tracks=0;
    bool sequence_seen=false, implicit_base=false;
    while (remaining) {
        if (!read_box(at,remaining,child)) return DirectError::Mp4Boxes;
        if (encryption_box(child.type)) return DirectError::EncryptedMedia;
        if (child.type==fourcc('m','f','h','d')) {
            if (sequence_seen||child.size!=8||AV_RB32(child.data)!=0) return DirectError::Mp4Boxes;
            sequence_seen=true;sequence=AV_RB32(child.data+4);
        }
        if (child.type!=fourcc('t','r','a','f')) continue;
        if (++tracks>2) return DirectError::Mp4Track;
        auto* field=child.data;
        auto left=child.size;
        bool tfhd=false,tfdt=false,trun=false;
        Box entry;
        while (left) {
            if (!read_box(field,left,entry)) return DirectError::Mp4Boxes;
            if (encryption_box(entry.type)) return DirectError::EncryptedMedia;
            if (entry.type==fourcc('t','f','h','d')) {
                if(tfhd||entry.size<8||(AV_RB32(entry.data)&1)) return DirectError::Mp4Boxes;
                tfhd=true;
                implicit_base|=(AV_RB32(entry.data)&0x020000)==0;
            } else if (entry.type==fourcc('t','f','d','t')) {
                if(tfdt||entry.size<8||entry.data[0]>1||entry.size!=(entry.data[0]?12u:8u))
                    return DirectError::Mp4Boxes;
                tfdt=true;
            } else if (entry.type==fourcc('t','r','u','n')) {
                if(entry.size<8||(!trun&&!(AV_RB32(entry.data)&1))||AV_RB32(entry.data+4)>max_segment_packets)
                    return DirectError::Mp4Boxes;
                trun=true;
            }
        }
        if (!tfhd||!tfdt||!trun) return DirectError::Mp4Boxes;
    }
    return tracks&&sequence_seen&&(!implicit_base||tracks==1)?DirectError::None:DirectError::Mp4Boxes;
}

DirectError inspect_mp4(const std::string& init, const std::string& fragment, unsigned& fragments) {
    const auto* at=reinterpret_cast<const uint8_t*>(init.data());
    size_t remaining=init.size();
    Box box;
    bool ftyp=false,moov=false,mvex=false;
    while(remaining) {
        if(!read_box(at,remaining,box)) return DirectError::Mp4Boxes;
        if(encryption_box(box.type)) return DirectError::EncryptedMedia;
        if(box.type==fourcc('f','t','y','p')) {
            if(ftyp||moov||box.size<8) return DirectError::Mp4Boxes;
            ftyp=true;
        } else if(box.type==fourcc('m','o','o','v')) {
            if(!ftyp||moov) return DirectError::Mp4Boxes;
            moov=true;
            const auto error=inspect_boxes(box.data,box.size,0,mvex);
            if(error!=DirectError::None) return error;
        } else if(box.type!=fourcc('f','r','e','e')&&box.type!=fourcc('s','k','i','p')) return DirectError::Mp4Boxes;
    }
    if(!ftyp||!moov||!mvex) return DirectError::Mp4Boxes;
    at=reinterpret_cast<const uint8_t*>(fragment.data());remaining=fragment.size();
    bool awaiting_data=false;
    uint32_t previous_sequence=0;
    while(remaining) {
        if(!read_box(at,remaining,box)) return DirectError::Mp4Boxes;
        if(encryption_box(box.type)) return DirectError::EncryptedMedia;
        if(box.type==fourcc('m','o','o','f')) {
            if(awaiting_data||fragments>=128) return DirectError::Mp4Boxes;
            uint32_t sequence=0;
            const auto error=inspect_fragment(box,sequence);
            if(error!=DirectError::None) return error;
            if(fragments&&sequence<=previous_sequence) return DirectError::Mp4Boxes;
            previous_sequence=sequence;
            awaiting_data=true;
        } else if(box.type==fourcc('m','d','a','t')) {
            if(!awaiting_data||!box.size) return DirectError::Mp4Boxes;
            awaiting_data=false;
            ++fragments;
        } else if(box.type!=fourcc('s','t','y','p')&&box.type!=fourcc('s','i','d','x')&&
                  box.type!=fourcc('e','m','s','g')&&box.type!=fourcc('p','r','f','t')&&
                  box.type!=fourcc('f','r','e','e')&&box.type!=fourcc('s','k','i','p')&&
                  box.type!=fourcc('m','f','r','a')) return DirectError::Mp4Boxes;
    }
    return fragments&&!awaiting_data?DirectError::None:DirectError::Mp4Boxes;
}

bool prefer_variant(double candidate, double selected) {
    // Prefer rates that fit a maximum-length segment in the download budget.
    // Keep a higher-rate rendition eligible when the master offers no smaller one.
    constexpr double budget = double(max_segment_bytes) * 8 / 12;
    if (candidate <= 0) return false;
    if (selected <= 0) return true;
    if (candidate <= budget) return selected > budget || candidate > selected;
    return selected > budget && candidate < selected;
}
} // namespace

void direct_hls_log_setup() { av_log_set_callback(hls_ffmpeg_log); }

DirectError direct_parse_playlist(const std::string& text, const std::string& base, DirectPlaylist& playlist) {
    playlist = DirectPlaylist();
    if (text.size() > 256 * 1024) return DirectError::StreamLimit;
    if (text.find('\0') != std::string::npos || text.find("{$") != std::string::npos) return DirectError::PlaylistSyntax;
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line) || (line != "#EXTM3U" && line != "#EXTM3U\r")) return DirectError::PlaylistSyntax;
    bool master = false, end = false, waiting_variant = false;
    struct Variant { std::string url, audio; double bandwidth; unsigned preference; };
    std::vector<Variant> candidates;
    std::map<std::string,bool> audio_groups;
    std::string variant_audio;
    double bandwidth = 0;
    unsigned preference = 0;
    int64_t duration = 0;
    unsigned variants = 0;
    unsigned init=0, period=0;
    HlsRange next_range;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 8192) return DirectError::StreamLimit;
        if (line[0] != '#') {
            if (end || (master && !waiting_variant) || (!master && duration == 0)) return DirectError::PlaylistSyntax;
            const auto url = HlsRelay::resolve(base, line);
            if (url.empty()) return DirectError::PlaylistSyntax;
            if (master) {
                candidates.push_back({url,variant_audio,bandwidth,preference});
                waiting_variant = false;
            } else {
                if (playlist.segments.size() >= 4096 || playlist.duration_ns > 24 * 60 * 60 * second - duration)
                    return DirectError::StreamLimit;
                if (period&&!init) return DirectError::PlaylistDiscontinuity;
                if (!resolve_range(next_range,url,playlist.segments)) return DirectError::PlaylistByteRange;
                DirectSegment segment;
                segment.url=url;segment.start_ns=playlist.duration_ns;segment.duration_ns=duration;
                segment.range=next_range;segment.init=init;segment.period=period;
                playlist.segments.push_back(std::move(segment));
                next_range={};
                playlist.duration_ns += duration;
                duration = 0;
            }
        } else if (starts(line, "#EXT-X-STREAM-INF:")) {
            if (end || duration || waiting_variant || init || period || next_range.length || !playlist.segments.empty()) return DirectError::PlaylistSyntax;
            if (++variants > 32) return DirectError::StreamLimit;
            master = waiting_variant = true;
            bandwidth = variant_bandwidth(line,preference);
            variant_audio=attribute(line,"AUDIO");
        } else if (starts(line,"#EXT-X-MEDIA:")) {
            if (attribute(line,"TYPE")=="AUDIO") {
                const auto group=attribute(line,"GROUP-ID");
                if (group.empty()||group.size()>128||audio_groups.size()>=64) return DirectError::PlaylistSyntax;
                audio_groups[group] |= attribute(line,"URI").empty();
            }
        } else if (starts(line,"#EXT-X-MAP:")) {
            if (master||end) return DirectError::PlaylistSyntax;
            DirectInitSegment map;
            playlist.diagnostic_tag="#EXT-X-MAP";
            if (!map_attributes(line,map.url,map.range)) return DirectError::PlaylistSyntax;
            map.url=HlsRelay::resolve(base,map.url);
            if (map.url.empty()) return DirectError::PlaylistSyntax;
            if (map.range.length && (map.range.offset == UINT64_MAX || map.range.length > 1024 * 1024 ||
                                     map.range.offset > uint64_t(INT64_MAX) - map.range.length))
                return DirectError::PlaylistByteRange;
            const auto prior=std::find_if(playlist.maps.begin(),playlist.maps.end(),[&](const DirectInitSegment& other){
                return map.url==other.url&&map.range==other.range;
            });
            if (prior==playlist.maps.end()) {
                if (playlist.maps.size()>=64) return DirectError::StreamLimit;
                playlist.maps.push_back(std::move(map));init=unsigned(playlist.maps.size());
            } else init=unsigned(prior-playlist.maps.begin()+1);
        } else if (starts(line,"#EXT-X-BYTERANGE:")) {
            if (master||end||next_range.length||!parse_range(line.substr(sizeof("#EXT-X-BYTERANGE:")-1),next_range))
                return DirectError::PlaylistByteRange;
        } else if (line=="#EXT-X-DISCONTINUITY") {
            if (master||end||period>=64) return DirectError::PlaylistDiscontinuity;
            playlist.diagnostic_tag=line;
            ++period;
        } else if (starts(line, "#EXTINF:")) {
            double parsed = 0;
            if (master || end || duration || !decimal(line.substr(8, line.find(',') - 8), parsed) || parsed < 0.01)
                return DirectError::PlaylistSyntax;
            if (parsed > 12) return DirectError::StreamLimit;
            duration = int64_t(std::llround(parsed * second));
        } else if (line == "#EXT-X-ENDLIST") {
            if (master || duration || end) return DirectError::PlaylistSyntax;
            end = true;
        } else if (!starts(line, "#EXT") || line == "#EXT-X-INDEPENDENT-SEGMENTS" ||
                   line == "#EXT-X-KEY:METHOD=NONE" || starts(line, "#EXT-X-VERSION:") ||
                   starts(line, "#EXT-X-TARGETDURATION:") || starts(line, "#EXT-X-MEDIA-SEQUENCE:") ||
                   starts(line, "#EXT-X-PLAYLIST-TYPE:") || starts(line, "#EXT-X-PROGRAM-DATE-TIME:") ||
                   starts(line, "#EXT-X-I-FRAME-STREAM-INF:")) {
            continue;
        } else {
            if (!starts(line, "#EXT-X-")) continue; // HLS clients ignore other extended comments.
            const auto tag = line.substr(0, line.find(':'));
            if (tag.size() > 48 || tag.find_first_not_of("#-ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
                return DirectError::PlaylistSyntax;
            if (tag == "#EXT-X-KEY" || tag == "#EXT-X-SESSION-KEY") {
                playlist.diagnostic_tag = tag; return DirectError::PlaylistEncryption;
            }
            if (tag == "#EXT-X-I-FRAMES-ONLY") {
                playlist.diagnostic_tag = tag; return DirectError::PlaylistInitSegment;
            }
            if (tag == "#EXT-X-BYTERANGE") {
                playlist.diagnostic_tag = tag; return DirectError::PlaylistByteRange;
            }
            if (tag == "#EXT-X-GAP" || tag == "#EXT-X-PART" || tag == "#EXT-X-SKIP" ||
                tag == "#EXT-X-PRELOAD-HINT" || tag == "#EXT-X-CONTENT-STEERING" || tag == "#EXT-X-DEFINE") {
                playlist.diagnostic_tag = tag; return DirectError::PlaylistTag;
            }
            // Unknown tags and metadata such as DATERANGE do not change
            // where this single-rendition VOD reader gets its complete segments.
            if (playlist.diagnostic_tag.empty()) playlist.diagnostic_tag = tag;
        }
    }
    if (waiting_variant || duration || next_range.length) return DirectError::PlaylistSyntax;
    if (master) {
        bool external_audio=false;
        const Variant* selected=nullptr;
        const Variant* alternate=nullptr;
        const auto preferable=[](const Variant& a,const Variant* b) {
            return !b||a.preference>b->preference||
                (a.preference==b->preference&&prefer_variant(a.bandwidth,b->bandwidth));
        };
        for (const auto& variant : candidates) {
            if (!variant.audio.empty()) {
                const auto group=audio_groups.find(variant.audio);
                if (group==audio_groups.end()||!group->second) { external_audio=true; continue; }
            }
            if(variant.bandwidth<=0||(selected&&variant.url==selected->url)) continue;
            if(preferable(variant,selected)){alternate=selected;selected=&variant;}
            else if(preferable(variant,alternate)) alternate=&variant;
        }
        if(selected) playlist.variant=selected->url;
        if(alternate) playlist.alternate=alternate->url;
        return !playlist.variant.empty()?DirectError::None:external_audio?DirectError::SeparateAudio:DirectError::PlaylistVariant;
    }
    if (playlist.segments.empty()) return period?DirectError::PlaylistDiscontinuity:DirectError::PlaylistSyntax;
    return end ? DirectError::None : DirectError::PlaylistLive;
}

DirectError direct_demux_segment(const std::string& data,const std::string& init,const std::atomic<bool>& cancelled,
                               AudioDecoder& aac,HlsAudioDecoder& audio,int width,int height,
                               bool mute,bool drain,DirectDemuxedSegment& segment) {
    segment = DirectDemuxedSegment();
    const TimestampWarnings warnings(segment.ffmpeg_timestamp_warnings);
    if (cancelled) return DirectError::FetchCancelled;
    if (data.size() > max_segment_bytes) return DirectError::StreamLimit;
    const bool mp4=!init.empty();
    if (mp4) {
        segment.container="fmp4";
        if (init.size()>1024*1024) return DirectError::StreamLimit;
        const auto result=inspect_mp4(init,data,segment.fragments);
        if(result!=DirectError::None) return result;
    } else {
        // Match MPEG-TS even with a misleading suffix or MIME type.
        const auto probe_size = std::min(data.size(), size_t(64 * 1024));
        std::vector<uint8_t> prefix(probe_size + AVPROBE_PADDING_SIZE, 0);
        std::copy_n(data.begin(), probe_size, prefix.begin());
        AVProbeData probe = {};
        probe.filename = "";
        probe.buf = prefix.data();
        probe.buf_size = int(probe_size);
        const auto* input = av_probe_input_format3(&probe, 1, &segment.probe_score);
        if (!input || std::strcmp(input->name, "mpegts")) {
            if (data.size() >= 8 && (!data.compare(4, 4, "styp") || !data.compare(4, 4, "moof") ||
                                   !data.compare(4, 4, "sidx"))) {
                segment.container = "fmp4";
                return DirectError::PlaylistInitSegment;
            }
            if (data.size() >= 8 && !data.compare(4, 4, "ftyp")) segment.container = "iso-bmff";
            return DirectError::SegmentContainer;
        }
        segment.container = "mpegts";
    }
    SegmentReader reader(init, data, cancelled);
    Demuxer demux;
    HevcStream hevc;
    bool discard_hevc_rasl=false;
    const AVStream* decoded_stream=nullptr;
    if(mute) segment.audio_fallback="audio-disabled";
    if (!demux.open(reader,mp4)) return cancelled ? DirectError::FetchCancelled : DirectError::DemuxOpen;
    if (mp4) {
        segment.tracks=demux.format->nb_streams;
        if (!segment.tracks||segment.tracks>2) return DirectError::Mp4Track;
        unsigned videos=0,audios=0;
        for (unsigned i=0;i<segment.tracks;++i) {
            const auto& stream=*demux.format->streams[i];
            const auto& par=*stream.codecpar;
            if (par.codec_type==AVMEDIA_TYPE_VIDEO) {
                ++videos;segment.video_codec=par.codec_id;
                segment.video_codec_name=avcodec_get_name(par.codec_id);
                segment.hevc=par.codec_id==AV_CODEC_ID_HEVC;
                if(segment.hevc) {
                    const auto* conf=par.extradata;
                    segment.source_width=par.width;
                    segment.source_height=par.height;
                    segment.color_primaries=par.color_primaries;
                    segment.color_transfer=par.color_trc;
                    segment.color_matrix=par.color_space;
                    if(par.codec_tag!=MKTAG('h','v','c','1')&&par.codec_tag!=MKTAG('h','e','v','1'))
                        return DirectError::HevcProfile;
                    if(!conf||par.extradata_size<23||par.extradata_size>65536||conf[0]!=1)
                        return DirectError::HevcConfiguration;
                    segment.h264_profile=conf[1]&31;
                    segment.h264_level=conf[12];
                    segment.hevc_tier=(conf[1]>>5)&1;
                    segment.hevc_chroma=conf[16]&3;
                    segment.hevc_luma_depth=8+(conf[17]&7);
                    segment.hevc_chroma_depth=8+(conf[18]&7);
                    if(par.color_trc==AVCOL_TRC_SMPTE2084||par.color_trc==AVCOL_TRC_ARIB_STD_B67)
                        return DirectError::HevcHdr;
                    if((conf[1]&0xdf)!=1||segment.hevc_chroma!=1||segment.hevc_luma_depth!=8||
                       segment.hevc_chroma_depth!=8||!conf[12]||conf[12]>153)
                        return DirectError::HevcProfile;
                    if(!demux.init_video(stream)) return DirectError::HevcConfiguration;
                    const auto& annex=*demux.annex_b->par_out;
                    const auto config=hevc.inspect(annex.extradata,size_t(annex.extradata_size));
                    if(!config.configuration_complete) return DirectError::HevcConfiguration;
                    segment.source_width=config.width;
                    segment.source_height=config.height;
                    if(config.profile!=1||config.bit_depth!=8) return DirectError::HevcProfile;
                    if(config.width<=0||config.height<=0||config.width>direct_hevc_max_width||
                       config.height>direct_hevc_max_height) return DirectError::HevcGeometry;
                    segment.h264_profile=config.profile;
                    segment.pixel_format="yuv420p";
                    continue;
                }
                if(par.codec_id!=AV_CODEC_ID_H264||(par.codec_tag!=MKTAG('a','v','c','1')&&par.codec_tag!=MKTAG('a','v','c','3')))
                    return DirectError::VideoCodec;
                if(!par.extradata||par.extradata_size<7||par.extradata_size>65536||
                   par.extradata[0]!=1||(par.extradata[4]&3)==2) return DirectError::Mp4Config;
                segment.h264_level = par.extradata[3];
            } else if(par.codec_type==AVMEDIA_TYPE_AUDIO) {
                ++audios;segment.audio_codec=par.codec_id;
                segment.audio_codec_name=avcodec_get_name(par.codec_id);
                if(!compatible_audio(par.codec_id,true)) segment.audio_fallback="unsupported-audio-codec";
                else if(!mp4_audio_config(par)) segment.audio_fallback="invalid-audio-config";
            }
        }
        if(videos!=1||audios>1) return DirectError::Mp4Track;
    }
    const bool continuing = width != 0;
    segment.width = width;
    segment.height = height;
    int video_index = -1, audio_index = -1, result = 0;
    unsigned packets = 0;
    int64_t shortest_decode_step = 0;
    int64_t first_packet_duration = 0;
    // Reuse the active decoder across continuous segments. A new decoder
    // needs in-band parameter sets and an IDR, which may follow leading AUs.
    std::vector<uint8_t> configuration;
    bool have_sps = false, have_pps = false;
    int64_t leading_dts = AV_NOPTS_VALUE, first_video_pts = 0;
    int64_t audio_pts_anchor = AV_NOPTS_VALUE;
    int64_t last_audio_pts = AV_NOPTS_VALUE;
    int audio_samples = 0;
    unsigned aac_without_pcm = 0;
    const unsigned frames_at_start=audio.frames;
    std::vector<HlsPcm> pcm_blocks;
    // Add only decoded, timestamped PCM. Audio fallback removes any PCM from
    // the segment before it becomes visible to the playback thread.
    const auto save_pcm=[&](std::vector<uint8_t>& buffer,int64_t pts) {
        if(buffer.size()>max_segment_bytes-segment.bytes) return false;
        DirectAccessUnit pcm;
        pcm.pts_ns=pcm.dts_ns=pts;
        pcm.duration_ns=av_rescale_q(buffer.size()/4,AVRational{1,44100},AVRational{1,int(second)});
        pcm.data.swap(buffer);
        segment.bytes+=pcm.data.size();
        if(!segment.audio_packets++) segment.first_pcm_bytes=pcm.data.size();
        segment.packets.push_back(std::move(pcm));
        return true;
    };
    const auto timestamp_error = [&](bool video, const char* reason) {
        segment.timestamp_problem = reason;
        return video ? DirectError::VideoTimestamp : DirectError::AudioTimestamp;
    };
    constexpr int64_t max_timestamp = 2 * 24 * 60 * 60 * second;
    while (!cancelled && (result = av_read_frame(demux.format, demux.packet)) >= 0) {
        if (++packets > max_segment_packets) return DirectError::StreamLimit;
        AVPacket& packet = *demux.packet;
        const AVStream& stream = *demux.format->streams[packet.stream_index];
        const bool video = stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        const bool is_audio = stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !is_audio) { av_packet_unref(&packet); continue; }
        if (video) {
            segment.video_codec = stream.codecpar->codec_id;
            segment.video_codec_name = avcodec_get_name(stream.codecpar->codec_id);
        }
        else {
            segment.audio_codec=stream.codecpar->codec_id;
            segment.audio_codec_name=avcodec_get_name(stream.codecpar->codec_id);
            ++segment.audio_input_packets;
            if(audio_index>=0 && packet.stream_index!=audio_index) return DirectError::MultipleTracks;
            audio_index=packet.stream_index;
        }
        if (mp4 && (av_packet_get_side_data(&packet,AV_PKT_DATA_ENCRYPTION_INFO,nullptr)||
                    av_packet_get_side_data(&packet,AV_PKT_DATA_ENCRYPTION_INIT_INFO,nullptr))) return DirectError::EncryptedMedia;
        const bool new_config=mp4 && av_packet_get_side_data(&packet,AV_PKT_DATA_NEW_EXTRADATA,nullptr);
        const bool oversized=packet.size<=0||size_t(packet.size)>max_packet_bytes;
        const bool corrupt=packet.flags&AV_PKT_FLAG_CORRUPT;
        if(video) {
            if(new_config) return DirectError::Mp4Config;
            if(oversized) return DirectError::StreamLimit;
            if(corrupt) return DirectError::PacketCorrupt;
        } else {
            if(!segment.audio_fallback) {
                if(new_config) segment.audio_fallback="audio-track-change";
                else if(oversized||corrupt) segment.audio_fallback="invalid-audio-packet";
                else if(!compatible_audio(stream.codecpar->codec_id,mp4))
                    segment.audio_fallback=compatible_audio(stream.codecpar->codec_id,true)?
                        "unsupported-audio-container":"unsupported-audio-codec";
            }
            if(segment.audio_fallback) {av_packet_unref(&packet);continue;}
        }
        const AVRational ns = {1, int(second)};
        DirectAccessUnit unit;
        unit.video = video;
        if (video) {
            if (stream.codecpar->codec_id != (segment.hevc?AV_CODEC_ID_HEVC:AV_CODEC_ID_H264)) return DirectError::VideoCodec;
            if (video_index >= 0 && packet.stream_index != video_index) return DirectError::MultipleTracks;
            video_index = packet.stream_index;
            if(mp4 && !demux.convert_video(stream)) return segment.hevc?DirectError::HevcConfiguration:DirectError::H264AnnexB;
            if(packet.size<=0 || size_t(packet.size)>max_packet_bytes) return DirectError::StreamLimit;
            bool keyframe=false, in_band_configuration=false;
            if(segment.hevc) {
                const auto info=hevc.inspect(packet.data,size_t(packet.size));
                if(info.vcl_type<0 && info.waiting_configuration && info.configuration_complete) {
                    av_packet_unref(&packet);
                    continue;
                }
                if(!info.valid) return DirectError::HevcConfiguration;
                if(info.parameter_sets_updated) return DirectError::TrackChange;
                if(!supported_hevc_mirror_profile(info)) return DirectError::HevcProfile;
                if(info.width<=0||info.height<=0||info.width>direct_hevc_max_width||
                   info.height>direct_hevc_max_height) return DirectError::HevcGeometry;
                if(segment.width&&(segment.width!=info.width||segment.height!=info.height))
                    return DirectError::TrackChange;
                segment.width=info.width;
                segment.height=info.height;
                keyframe=info.keyframe;
                const bool rasl=info.vcl_type==8||info.vcl_type==9;
                if(discard_hevc_rasl&&rasl){av_packet_unref(&packet);continue;}
                if(info.vcl_type!=6&&info.vcl_type!=7)
                    discard_hevc_rasl=!continuing&&!segment.video_packets&&info.vcl_type==21;
                if(keyframe) {
                    bool added=false;
                    unit.data.assign(packet.data,packet.data+packet.size);
                    if(!hevc.prepend_missing_parameter_sets(unit.data,info,added)||!ensure_hevc_aud(unit.data,info.has_aud))
                        return DirectError::HevcConfiguration;
                    segment.sps_pps=true;
                    if(segment.hevc_rap<0) segment.hevc_rap=info.vcl_type;
                }
            } else {
                const auto info = inspect_h264(packet.data, size_t(packet.size));
                if (!info.valid) return DirectError::H264AnnexB;
                if (info.width) {
                    segment.h264_profile = int(info.profile);
                    segment.pixel_format = info.chroma == 1 && info.bit_depth == 8 ? "yuv420p" : "other";
                    if (info.height > 1080 || info.width > 1920 || info.chroma != 1 || info.bit_depth != 8)
                        return DirectError::H264Format;
                    if (segment.width && (info.width != segment.width || info.height != segment.height))
                        return DirectError::TrackChange;
                    segment.width = info.width;
                    segment.height = info.height;
                }
                if (!info.configuration.empty()) {
                    if (info.width && info.pps) configuration.clear();
                    if (configuration.size() + info.configuration.size() > 65536) return DirectError::StreamLimit;
                    configuration.insert(configuration.end(), info.configuration.begin(), info.configuration.end());
                    have_sps |= info.width != 0;
                    have_pps |= info.pps;
                    segment.sps_pps |= have_sps && have_pps;
                }
                if (!info.picture) { av_packet_unref(&packet); continue; }
                keyframe=info.keyframe;
                in_band_configuration=info.width && info.pps;
            }
            segment.idr |= keyframe;
            if (!continuing && !segment.video_packets && (!keyframe || !segment.sps_pps)) {
                if (leading_dts == AV_NOPTS_VALUE && packet.dts != AV_NOPTS_VALUE) {
                    leading_dts = av_rescale_q(packet.dts, stream.time_base, ns);
                    if (leading_dts < -second || leading_dts > max_timestamp)
                        return timestamp_error(true, "out-of-range");
                }
                av_packet_unref(&packet);
                continue;
            }
            if (packet.pts == AV_NOPTS_VALUE) return timestamp_error(true, "missing-pts");
            unit.pts_ns = av_rescale_q(packet.pts, stream.time_base, ns);
            if (packet.dts == AV_NOPTS_VALUE) {
                // A random-access IDR can start the decode timeline at its
                // own PTS. Subsequent decode times still have to keep order.
                if (continuing || segment.video_packets || !keyframe) return timestamp_error(true, "missing-dts");
                unit.dts_ns = unit.pts_ns;
                ++segment.inferred_dts;
            } else {
                unit.dts_ns = av_rescale_q(packet.dts, stream.time_base, ns);
            }
            unit.duration_ns = av_rescale_q(packet.duration, stream.time_base, ns);
            if (unit.pts_ns < -second || unit.pts_ns > max_timestamp || unit.dts_ns < -second ||
                unit.dts_ns > max_timestamp || unit.duration_ns < 0 || unit.duration_ns > second)
                return timestamp_error(true, "out-of-range");
            if (segment.video_packets && unit.dts_ns < segment.last_video_dts_ns)
                return timestamp_error(true, "decode-order");
            if (segment.video_packets && unit.dts_ns == segment.last_video_dts_ns) ++segment.tied_dts;
            if (segment.video_packets) {
                const auto step=unit.dts_ns-segment.last_video_dts_ns;
                if(segment.video_packets==1||step<shortest_decode_step) shortest_decode_step=step;
            } else {
                first_packet_duration=unit.duration_ns;
                segment.first_video_dts_ns = unit.dts_ns;
                segment.starts_with_idr = keyframe;
                first_video_pts = unit.pts_ns;
            }
            if (unit.pts_ns < unit.dts_ns) ++segment.reordered;
            segment.last_video_dts_ns = unit.dts_ns;
            ++segment.video_packets;
            if(!segment.hevc) {
                if (!in_band_configuration) unit.data.swap(configuration);
                configuration.clear();
                have_sps = have_pps = false;
            }
            if (!segment.hevc || unit.data.empty())
                unit.data.insert(unit.data.end(), packet.data, packet.data + packet.size);
        } else {
            if(mp4 || stream.codecpar->codec_id!=AV_CODEC_ID_AAC) {
                decoded_stream=&stream;
                segment.audio_fallback=audio.decode(stream,&packet,pcm_blocks);
                segment.aac_sample_rate=audio.rate;
                segment.aac_channels=audio.channels;
                if(!segment.audio_fallback) {
                    ++segment.audio_decoded_packets;
                    for(auto& block:pcm_blocks)
                        if(!save_pcm(block.data,block.time_ns)) return DirectError::StreamLimit;
                }
                av_packet_unref(&packet);
                continue;
            }
            const auto adts = adts_aac_lc(packet);
            if (!adts.sample_rate) {
                segment.audio_fallback="invalid-adts";
                av_packet_unref(&packet);
                continue;
            }
            if (segment.aac_sample_rate && (segment.aac_sample_rate != adts.sample_rate ||
                                            segment.aac_channels != adts.channels)) {
                segment.audio_fallback="audio-track-change";
                av_packet_unref(&packet);
                continue;
            }
            segment.aac_sample_rate = adts.sample_rate;
            segment.aac_channels = adts.channels;
            const AVRational sample_time = {1, adts.sample_rate};
            // MPEG-TS timestamps the PES. Splitting ADTS access units can leave
            // subsequent frames without a timestamp until the next PES anchor.
            if (packet.pts != AV_NOPTS_VALUE) {
                audio_pts_anchor = av_rescale_q(packet.pts, stream.time_base, ns);
                audio_samples = 0;
            }
            if (audio_pts_anchor == AV_NOPTS_VALUE) return timestamp_error(false, "missing-anchor");
            unit.pts_ns = unit.dts_ns = audio_pts_anchor + av_rescale_q(audio_samples, sample_time, ns);
            if (unit.pts_ns <= last_audio_pts) return timestamp_error(false, "presentation-order");
            last_audio_pts = unit.pts_ns;
            audio_samples += 1024;
            unit.duration_ns = av_rescale_q(1024, sample_time, ns);
            const bool decoded=aac.decode_adts(packet.data,size_t(packet.size),unit.data);
            if (!decoded) {
                segment.audio_fallback="aac-decode";
                av_packet_unref(&packet);
                continue;
            }
            ++segment.audio_decoded_packets;
            if (unit.data.empty()) {
                // The first Mux ADTS packet carries encoder data and consumes
                // a timestamp slot, while AAC returns success with no frame.
                ++segment.aac_without_pcm;
                if (++aac_without_pcm > 8) segment.audio_fallback="no-audio-pcm";
                av_packet_unref(&packet);
                continue;
            }
            aac_without_pcm = 0;
            unit.duration_ns = av_rescale_q(unit.data.size() / 4, AVRational{1, 44100}, ns);
            if (!segment.audio_packets++) segment.first_pcm_bytes = unit.data.size();
        }
        if (unit.pts_ns < -second || unit.pts_ns > max_timestamp || unit.dts_ns < -second ||
            unit.dts_ns > max_timestamp || unit.duration_ns < 0 || unit.duration_ns > second)
            return timestamp_error(video, "out-of-range");
        if (unit.pts_ns < unit.dts_ns - second || unit.pts_ns > unit.dts_ns + second)
            return timestamp_error(true, "composition-offset");
        if (unit.data.size() > max_segment_bytes - segment.bytes) return DirectError::StreamLimit;
        segment.bytes += unit.data.size();
        segment.packets.push_back(std::move(unit));
        av_packet_unref(&packet);
    }
    if (cancelled) return DirectError::FetchCancelled;
    if (result != AVERROR_EOF) return DirectError::DemuxRead;
    if (mp4 && video_index<0) return DirectError::Mp4Track;
    if (video_index < 0) return DirectError::MissingH264;
    if (!segment.video_packets) return segment.hevc?DirectError::HevcConfiguration:
        segment.idr?DirectError::H264Configuration:DirectError::H264Keyframe;
    const auto mean_frame_ns=segment.video_packets>1?
        (segment.last_video_dts_ns-segment.first_video_dts_ns)/(segment.video_packets-1):first_packet_duration;
    if(mean_frame_ns>0)
        segment.input_fps_milli=int(std::min<int64_t>(240000,(second*1000+mean_frame_ns/2)/mean_frame_ns));
    const auto frame_step=segment.video_packets>1?shortest_decode_step:first_packet_duration;
    if(segment.hevc && direct_hevc_uhd(segment.width,segment.height) && frame_step<hevc_uhd_min_frame_ns)
        return DirectError::HevcFrameRate;
    if(drain && decoded_stream && !segment.audio_fallback) {
        segment.audio_fallback=audio.decode(*decoded_stream,nullptr,pcm_blocks);
        if(!segment.audio_fallback)
            for(auto& block:pcm_blocks)
                if(!save_pcm(block.data,block.time_ns)) return DirectError::StreamLimit;
    }
    segment.audio_frames=decoded_stream?audio.frames-frames_at_start:segment.audio_packets;
    if(segment.audio_fallback) {
        const auto end=std::remove_if(segment.packets.begin(),segment.packets.end(),[&](const DirectAccessUnit& unit) {
            if(unit.video) return false;
            segment.bytes-=unit.data.size();
            return true;
        });
        segment.packets.erase(end,segment.packets.end());
        segment.audio_packets=0;
        segment.first_pcm_bytes=0;
    }
    if (leading_dts != AV_NOPTS_VALUE) {
        segment.skipped_ns = std::max(int64_t(0), segment.first_video_dts_ns - leading_dts);
        const auto keep = std::remove_if(segment.packets.begin(), segment.packets.end(), [&](const DirectAccessUnit& unit) {
            if (unit.video || unit.pts_ns + unit.duration_ns > first_video_pts) return false;
            segment.bytes -= unit.data.size();
            --segment.audio_packets;
            return true;
        });
        segment.packets.erase(keep, segment.packets.end());
    }
    if(!segment.audio_packets&&!segment.audio_fallback)
        segment.audio_fallback=audio_index<0?"no-audio-track":"no-audio-pcm";
    std::stable_sort(segment.packets.begin(), segment.packets.end(), [](const DirectAccessUnit& a, const DirectAccessUnit& b) {
        return a.dts_ns < b.dts_ns;
    });
    if (segment.packets.back().dts_ns - segment.packets.front().dts_ns > 13 * second) return DirectError::StreamLimit;
    return DirectError::None;
}
