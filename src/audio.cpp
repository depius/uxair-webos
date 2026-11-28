#include "audio.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#ifdef UXAIR_DIRECT_BUILD
#include <libavformat/avformat.h>
#endif
}
#include <cstdio>
#include <cstring>
#include <utility>
namespace { constexpr unsigned adts_aac = 9, mp4_aac = 10; }
AudioDecoder::~AudioDecoder() { reset(); }
void AudioDecoder::reset() {
    swr_free(&resampler_); avcodec_free_context(&context_); codec_ = 0;
}
bool AudioDecoder::configure(unsigned codec, const uint8_t* asc, size_t asc_size, int rate, int channels) {
    reset();
    AVCodecID id = codec == 2 ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
    if (codec != 2 && codec != 4 && codec != 8 && codec != adts_aac && codec != mp4_aac) return false;
    if (codec==mp4_aac && (!asc||asc_size<2||asc_size>64||rate<=0||(channels!=1&&channels!=2))) return false;
    const AVCodec *decoder = avcodec_find_decoder(id);
    if (!decoder || !(context_ = avcodec_alloc_context3(decoder))) return false;
    const uint8_t eld[] = {0xf8,0xe8,0x50,0x00};
    const uint8_t lc[] = {0x12,0x10};
    const uint8_t alac[] = {0,0,0,0x24,0x61,0x6c,0x61,0x63,0,0,0,0,0,0,1,0x60,0,0x10,0x28,0x0a,0x0e,2,0,0xff,0,0,0,0,0,0,0,0,0,0,0xac,0x44};
    const uint8_t *config = codec==2?alac:codec==8?eld:lc;
    size_t length = codec==2?sizeof(alac):codec==8?sizeof(eld):sizeof(lc);
    if (codec==mp4_aac) { config=asc; length=asc_size; }
    if (codec != adts_aac) {
        context_->extradata = static_cast<uint8_t *>(av_mallocz(length + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!context_->extradata) { reset(); return false; }
        memcpy(context_->extradata,config,length); context_->extradata_size=length;
        context_->sample_rate=codec==mp4_aac?rate:44100;
        av_channel_layout_default(&context_->ch_layout,codec==mp4_aac?channels:2);
    }
    if (avcodec_open2(context_,decoder,nullptr)<0) { reset(); return false; }
    codec_=codec;
    input_rate_=rate;input_channels_=channels;
    fprintf(stderr,"[audio] decoder=%s ct=%u\n",decoder->name,codec);
    return true;
}
bool AudioDecoder::decode_adts(const uint8_t *data, size_t size, std::vector<uint8_t>& pcm) {
    return decode_packet(adts_aac, data, size, pcm);
}
bool AudioDecoder::decode_mp4_aac(const uint8_t *data, size_t size, const uint8_t *config, size_t config_size,
                                  int rate, int channels, std::vector<uint8_t>& pcm) {
    if (codec_!=mp4_aac && !configure(mp4_aac,config,config_size,rate,channels)) return false;
    return decode_packet(mp4_aac,data,size,pcm);
}
bool AudioDecoder::decode(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm) {
    if (codec == adts_aac||codec == mp4_aac) { pcm.clear(); return false; }
    return decode_packet(codec, data, size, pcm);
}
bool AudioDecoder::decode_packet(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm) {
    pcm.clear();
    if (!data || !size || size>1048576) return false;
    if (codec==1) {
        if (size%4) return false;
        pcm.assign(data,data+size); return true;
    }
    if (codec_!=codec && !configure(codec)) return false;
    AVPacket *packet=av_packet_alloc(); AVFrame *frame=av_frame_alloc();
    if (!packet || !frame) { av_packet_free(&packet); av_frame_free(&frame); return false; }
    bool ok = av_new_packet(packet,size)>=0;
    if (ok) { memcpy(packet->data,data,size); ok=avcodec_send_packet(context_,packet)>=0; }
    while (ok) {
        int r=avcodec_receive_frame(context_,frame);
        if (r==AVERROR(EAGAIN) || r==AVERROR_EOF) break;
        if (r<0) { ok=false; break; }
        if (codec==mp4_aac && (frame->sample_rate!=input_rate_||frame->ch_layout.nb_channels!=input_channels_)) {
            ok=false;break;
        }
        if (!resampler_) {
            AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler_,&stereo,AV_SAMPLE_FMT_S16,44100,
                &frame->ch_layout,static_cast<AVSampleFormat>(frame->format),frame->sample_rate,0,nullptr)<0 || swr_init(resampler_)<0) {
                ok=false; break;
            }
        }
        int capacity=swr_get_out_samples(resampler_,frame->nb_samples);
        if (capacity<0 || capacity>65536) { ok=false; break; }
        size_t offset=pcm.size(); pcm.resize(offset+capacity*4);
        uint8_t *output=pcm.data()+offset;
        int samples=swr_convert(resampler_,&output,capacity,const_cast<const uint8_t **>(frame->extended_data),frame->nb_samples);
        if (samples<0) { ok=false; break; }
        pcm.resize(offset+samples*4);
        av_frame_unref(frame);
    }
    av_frame_free(&frame); av_packet_free(&packet);
    return ok;
}

#ifdef UXAIR_DIRECT_BUILD
HlsAudioDecoder::~HlsAudioDecoder() { reset(); }

void HlsAudioDecoder::reset() {
    swr_free(&resampler_);
    avcodec_free_context(&context_);
    clock_started_=false;
    origin_=input_samples_=output_samples_=0;
    rate=channels=0;
    frames=0;
    format_=-1;
}

const char* HlsAudioDecoder::decode(const AVStream& stream,const AVPacket* packet,std::vector<HlsPcm>& blocks) {
    blocks.clear();
    const auto& parameters=*stream.codecpar;
    if(!context_) {
        if(!packet) return nullptr;
        const auto* codec=avcodec_find_decoder(parameters.codec_id);
        if(!codec) return "decoder-unavailable";
        context_=avcodec_alloc_context3(codec);
        if(!context_ || avcodec_parameters_to_context(context_,&parameters)<0) return "decoder-init";
        context_->pkt_timebase=stream.time_base;
        context_->max_samples=max_frame_samples*8;
        context_->err_recognition=AV_EF_CRCCHECK|AV_EF_EXPLODE;
        if(avcodec_open2(context_,codec,nullptr)<0) return "decoder-init";
    }
    if(context_->codec_id!=parameters.codec_id || av_cmp_q(context_->pkt_timebase,stream.time_base))
        return "audio-track-change";
    if(avcodec_send_packet(context_,packet)<0) return "decode-error";
    AVFrame* frame=av_frame_alloc();
    if(!frame) return "frame-allocation";
    constexpr AVRational nanoseconds={1,1000000000};
    constexpr AVRational pcm_time={1,44100};
    const auto convert=[&](const uint8_t** data,int samples) {
        const int capacity=swr_get_out_samples(resampler_,samples);
        if(capacity<0 || capacity>16384 || blocks.size()>=16) return false;
        if(!capacity) return true;
        HlsPcm block;
        block.time_ns=origin_+av_rescale_q(output_samples_,pcm_time,nanoseconds);
        block.data.resize(size_t(capacity)*4);
        uint8_t* output=block.data.data();
        const int count=swr_convert(resampler_,&output,capacity,data,samples);
        if(count<0) return false;
        if(count) {
            output_samples_+=count;
            block.data.resize(size_t(count)*4);
            blocks.push_back(std::move(block));
        }
        return true;
    };
    const char* error=nullptr;
    int64_t previous_frame_pts=AV_NOPTS_VALUE;
    for (;;) {
        const int result=avcodec_receive_frame(context_,frame);
        if(result==AVERROR(EAGAIN)||result==AVERROR_EOF) break;
        if(result<0 || (frame->flags&AV_FRAME_FLAG_CORRUPT)) {error="decode-error";break;}
        auto& layout=frame->ch_layout;
        const int incoming_rate=frame->sample_rate;
        if(frame->nb_samples<=0 || frame->nb_samples>max_frame_samples || incoming_rate<8000 || incoming_rate>96000 ||
           layout.nb_channels<1 || layout.nb_channels>8 || !av_channel_layout_check(&layout)) {
            error="unsupported-audio-layout";break;
        }
        if(layout.order==AV_CHANNEL_ORDER_UNSPEC) {
            if(layout.nb_channels>2) {error="unsupported-audio-layout";break;}
            av_channel_layout_default(&layout,layout.nb_channels);
        }
        if(layout.order!=AV_CHANNEL_ORDER_NATIVE) {error="unsupported-audio-layout";break;}
        const AVRational source_time={1,incoming_rate};
        const int64_t pts=frame->pts==AV_NOPTS_VALUE?AV_NOPTS_VALUE:
            av_rescale_q(frame->pts,stream.time_base,nanoseconds);
        if(pts!=AV_NOPTS_VALUE && (pts< -1000000000LL || pts>172800000000000LL)) {
            error="audio-timestamp";break;
        }
        if(!clock_started_) {
            if(pts==AV_NOPTS_VALUE) {av_frame_unref(frame);continue;}
            origin_=pts;clock_started_=true;
        } else if(pts!=AV_NOPTS_VALUE && pts!=previous_frame_pts) {
            const auto expected=origin_+av_rescale_q(input_samples_,source_time,nanoseconds);
            // Missing timestamps after the first anchor use the exact sample
            // count. A larger jump needs an HLS discontinuity and a new clock.
            if(pts<expected-5000000 || pts>expected+5000000) {error="audio-timestamp";break;}
        }
        if(!resampler_) {
            AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
            if(swr_alloc_set_opts2(&resampler_,&stereo,AV_SAMPLE_FMT_S16,44100,&layout,
                                  static_cast<AVSampleFormat>(frame->format),incoming_rate,0,nullptr)<0 ||
               !resampler_ || swr_init(resampler_)<0) {
                error="resampler-init";break;
            }
            rate=incoming_rate;channels=layout.nb_channels;format_=frame->format;channel_mask_=layout.u.mask;
        } else if(incoming_rate!=rate || layout.u.mask!=channel_mask_ || frame->format!=format_) {
            error="audio-track-change";break;
        }
        if(!convert(const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples)) {
            error="resampler-convert";break;
        }
        input_samples_+=frame->nb_samples;
        previous_frame_pts=pts;
        ++frames;
        av_frame_unref(frame);
    }
    if(!packet && !error && resampler_ && !convert(nullptr,0)) error="resampler-drain";
    av_frame_free(&frame);
    return error;
}
#endif
